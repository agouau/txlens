"""Test device metrics with deterministic ESP-IDF stubs. Run in a VS x64 prompt."""
from pathlib import Path
import subprocess
import tempfile


PLATFORM = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "router.h"
#define ESP_OK 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define ESP_PARTITION_TYPE_ANY 0
#define ESP_PARTITION_SUBTYPE_ANY 0
typedef uint64_t configRUN_TIME_COUNTER_TYPE;
static int64_t fake_now = 1000000;
static uint64_t fake_idle[2];
static bool online = true, psram = true;
static int metadata_reads;
static int64_t esp_timer_get_time(void) { return fake_now; }
static uint64_t ulTaskGetIdleRunTimeCounterForCore(int core) { return fake_idle[core]; }
static size_t heap_caps_get_total_size(uint32_t caps) {
    return caps == MALLOC_CAP_SPIRAM ? (psram ? 8 * 1048576 : 0) : 300 * 1024;
}
static size_t heap_caps_get_free_size(uint32_t caps) {
    return caps == MALLOC_CAP_SPIRAM ? (psram ? 6 * 1048576 : 0) : 200 * 1024;
}
static int esp_flash_get_size(void *chip, uint32_t *size) { (void)chip; *size = 16 * 1048576; return 0; }
typedef struct { uint32_t address, size; } esp_partition_t;
typedef struct { uint32_t offset, size; } esp_partition_pos_t;
typedef struct { uint32_t image_len; } esp_image_metadata_t;
typedef const esp_partition_t *esp_partition_iterator_t;
static const esp_partition_t partition = {65536, 1500 * 1024};
static esp_partition_iterator_t esp_partition_find(int type, int sub, const char *name) {
    (void)type; (void)sub; (void)name; return &partition;
}
static const esp_partition_t *esp_partition_get(esp_partition_iterator_t it) { return it; }
static esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t it) { (void)it; return NULL; }
static const esp_partition_t *esp_ota_get_running_partition(void) { return &partition; }
static int esp_image_get_metadata(const esp_partition_pos_t *pos, esp_image_metadata_t *meta) {
    assert(pos->size == partition.size); ++metadata_reads; meta->image_len = 1000 * 1024; return 0;
}
router_status_t router_get_status(void) {
    router_status_t status = {.ap_ip = "192.168.50.1", .clients = 1};
    if (online) strcpy(status.sta_ip, "192.168.1.20");
    return status;
}
'''

TEST = r'''
#include "platform.h"
#include "device_status.c"
static const char *fake_clock = "Not synchronized";
static sd_log_status_t fake_sd = SD_LOG_CHECKING;
sd_log_status_t sd_log_get_status(void) { return fake_sd; }
static uint64_t fake_used, fake_total;
bool sd_log_get_space(uint64_t *used_bytes, uint64_t *total_bytes) {
    *used_bytes = fake_used; *total_bytes = fake_total; return fake_total > 0;
}
bool app_clock_is_ready(void) { return strcmp(fake_clock, "Not synchronized") != 0; }
void app_clock_format(char *buffer, size_t size) { snprintf(buffer, size, "%s", fake_clock); }
wifi_settings_t wifi_settings_get(void) { return (wifi_settings_t){.ap_ssid = "Test hotspot"}; }
int main(void) {
    char text[1200];
    device_status_format(text, sizeof(text));
    assert(strstr(text, "Sampling..."));
    assert(strstr(text, "AP: Test hotspot"));
    assert(strstr(text, "Time: Not synchronized"));
    assert(strstr(text, "SD card: Checking...\nUsage: --\nLogging: Paused"));
    fake_sd = SD_LOG_READY;
    fake_used = UINT64_C(5) * 1073741824 / 4;
    fake_total = UINT64_C(32) * 1073741824;
    device_status_format(text, sizeof(text));
    assert(strstr(text, "SD card: Ready\nUsage: 1.25 / 32.00 GB\nLogging: Waiting for time"));
    fake_clock = "2026-11-04 12:34:56 UTC-8";
    device_status_sample();
    fake_now += 1000000; fake_idle[0] += 250000; fake_idle[1] += 750000;
    device_status_sample();
    device_status_format(text, sizeof(text));
    assert(strstr(text, "CPU: C0 75%, C1 25%\n"));
    assert(!strstr(text, "avg") && !strstr(text, "used / total") && !strstr(text, "Partitions"));
    assert(strstr(text, "SD card: Ready\nUsage: 1.25 / 32.00 GB\nLogging: Recording"));
    assert(strstr(text, fake_clock));
    assert(strstr(text, "RAM: 100.0 / 300.0 KB\n"));
    assert(strstr(text, "PSRAM: 2.00 / 8.00 MB\n"));
    assert(strstr(text, "Flash: 1.46 / 16.00 MB\nFirmware:"));
    assert(strstr(text, "Firmware: 1000 / 1500 KB"));
    assert(strstr(text, "STA IP: 192.168.1.20"));
    assert(strstr(text, "AP IP: 192.168.50.1"));
    assert(strstr(text, "Clients: 1"));
    assert(!strstr(text, "Uptime"));
    assert(!strstr(text, "Free min"));
    assert(!strstr(text, "Wi-Fi"));
    assert(!strstr(text, "ESP-IDF"));
    assert(metadata_reads == 1);
    // Counters past the 32-bit microsecond boundary retain the correct interval.
    fake_now += 5000000000LL; fake_idle[0] += 5000000000ULL;
    device_status_sample();
    fake_now += 1000000; fake_idle[0] += 1000000;
    device_status_sample();
    device_status_format(text, sizeof(text));
    assert(strstr(text, "CPU: C0 0%, C1 100%"));
    // Clamp task switch accounting that falls just outside a sampling interval.
    fake_now += 1000000; fake_idle[0] += 1001000;
    device_status_sample();
    online = false; psram = false;
    device_status_format(text, sizeof(text));
    assert(strstr(text, "C0 0%"));
    assert(strstr(text, "PSRAM: 0.00 / 0.00 MB\n"));
    assert(strstr(text, "STA IP: --"));
    assert(strstr(text, "AP IP: 192.168.50.1"));
    char tiny[8];
    device_status_format(tiny, sizeof(tiny));
    assert(tiny[7] == 0);
    assert(metadata_reads == 1);
    fake_sd = SD_LOG_UNAVAILABLE;
    fake_total = 0;
    device_status_format(text, sizeof(text));
    assert(strstr(text, "SD card: Unavailable\nUsage: --\nLogging: Paused"));
    fake_sd = SD_LOG_WRITING;
    fake_total = UINT64_C(64) * 1073741824;
    fake_used = 0;
    device_status_format(text, sizeof(text));
    assert(strstr(text, "SD card: Ready\nUsage: 0.00 / 64.00 GB\nLogging: Writing"));
    fake_sd = SD_LOG_WRITE_ERROR;
    fake_used = fake_total;
    device_status_format(text, sizeof(text));
    assert(strstr(text, "SD card: Ready\nUsage: 64.00 / 64.00 GB\nLogging: Write error"));
    puts("PASS: per-core interval load, long uptime, heap metrics, flash metadata, offline and bounded output");
    return 0;
}
'''


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="device-status-") as directory:
        temp = Path(directory)
        (temp / "platform.h").write_text(PLATFORM, encoding="utf-8")
        for name in ("esp_flash.h", "esp_heap_caps.h", "esp_image_format.h",
                     "esp_ota_ops.h", "esp_partition.h", "esp_timer.h",
                     "freertos/FreeRTOS.h", "freertos/task.h"):
            path = temp / name
            path.parent.mkdir(exist_ok=True)
            path.write_text('#include "platform.h"\n', encoding="utf-8")
        (temp / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n", encoding="utf-8")
        (temp / "test.c").write_text(TEST, encoding="utf-8")
        exe = temp / "test.exe"
        subprocess.run(["cl", "/nologo", "/std:c11", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
                        f"/I{temp}", f"/I{root / 'main'}", str(temp / "test.c"),
                        f"/Fo{temp / 'test.obj'}", f"/Fe{exe}", "/link", "/INCREMENTAL:NO"], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
