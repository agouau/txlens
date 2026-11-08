"""Test the SD worker with deterministic card, clock and RTOS stubs."""
from pathlib import Path
import subprocess
import tempfile

PLATFORM = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include <sys/stat.h>
typedef int esp_err_t;
typedef struct netif esp_netif_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_NO_MEM 2
#define MALLOC_CAP_SPIRAM 0
#define SPI2_HOST 2
#define SPI_DMA_CH_AUTO 0
#define SDSPI_SLOT_NO_CS -1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(...) ((void)TAG)
#define ESP_LOGW(...) ((void)TAG)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) assert(++*(lock) == 1)
#define portEXIT_CRITICAL(lock) assert(--*(lock) == 0)
static const char *esp_err_to_name(int error) { return "test"; }
static void *heap_caps_calloc(size_t n, size_t s, int caps) { return calloc(n, s); }
#define heap_caps_free free
static int esp_netif_tcpip_exec(esp_err_t (*fn)(void *), void *arg) { return fn(arg); }
static int64_t fake_tick;
static int64_t esp_timer_get_time(void) { return fake_tick; }
static time_t fake_now = 1793491200 + 119;
static time_t fake_time(time_t *out) { return fake_now; }
static struct tm *fake_gmtime(const time_t *now, struct tm *out) {
    return gmtime_s(out, now) == 0 ? out : NULL;
}
#define time fake_time
#define gmtime_r fake_gmtime
static int fake_mkdir(const char *path, int mode) { return 0; }
#define mkdir fake_mkdir
static bool fail_open;
static unsigned file_opens;
static FILE *fake_fopen(const char *path, const char *mode) {
    assert(strstr(path, ".csv")); ++file_opens;
    return fail_open ? NULL : fopen("minute.csv", "w");
}
#define fopen fake_fopen
static int fake_remove(const char *path) { return remove("minute.csv"); }
#define remove fake_remove
typedef int spi_device_handle_t;
typedef struct { int clock_speed_hz, spics_io_num, queue_size; } spi_device_interface_config_t;
typedef struct { size_t length; const void *tx_buffer; } spi_transaction_t;
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
static int spi_bus_add_device(int bus, const spi_device_interface_config_t *cfg, int *dev) { *dev = 1; return 0; }
static int spi_device_transmit(int dev, spi_transaction_t *t) { assert(t->length == 80); return 0; }
static int spi_bus_remove_device(int dev) { return 0; }
static int spi_bus_initialize(int bus, const spi_bus_config_t *cfg, int dma) { return 0; }
static int spi_bus_free(int bus) { return 0; }
typedef struct { int serial; } sdmmc_cid_t;
typedef struct { sdmmc_cid_t cid; } sdmmc_card_t;
typedef struct { int unused; } sdmmc_host_t;
typedef struct { int gpio_cs; } sdspi_device_config_t;
typedef struct { int max_files; bool format_if_mount_failed; } esp_vfs_fat_mount_config_t;
#define SDSPI_HOST_DEFAULT() {0}
#define SDSPI_DEVICE_CONFIG_DEFAULT() {0}
#define VFS_FAT_MOUNT_DEFAULT_CONFIG() {0}
static bool present, selected, mounted, fail_unmount;
static unsigned mounts, unmounts;
static sdmmc_card_t fake_card;
static int esp_vfs_fat_sdspi_mount(const char *root, const sdmmc_host_t *host,
                                 const sdspi_device_config_t *slot,
                                 const esp_vfs_fat_mount_config_t *cfg, sdmmc_card_t **card) {
    assert(selected && !mounted && !cfg->format_if_mount_failed);
    assert(slot->gpio_cs == -1); ++mounts;
    if (!present) return ESP_FAIL;
    mounted = true; *card = &fake_card; return 0;
}
static int esp_vfs_fat_sdcard_unmount(const char *root, sdmmc_card_t *card) {
    assert(mounted && card == &fake_card); mounted = false; ++unmounts;
    return fail_unmount ? ESP_FAIL : ESP_OK;
}
static void (*worker)(void *);
static void *worker_arg;
static int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int priority, void *handle) {
    worker = fn; worker_arg = arg; return pdPASS;
}
static unsigned delays, stop_after;
static bool advance_clock;
static jmp_buf stop;
static void vTaskDelay(int ticks) {
    assert(ticks == 200); fake_tick += 200000;
    if (advance_clock && fake_tick % 1000000 == 0) ++fake_now;
    if (++delays == stop_after) longjmp(stop, 1);
}
'''

TEST = r'''
#include "platform.h"
#include "sd_log.c"
static bool clock_ready;
static unsigned cache_loads, cache_saves, cache_restores, cache_snapshots;
static int cache_error;
static unsigned prune_calls;
static bool fail_prune;
static bool fail_space_query;
static uint64_t fake_total = UINT64_C(32) * 1073741824;
static uint64_t fake_free = UINT64_C(24) * 1073741824;
static bool verify_prune_interval;
static int64_t last_prune_tick = -1;
bool sd_log_prune(const char *root, uint64_t min_free_bytes,
                  uint64_t *total_bytes, uint64_t *free_bytes) {
    assert(mounted && strcmp(root, "/sdcard") == 0);
    assert(min_free_bytes == UINT64_C(100) * 1024 * 1024);
    if (verify_prune_interval) {
        assert(last_prune_tick < 0 || fake_tick - last_prune_tick >= 60000000);
        last_prune_tick = fake_tick;
    }
    ++prune_calls;
    *total_bytes = fail_space_query ? 0 : fake_total;
    *free_bytes = fail_space_query ? 0 : fake_free;
    return !fail_prune && !fail_space_query;
}
int ip_cache_db_load(const char *path, ip_cache_t *snapshot) {
    assert(mounted && strstr(path, "ip_cache.sqlite")); ++cache_loads; return cache_error;
}
int ip_cache_db_save(const char *path, const ip_cache_t *snapshot) {
    assert(mounted && strstr(path, "ip_cache.sqlite")); ++cache_saves; return cache_error;
}
void ip_info_cache_snapshot(ip_cache_t *snapshot) { ++cache_snapshots; }
void ip_info_cache_restore(const ip_cache_t *snapshot) { ++cache_restores; }
static traffic_tx_observer_t observer;
bool app_clock_is_ready(void) { return clock_ready; }
bool ip_info_name(uint32_t ip, char name[256]) { return false; }
esp_err_t display_sd_select(bool on) { selected = on; return 0; }
esp_err_t traffic_meter_set_observer(traffic_tx_observer_t fn) { observer = fn; return 0; }
static void run(unsigned loops) {
    delays = 0; stop_after = loops;
    if (setjmp(stop) == 0) worker(worker_arg);
}
int main(void) {
    uint64_t used, total;
    assert(!sd_log_get_space(&used, &total) && used == 0 && total == 0);
    assert(sd_log_get_status() == SD_LOG_CHECKING);
    assert(sd_log_init() == ESP_OK && observer);
    observer(1, 100);
    assert(s_minutes[1]->count == 0); // No clock, no data.
    run(26); // First probe, then another after five seconds, before time sync.
    assert(mounts == 2 && file_opens == 0 && !mounted && !selected);
    assert(sd_log_get_status() == SD_LOG_UNAVAILABLE);
    present = true; check_card();
    assert(sd_log_get_status() == SD_LOG_READY && !mounted && !selected);
    assert(unmounts == 1 && file_opens == 0);
    assert(cache_loads == 1 && cache_restores == 1 && cache_saves == 0);
    assert(sd_log_get_space(&used, &total));
    assert(total == fake_total && used == fake_total - fake_free);
    fake_free -= 1048576;
    check_card();
    assert(cache_loads == 1); // Do not repeatedly import stale disk entries.
    assert(sd_log_get_space(&used, &total) && used == fake_total - fake_free - 1048576);
    fake_tick += 60000000; check_card(); // Save even before clock sync.
    assert(cache_saves == 1 && cache_snapshots == 1 && file_opens == 0);
    assert(sd_log_get_space(&used, &total) && used == fake_total - fake_free);
    fake_tick += 60000000; cache_error = 10; check_card();
    assert(sd_log_get_status() == SD_LOG_WRITE_ERROR && cache_saves == 2);
    cache_error = 0; check_card();
    assert(cache_saves == 3 && sd_log_get_status() == SD_LOG_READY && prune_calls > 0);
    clock_ready = true;
    unsigned previous_prunes = prune_calls;
    check_card();
    assert(prune_calls == previous_prunes); // Frequent card probes do not query space again.
    observer(1, 123);
    ++fake_now; observer(2, 456); // New minute arrives before worker takes old one.
    traffic_minute_t *output = worker_arg;
    traffic_minute_reset(output, fake_now / 60 - 1);
    assert(take_minute(&output) == ESP_OK);
    assert(output->minute == fake_now / 60 - 1 && output->count == 1);
    assert(output->rows[0].ip == 1 && output->rows[0].bytes[59] == 123);
    assert(s_minutes[0]->rows[0].bytes[0] == 456);
    previous_prunes = prune_calls;
    write_minute(output);
    assert(sd_log_get_status() == SD_LOG_READY && file_opens == 1);
    assert(prune_calls == previous_prunes); // CSV writes never query free space.
    fail_open = true; write_minute(output);
    assert(sd_log_get_status() == SD_LOG_WRITE_ERROR);
    fake_now += 60;
    check_card(); // Mount success alone must not hide a failed write.
    assert(sd_log_get_status() == SD_LOG_WRITE_ERROR);
    fail_open = false; write_minute(output);
    assert(sd_log_get_status() == SD_LOG_READY);
    present = false; check_card();
    assert(sd_log_get_status() == SD_LOG_UNAVAILABLE);
    assert(!sd_log_get_space(&used, &total) && used == 0 && total == 0);
    present = true; check_card();
    assert(sd_log_get_status() == SD_LOG_READY && !mounted && !selected);
    assert(sd_log_get_space(&used, &total) && total == fake_total);
    assert(cache_loads == 2 && cache_restores == 2);
    fake_total *= 2;
    previous_prunes = prune_calls;
    ++fake_card.cid.serial; check_card(); // Card swap between probes.
    assert(cache_loads == 3 && cache_restores == 3);
    assert(prune_calls == previous_prunes + 1);
    assert(sd_log_get_space(&used, &total) && total == fake_total);
    present = false; check_card();
    present = true; cache_error = 10; check_card();
    assert(cache_loads == 4 && cache_restores == 3 && sd_log_get_status() == SD_LOG_WRITE_ERROR);
    cache_error = 0; check_card();
    assert(cache_loads == 5 && cache_restores == 4);
    fake_now += 60; fake_tick += 60000000; fail_prune = true; check_card();
    assert(sd_log_get_status() == SD_LOG_WRITE_ERROR);
    assert(sd_log_get_space(&used, &total)); // Cleanup errors can still have a valid reading.
    unsigned failed_calls = prune_calls;
    fake_tick += 5000000; check_card();
    assert(prune_calls == failed_calls); // Failed space checks also wait a minute to retry.
    unsigned previous_opens = file_opens, previous_saves = cache_saves;
    previous_prunes = prune_calls;
    fake_tick += 60000000; write_minute(output);
    assert(file_opens == previous_opens + 1 && cache_saves == previous_saves + 1);
    assert(prune_calls == previous_prunes); // Cleanup failure does not add a pre-write check.
    fail_prune = false; check_card();
    assert(prune_calls == failed_calls + 1);
    check_card();
    assert(prune_calls == failed_calls + 1);
    fake_tick += 60000000; fail_space_query = true; check_card();
    assert(!sd_log_get_space(&used, &total) && total == 0);
    fail_space_query = false;
    previous_prunes = prune_calls;
    check_card();
    assert(prune_calls == previous_prunes && !sd_log_get_space(&used, &total));
    fake_tick += 60000000; check_card();
    assert(sd_log_get_space(&used, &total) && total == fake_total);
    fail_unmount = true; check_card();
    assert(sd_log_get_status() == SD_LOG_UNAVAILABLE && !sd_log_get_space(&used, &total));
    fail_unmount = false; check_card();
    assert(sd_log_get_space(&used, &total));
    present = false; write_minute(output);
    assert(sd_log_get_status() == SD_LOG_UNAVAILABLE && !sd_log_get_space(&used, &total));
    present = true; check_card();
    assert(sd_log_get_space(&used, &total));
    // Keep running across two minute writes: checks neither repeat on writes
    // nor get postponed indefinitely by regular traffic.
    traffic_minute_t *allocated[] = {s_minutes[0], s_minutes[1], output};
    worker_arg = output;
    fake_tick = 1000000000;
    fake_now = fake_now / 60 * 60;
    advance_clock = verify_prune_interval = true;
    previous_prunes = prune_calls;
    previous_opens = file_opens;
    unsigned previous_mounts = mounts;
    run(611);
    assert(prune_calls == previous_prunes + 3);
    assert(file_opens == previous_opens + 2);
    assert(mounts == previous_mounts + 25 + 2); // 25 status probes and two CSV writes; no extra space mounts.
    assert(!mounted && !selected);
    for (size_t i = 0; i < 3; ++i) free(allocated[i]);
    free(s_cache_snapshot.entries);
    puts("PASS: CSV handoff, card probes/swaps, cache saves/restores, cached 64-bit usage, 5-second status/60-second space checks, errors/retry");
    return 0;
}
'''


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="sd-log-") as directory:
        work = Path(directory)
        (work / "platform.h").write_text(PLATFORM, encoding="utf-8")
        (work / "stdatomic.h").write_text(
            "#pragma once\ntypedef int atomic_int;\n"
            "#define atomic_load(p) (*(p))\n#define atomic_store(p,v) (*(p)=(v))\n",
            encoding="utf-8")
        for name in ("esp_err.h", "esp_netif.h", "esp_heap_caps.h", "esp_log.h",
                     "esp_timer.h", "esp_vfs_fat.h", "driver/spi_master.h",
                     "driver/sdspi_host.h", "freertos/FreeRTOS.h", "freertos/task.h"):
            path = work / name
            path.parent.mkdir(exist_ok=True)
            path.write_text('#include "platform.h"\n', encoding="utf-8")
        source = work / "test.c"
        source.write_text(TEST, encoding="utf-8")
        exe = work / "test.exe"
        subprocess.run([
            "cl", "/nologo", "/std:c11", "/W4", "/WX", "/wd4100", "/D_CRT_SECURE_NO_WARNINGS",
            f"/I{work}", f"/I{root / 'main'}", str(source),
            str(root / "main/traffic_minute.c"), str(root / "main/traffic_csv.c"),
            f"/Fo{work}\\", f"/Fe{exe}",
        ], cwd=work, check=True)
        subprocess.run([str(exe)], cwd=work, check=True)


if __name__ == "__main__":
    main()
