#include "device_status.h"

#include <stdio.h>
#include "app_clock.h"
#include "app_config.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "router.h"
#include "wifi_settings.h"
#include "sd_log.h"

static int64_t s_sample_time;
static configRUN_TIME_COUNTER_TYPE s_idle[2];
static double s_cpu[2];
static bool s_cpu_ready;

void device_status_sample(void)
{
    int64_t now = esp_timer_get_time();
    for (int core = 0; core < 2; ++core) {
        configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(core);
        if (s_sample_time != 0) {
            double idle_fraction = (double)(idle - s_idle[core]) / (now - s_sample_time);
            // Task switch accounting can straddle the sample boundary.
            if (idle_fraction < 0) idle_fraction = 0;
            if (idle_fraction > 1) idle_fraction = 1;
            s_cpu[core] = 100 * (1 - idle_fraction);
        }
        s_idle[core] = idle;
    }
    s_cpu_ready = s_sample_time != 0;
    s_sample_time = now;
}

void device_status_format(char *text, size_t size)
{
    // These values cannot change without rebooting. Read image metadata only once.
    static bool flash_read;
    static uint32_t flash_size, reserved, app_size, image_size;
    if (!flash_read) {
        esp_flash_get_size(NULL, &flash_size);
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                       ESP_PARTITION_SUBTYPE_ANY, NULL);
        while (it) {
            reserved += esp_partition_get(it)->size;
            it = esp_partition_next(it);
        }
        const esp_partition_t *app = esp_ota_get_running_partition();
        if (app) {
            app_size = app->size;
            esp_partition_pos_t pos = {.offset = app->address, .size = app->size};
            esp_image_metadata_t metadata;
            if (esp_image_get_metadata(&pos, &metadata) == ESP_OK) image_size = metadata.image_len;
        }
        flash_read = true;
    }

    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t ram_total = heap_caps_get_total_size(internal);
    size_t ram_free = heap_caps_get_free_size(internal);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    router_status_t router = router_get_status();
    wifi_settings_t settings = wifi_settings_get();
    char clock[80];
    app_clock_format(clock, sizeof(clock));
    char cpu[80] = "Sampling...";
    if (s_cpu_ready)
        snprintf(cpu, sizeof(cpu), "C0 %.0f%%, C1 %.0f%%", s_cpu[0], s_cpu[1]);
    char flash[128] = "Unavailable";
    if (flash_size)
        snprintf(flash, sizeof(flash), "%.2f / %.2f MB\nFirmware: %.0f / %.0f KB",
                 reserved / 1048576.0, flash_size / 1048576.0,
                 image_size / 1024.0, app_size / 1024.0);

    sd_log_status_t sd = sd_log_get_status();
    const char *card = sd == SD_LOG_CHECKING ? "Checking..." :
                       sd == SD_LOG_UNAVAILABLE ? "Unavailable" : "Ready";
    const char *logging = "Paused";
    if (sd == SD_LOG_WRITING) logging = "Writing";
    else if (sd == SD_LOG_WRITE_ERROR) logging = "Write error";
    else if (sd == SD_LOG_READY)
        logging = app_clock_is_ready() ? "Recording" : "Waiting for time";
    char usage[80] = "--";
    uint64_t used_bytes, total_bytes;
    if (sd_log_get_space(&used_bytes, &total_bytes))
        snprintf(usage, sizeof(usage), "%.2f / %.2f GB",
                 used_bytes / 1073741824.0, total_bytes / 1073741824.0);

    snprintf(text, size,
        "CPU: %s\n"
        "RAM: %.1f / %.1f KB\n"
        "PSRAM: %.2f / %.2f MB\n"
        "Flash: %s\n\n"
        "SD card: %s\nUsage: %s\nLogging: %s\n\n"
        "STA IP: %s\nAP: %s\nAP IP: %s\nClients: %u / %u\n\n"
        "Time: %s",
        cpu,
        (ram_total - ram_free) / 1024.0, ram_total / 1024.0,
        (psram_total - psram_free) / 1048576.0, psram_total / 1048576.0,
        flash, card, usage, logging, router.sta_ip[0] ? router.sta_ip : "--", settings.ap_ssid,
        router.ap_ip, router.clients, (unsigned)ROUTER_AP_MAX_CONNECTIONS, clock);
}
