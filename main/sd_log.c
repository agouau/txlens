#include "sd_log.h"
#include "sd_log_retention.h"
#include "app_clock.h"
#include "app_config.h"
#include "display.h"
#include "ip_info.h"
#include "ip_cache_db.h"
#include "traffic_meter.h"
#include "traffic_minute.h"
#include "traffic_csv.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SD_ROOT "/sdcard"
static const char *TAG = "sd_log";
// Only lwIP touches these two pointers and their contents. The writer owns a
// third buffer, exchanged for a completed minute without copying on lwIP.
static traffic_minute_t *s_minutes[2];
static atomic_int s_status = SD_LOG_CHECKING;
static ip_cache_t s_cache_snapshot;
static bool s_cache_loaded;
static sdmmc_cid_t s_card_id;
static int64_t s_next_cache_save;
static int64_t s_next_space_check;
static portMUX_TYPE s_space_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_total_bytes, s_free_bytes;

_Static_assert(SD_LOG_MIN_FREE_MIB > 0, "SD_LOG_MIN_FREE_MIB must be positive");

sd_log_status_t sd_log_get_status(void)
{
    return (sd_log_status_t)atomic_load(&s_status);
}

bool sd_log_get_space(uint64_t *used_bytes, uint64_t *total_bytes)
{
    portENTER_CRITICAL(&s_space_lock);
    uint64_t total = s_total_bytes, available = s_free_bytes;
    portEXIT_CRITICAL(&s_space_lock);
    *total_bytes = total;
    *used_bytes = total >= available ? total - available : 0;
    return total > 0 && available <= total;
}

static void set_space(uint64_t total_bytes, uint64_t free_bytes)
{
    portENTER_CRITICAL(&s_space_lock);
    s_total_bytes = total_bytes;
    s_free_bytes = free_bytes;
    portEXIT_CRITICAL(&s_space_lock);
}

static void clear_space(void)
{
    set_space(0, 0);
    s_next_space_check = 0;
}

static void record_tx(uint32_t ip, uint16_t bytes)
{
    if (!app_clock_is_ready()) return;
    time_t now = time(NULL);
    if (now < 0) return;
    traffic_minute_record(s_minutes[(now / 60) % 2], now, ip, bytes);
}

static esp_err_t take_minute(void *arg)
{
    traffic_minute_t **buffer = arg;
    time_t minute = (*buffer)->minute;
    size_t slot = minute % 2;
    if (s_minutes[slot]->minute != minute) return ESP_OK;
    traffic_minute_t *done = s_minutes[slot];
    s_minutes[slot] = *buffer;
    traffic_minute_reset(s_minutes[slot], -1);
    *buffer = done;
    return ESP_OK;
}

