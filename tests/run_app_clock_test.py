"""Exercise the real startup clock task with deterministic network/RTOS stubs.

Run in a Visual Studio x64 Developer Command Prompt.
"""
from pathlib import Path
import subprocess
import tempfile


PLATFORM = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_TIMEOUT 2
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(...) ((void)TAG)
#define ESP_LOGW(...) ((void)TAG)
static const char *esp_err_to_name(int error) { (void)error; return "timeout"; }
static void (*task_fn)(void *);
static int starts, waits, deinits, delays, deletes, create_ok = 1;
static time_t fake_now = 86400;
static time_t fake_time(time_t *out) { if (out) *out = fake_now; return fake_now; }
static struct tm *fake_gmtime(const time_t *now, struct tm *out) {
    return gmtime_s(out, now) == 0 ? out : NULL;
}
#define time fake_time
#define gmtime_r fake_gmtime
typedef struct { bool start; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) { .start = true }
static int esp_netif_sntp_init(const esp_sntp_config_t *config) {
    assert(!config->start);
    return ESP_OK;
}
static int esp_netif_sntp_start(void) { ++starts; return ESP_OK; }
static int esp_netif_sntp_sync_wait(int ticks) {
    assert(ticks == 10000);
    return ++waits < 3 ? ESP_ERR_TIMEOUT : ESP_OK;
}
static void esp_netif_sntp_deinit(void) { ++deinits; }
static int xTaskCreate(void (*fn)(void *), const char *name, int stack,
                       void *arg, int priority, void *handle) {
    (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    task_fn = fn;
    return create_ok;
}
static void vTaskDelay(int ticks) { assert(ticks == 500); ++delays; }
static void vTaskDelete(void *task) { assert(task == NULL); ++deletes; }
'''

TEST = r'''
#include "platform.h"
#include "router.h"
router_status_t router_get_status(void) {
    // Initially offline, then ready. Later reconnections cannot restart the
    // service because the synchronization task has already deleted itself.
    return (router_status_t){ .state = delays ? ROUTER_READY : ROUTER_CONNECTING };
}
#include "app_clock.c"
int main(void) {
    char text[80];
    app_clock_format(text, sizeof(text));
    assert(strcmp(text, "Not synchronized") == 0);
    assert(app_clock_init() == ESP_OK && !app_clock_is_ready());
    assert(starts == 0);
    task_fn(NULL);
    assert(delays == 1 && starts == 1 && waits == 3);
    assert(deinits == 1 && deletes == 1 && app_clock_is_ready());
    app_clock_format(text, sizeof(text));
    assert(strcmp(text, EXPECTED_TIME) == 0);
    assert(fake_now == 86400); // Display offsets must not alter UTC system time.
    char previous[80];
    strcpy(previous, text);
    fake_now++;
    app_clock_format(text, sizeof(text));
    assert(strcmp(text, previous) != 0);
    assert(starts == 1 && waits == 3 && deinits == 1);
    create_ok = 0;
    assert(app_clock_init() == ESP_ERR_NO_MEM && deinits == 2);
    puts("PASS: offline startup, retry until success, SNTP shutdown, local clock advances, task allocation failure");
    return 0;
}
'''


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="app-clock-") as directory:
        temp = Path(directory)
        (temp / "platform.h").write_text(PLATFORM, encoding="utf-8")
        # Single-thread scheduling is explicit in this test. Firmware uses C11
        # atomics; the host shim only replaces their storage/load operations.
        (temp / "stdatomic.h").write_text(
            "#pragma once\n#include <stdbool.h>\ntypedef bool atomic_bool;\n"
            "#define atomic_load(p) (*(p))\n#define atomic_store(p,v) (*(p)=(v))\n",
            encoding="utf-8")
        for name in ("esp_err.h", "esp_log.h", "esp_netif_sntp.h",
                     "freertos/FreeRTOS.h", "freertos/task.h"):
            path = temp / name
            path.parent.mkdir(exist_ok=True)
            path.write_text('#include "platform.h"\n', encoding="utf-8")
        source = temp / "test.c"
        source.write_text(TEST, encoding="utf-8")
        (temp / "app_clock.c").write_text(
            (root / "main/app_clock.c").read_text(encoding="utf-8"), encoding="utf-8")
        exe = temp / "test.exe"
        cases = (
            (8, "1970-01-02 08:00:00 UTC+8"),
            (0, "1970-01-02 00:00:00 UTC+0"),
            (-8, "1970-01-01 16:00:00 UTC-8"),
            (5.5, "1970-01-02 05:30:00 UTC+5:30"),
            (-3.5, "1970-01-01 20:30:00 UTC-3:30"),
            (5.75, "1970-01-02 05:45:00 UTC+5:45"),
        )
        for offset, expected in cases:
            (temp / "app_config.h").write_text(
                f'#define APP_CLOCK_UTC_OFFSET_HOURS {offset}\n'
                '#define APP_CLOCK_NTP_SERVER "time.google.com"\n'
                f'#define EXPECTED_TIME "{expected}"\n', encoding="utf-8")
            subprocess.run([
                "cl", "/nologo", "/std:c11", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
                f"/I{temp}", f"/I{root / 'main'}", str(source),
                f"/Fo{temp / 'test.obj'}", f"/Fe{exe}", "/link", "/INCREMENTAL:NO",
            ], check=True)
            subprocess.run([str(exe)], check=True)
        print("PASS: UTC offsets 8, 0, -8, 5.5, -3.5, 5.75 and date rollover")


if __name__ == "__main__":
    main()