static esp_err_t mount_card(sdmmc_card_t **card)
{
    // Supply the required startup clocks with CS high before putting the card
    // in SPI mode. This bus has only the SD card; EXIO4 can then stay low until
    // unmount, with no native GPIO chip select required by the SDSPI driver.
    esp_err_t error = display_sd_select(false);
    if (error != ESP_OK) return error;
    spi_device_interface_config_t config = {
        .clock_speed_hz = 400000,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    spi_device_handle_t device;
    error = spi_bus_add_device(SPI2_HOST, &config, &device);
    if (error != ESP_OK) return error;
    uint8_t clocks[10];
    memset(clocks, 0xff, sizeof(clocks));
    spi_transaction_t transaction = {.length = sizeof(clocks) * 8, .tx_buffer = clocks};
    error = spi_device_transmit(device, &transaction);
    esp_err_t remove_error = spi_bus_remove_device(device);
    if (error != ESP_OK) return error;
    if (remove_error != ESP_OK) return remove_error;
    error = display_sd_select(true);
    if (error != ESP_OK) return error;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SDSPI_SLOT_NO_CS;
    esp_vfs_fat_mount_config_t mount = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mount.max_files = 3; // SQLite database + on-card rollback journal.
    mount.format_if_mount_failed = false;
    return esp_vfs_fat_sdspi_mount(SD_ROOT, &host, &slot, &mount, card);
}

static bool minute_path(time_t minute, char path[80])
{
    time_t local = minute * 60 + (int)(APP_CLOCK_UTC_OFFSET_HOURS * 60) * 60;
    struct tm date;
    if (gmtime_r(&local, &date) == NULL) return false;
    const int parts[] = {date.tm_year + 1900, date.tm_mon + 1, date.tm_mday, date.tm_hour};
    size_t used = strlen(SD_ROOT);
    memcpy(path, SD_ROOT, used + 1);
    for (size_t i = 0; i < 4; ++i) {
        int length = snprintf(path + used, 80 - used, i == 0 ? "/%04d" : "/%02d", parts[i]);
        if (length < 0 || (size_t)length >= 80 - used) return false;
        used += length;
        if (mkdir(path, 0775) != 0 && errno != EEXIST) return false;
    }
    int length = snprintf(path + used, 80 - used, "/%02d.csv", date.tm_min);
    return length > 0 && (size_t)length < 80 - used;
}

static bool unmount_card(sdmmc_card_t *card)
{
    esp_err_t error = esp_vfs_fat_sdcard_unmount(SD_ROOT, card);
    if (error != ESP_OK) ESP_LOGW(TAG, "SD unmount: %s", esp_err_to_name(error));
    esp_err_t deselect_error = display_sd_select(false);
    if (deselect_error != ESP_OK)
        ESP_LOGW(TAG, "SD deselect: %s", esp_err_to_name(deselect_error));
    if (error != ESP_OK || deselect_error != ESP_OK) clear_space();
    return error == ESP_OK && deselect_error == ESP_OK;
}

static bool sync_cache(const sdmmc_card_t *card)
{
    // Detect a different card even when it was swapped between probes.
    if (memcmp(&s_card_id, &card->cid, sizeof(s_card_id)) != 0) {
        s_cache_loaded = false;
        s_card_id = card->cid;
        clear_space();
    }
    int64_t now = esp_timer_get_time();
    int rc;
    if (!s_cache_loaded) {
        rc = ip_cache_db_load(SD_ROOT "/ip_cache.sqlite", &s_cache_snapshot);
        if (rc != 0) goto fail;
        ip_info_cache_restore(&s_cache_snapshot);
        s_cache_loaded = true;
        s_next_cache_save = now + SD_LOG_CACHE_SAVE_MS * 1000LL;
    }
    if (now < s_next_cache_save) return true;
    atomic_store(&s_status, SD_LOG_WRITING);
    ip_info_cache_snapshot(&s_cache_snapshot);
    rc = ip_cache_db_save(SD_ROOT "/ip_cache.sqlite", &s_cache_snapshot);
    if (rc != 0) goto fail;
    s_next_cache_save = esp_timer_get_time() + SD_LOG_CACHE_SAVE_MS * 1000LL;
    return true;
fail:
    ESP_LOGW(TAG, "IP cache database: SQLite error %d; will retry", rc);
    return false;
}

static void check_card(void)
{
    sdmmc_card_t *card = NULL;
    if (mount_card(&card) != ESP_OK) {
        s_cache_loaded = false;
        clear_space();
        display_sd_select(false);
        atomic_store(&s_status, SD_LOG_UNAVAILABLE);
        return;
    }
    bool success = sync_cache(card);
    if (esp_timer_get_time() >= s_next_space_check) {
        sd_log_status_t previous = sd_log_get_status();
        atomic_store(&s_status, SD_LOG_WRITING);
        uint64_t total_bytes, free_bytes;
        bool space_ok = sd_log_prune(SD_ROOT, (uint64_t)SD_LOG_MIN_FREE_MIB * 1024 * 1024,
                                     &total_bytes, &free_bytes);
        set_space(total_bytes, free_bytes);
        atomic_store(&s_status, previous);
        s_next_space_check = esp_timer_get_time() + SD_LOG_SPACE_CHECK_MS * 1000LL;
        if (!space_ok) ESP_LOGW(TAG, "Cannot free enough SD space; will retry at the next space check");
        success = success && space_ok;
    }
    if (!unmount_card(card)) {
        s_cache_loaded = false;
        atomic_store(&s_status, SD_LOG_UNAVAILABLE);
    }
    else if (!success) atomic_store(&s_status, SD_LOG_WRITE_ERROR);
    // A successful mount cannot prove a previous write error has cleared.
    else if (sd_log_get_status() != SD_LOG_WRITE_ERROR)
        atomic_store(&s_status, SD_LOG_READY);
}

static void write_minute(const traffic_minute_t *data)
{
    sdmmc_card_t *card = NULL;
    esp_err_t error = mount_card(&card);
    if (error != ESP_OK) {
        s_cache_loaded = false;
        clear_space();
        ESP_LOGW(TAG, "Minute skipped: SD unavailable (%s)", esp_err_to_name(error));
        display_sd_select(false);
        atomic_store(&s_status, SD_LOG_UNAVAILABLE);
        return;
    }
    char path[80] = {0};
    bool success = false;
    atomic_store(&s_status, SD_LOG_WRITING);
    bool cache_ok = sync_cache(card);
    if (minute_path(data->minute, path)) {
        // Exclusive creation preserves an existing file after a restart.
        FILE *file = fopen(path, "wx");
        if (file != NULL) {
            success = traffic_csv_write(file, data, ip_info_name);
            if (fclose(file) != 0) success = false;
            if (!success) remove(path); // Do not leave a knowingly partial CSV.
        }
    }
    if (success) ESP_LOGI(TAG, "Saved %s (%u IPs)", path, (unsigned)data->count);
    else ESP_LOGW(TAG, "Minute skipped: cannot create/write %s", path);
    if (!unmount_card(card)) {
        s_cache_loaded = false;
        success = false;
    }
    atomic_store(&s_status, success && cache_ok ? SD_LOG_READY : SD_LOG_WRITE_ERROR);
    if (data->dropped_bytes)
        ESP_LOGW(TAG, "Minute IP capacity reached: dropped %" PRIu64 " bytes", data->dropped_bytes);
}

static void log_task(void *arg)
{
    traffic_minute_t *buffer = arg;
    time_t current = -1;
    int64_t next_card_check = 0;
    while (true) {
        if (app_clock_is_ready()) {
            time_t now = time(NULL) / 60;
            if (current != -1 && now > current) {
                traffic_minute_reset(buffer, now - 1);
                if (esp_netif_tcpip_exec(take_minute, &buffer) == ESP_OK) {
                    write_minute(buffer);
                }
            }
            current = now;
        }
        // Reuse the card probe's mount for the slower space check.
        // CSV writes do not change either schedule.
        int64_t tick = esp_timer_get_time();
        if (tick >= next_card_check) {
            check_card();
            next_card_check = esp_timer_get_time() + SD_LOG_CARD_CHECK_MS * 1000LL;
        }
        vTaskDelay(pdMS_TO_TICKS(SD_LOG_POLL_MS));
    }
}

esp_err_t sd_log_init(void)
{
    s_cache_snapshot.count = IP_INFO_CACHE_COUNT;
    s_cache_snapshot.entries = heap_caps_calloc(IP_INFO_CACHE_COUNT,
                                               sizeof(*s_cache_snapshot.entries), MALLOC_CAP_SPIRAM);
    if (!s_cache_snapshot.entries) return ESP_ERR_NO_MEM;
    traffic_minute_t *buffers[3] = {0};
    for (size_t i = 0; i < 3; ++i) {
        buffers[i] = heap_caps_calloc(1, sizeof(*buffers[i]), MALLOC_CAP_SPIRAM);
        if (buffers[i] == NULL) {
            for (size_t j = 0; j < i; ++j) heap_caps_free(buffers[j]);
            heap_caps_free(s_cache_snapshot.entries);
            s_cache_snapshot.entries = NULL;
            return ESP_ERR_NO_MEM;
        }
        traffic_minute_reset(buffers[i], -1);
    }
    const spi_bus_config_t bus = {
        .mosi_io_num = SD_MOSI_GPIO, .miso_io_num = SD_MISO_GPIO, .sclk_io_num = SD_SCLK_GPIO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t error = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) goto fail;
    s_minutes[0] = buffers[0];
    s_minutes[1] = buffers[1];
    error = traffic_meter_set_observer(record_tx);
    if (error == ESP_OK && xTaskCreate(log_task, "sd_log", 12288, buffers[2], 2, NULL) == pdPASS)
        return ESP_OK;
    traffic_meter_set_observer(NULL);
    spi_bus_free(SPI2_HOST);
    if (error == ESP_OK) error = ESP_ERR_NO_MEM;
fail:
    heap_caps_free(s_cache_snapshot.entries);
    s_cache_snapshot.entries = NULL;
    for (size_t i = 0; i < 3; ++i) heap_caps_free(buffers[i]);
    s_minutes[0] = s_minutes[1] = NULL;
    return error;
}
