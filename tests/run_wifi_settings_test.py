"""Run real Wi-Fi parsing/storage code with deterministic NVS/timer stubs (VS x64 prompt)."""
from pathlib import Path
import subprocess
import tempfile

PLATFORM = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NVS_NOT_FOUND 4
#define ESP_FAIL 5
#define ESP_LOGW(...) ((void)0)
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define portMAX_DELAY 100
typedef int nvs_handle_t;
typedef int SemaphoreHandle_t;
typedef int esp_timer_handle_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
static unsigned char disk[256], pending[256];
static char disk_key[140], pending_key[140];
static char disk_default[64], pending_default[64];
static unsigned random_state, random_calls;
static bool entropy_enabled;
static void bootloader_random_enable(void) { assert(!entropy_enabled); entropy_enabled=true; }
static void bootloader_random_disable(void) { assert(entropy_enabled); entropy_enabled=false; }
static uint32_t esp_random(void) {
    assert(entropy_enabled); ++random_calls;
    return ++random_state * 2654435761U;
}
static size_t disk_size, pending_size;
static bool exists, pending_exists;
static int write_error, commit_error, open_error, timer_starts, restarts;
static void (*restart_callback)(void *);
static int xSemaphoreCreateMutex(void) { return 1; }
static void xSemaphoreTake(int lock, int ticks) { assert(lock == 1 && ticks == portMAX_DELAY); }
static void xSemaphoreGive(int lock) { assert(lock == 1); }
static esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    restart_callback = args->callback; *out = 1; return ESP_OK;
}
static esp_err_t esp_timer_start_once(int handle, uint64_t delay) {
    assert(handle == 1 && delay == 2000000); ++timer_starts; return ESP_OK;
}
static void esp_restart(void) { ++restarts; }
static esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out) {
    assert(!strcmp(ns, "wifi_settings"));
    if (open_error) return open_error;
    if (mode == NVS_READONLY && !exists && !disk_key[0]) return ESP_ERR_NVS_NOT_FOUND;
    *out = 1; memcpy(pending, disk, sizeof(disk)); pending_size = disk_size;
    strcpy(pending_key, disk_key);
    strcpy(pending_default, disk_default);
    pending_exists = exists; return ESP_OK;
}
static esp_err_t nvs_get_blob(int handle, const char *key, void *out, size_t *size) {
    assert(handle == 1 && !strcmp(key, "config"));
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    if (disk_size > *size) return ESP_FAIL;
    memcpy(out, disk, disk_size); *size = disk_size; return ESP_OK;
}
static esp_err_t nvs_set_blob(int handle, const char *key, const void *data, size_t size) {
    assert(handle == 1 && !strcmp(key, "config") && size <= sizeof(pending));
    if (write_error) return write_error;
    memcpy(pending, data, size); pending_size = size; pending_exists = true; return ESP_OK;
}
static esp_err_t nvs_commit(int handle) {
    assert(handle == 1);
    if (commit_error) return commit_error;
    memcpy(disk, pending, sizeof(disk)); disk_size = pending_size; exists = pending_exists;
    strcpy(disk_key, pending_key); strcpy(disk_default,pending_default); return ESP_OK;
}
static esp_err_t nvs_get_str(int handle, const char *key, char *out, size_t *size) {
    assert(handle == 1);
    assert(!strcmp(key, "abuse_key") || !strcmp(key, "ap_default"));
    const char *value = !strcmp(key, "ap_default") ? disk_default : disk_key;
    if (!value[0]) return ESP_ERR_NVS_NOT_FOUND;
    if (strlen(value) + 1 > *size) return ESP_FAIL;
    strcpy(out, value); *size = strlen(value) + 1; return ESP_OK;
}
static esp_err_t nvs_set_str(int handle, const char *key, const char *value) {
    assert(handle == 1);
    assert(!strcmp(key, "abuse_key") || !strcmp(key, "ap_default"));
    if (write_error) return write_error;
    strcpy(!strcmp(key, "ap_default") ? pending_default : pending_key, value); return ESP_OK;
}
static void nvs_close(int handle) { assert(handle == 1); }
'''

TEST = r'''
#include "platform.h"
#include "wifi_settings.c"
static void boot(void) {
    s_saved = false; s_restarting = false;
    assert(wifi_settings_init() == ESP_OK);
}
int main(void) {
    // First-boot failures cannot silently use an ephemeral or shared password.
    write_error=ESP_FAIL;
    assert(wifi_settings_init()==ESP_FAIL && !disk_default[0] && !entropy_enabled);
    write_error=0; commit_error=ESP_FAIL;
    assert(wifi_settings_init()==ESP_FAIL && !disk_default[0] && !entropy_enabled);
    commit_error=0;
    boot();
    wifi_settings_t first=wifi_settings_get();
    assert(strlen(first.ap_password)==8 && wifi_settings_valid(&first));
    assert(!strcmp(first.ap_password,disk_default) && !entropy_enabled);
    unsigned calls=random_calls;
    boot();
    wifi_settings_t second=wifi_settings_get();
    assert(!strcmp(first.ap_password,second.ap_password) && random_calls==calls);
    assert(!wifi_settings_saved());
    assert(!device_settings_configured());
    write_error = ESP_FAIL;
    assert(abuseipdb_key_save("test-api-key") == ESP_FAIL && !device_settings_configured());
    write_error = 0; commit_error = ESP_FAIL;
    assert(abuseipdb_key_save("test-api-key") == ESP_FAIL && !device_settings_configured());
    commit_error = 0;
    assert(abuseipdb_key_save("test-api-key") == ESP_OK);
    assert(device_settings_configured() && !wifi_settings_saved() && timer_starts == 0);
    boot();
    assert(device_settings_configured() && !wifi_settings_saved());
    assert(abuseipdb_key_save("") == ESP_OK && !device_settings_configured());
    boot();
    assert(!device_settings_configured());
    wifi_settings_t defaults = wifi_settings_get();
    assert(!strcmp(defaults.ap_ssid, "TxLensAP") && !defaults.sta_ssid[0] && !defaults.sta_password[0]);
    wifi_settings_t value = defaults;
    assert(wifi_settings_parse("{\"sta_ssid\":\"Upstream\",\"ap_ssid\":\"New AP\"}", &value));
    assert(!strcmp(value.ap_password, defaults.ap_password));
    assert(wifi_settings_parse("{\"sta_ssid\":\"\",\"sta_password\":\"\",\"ap_ssid\":\"New AP\"}", &value));
    const char *invalid[] = {
        "{}", "[]", "null", "{", "{\"sta_ssid\":false,\"ap_ssid\":\"AP\"}",
        "{\"sta_ssid\":\"x\",\"ap_ssid\":\"\"}",
        "{\"sta_ssid\":\"x\",\"ap_ssid\":\"AP\",\"ap_password\":\"short\"}",
        "{\"sta_ssid\":\"x\",\"ap_ssid\":\"AP\",\"sta_password\":\"short\"}",
        "{\"sta_ssid\":\"x\",\"ap_ssid\":\"AP\\n\"}",
        "{\"sta_ssid\":\"x\",\"ap_ssid\":\"AP\\u0000bad\"}",
        "{\"sta_ssid\":\"x\",\"ap_ssid\":\"AP\"} garbage",
        "{\"sta_ssid\":\"123456789012345678901234567890123\",\"ap_ssid\":\"AP\"}",
        "{\"sta_ssid\":\"x\",\"ap_ssid\":\"AP\",\"ap_password\":null}",
        "{\"sta_ssid\":\"x\",\"ap_ssid\":\"AP\",\"extra\":[[[]]]}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        wifi_settings_t before = value;
        assert(!wifi_settings_parse(invalid[i], &value));
        assert(!memcmp(&before, &value, sizeof(value)));
    }
    memset(value.ap_ssid, 's', 32); value.ap_ssid[32] = 0;
    memset(value.ap_password, 'p', 63); value.ap_password[63] = 0;
    assert(wifi_settings_valid(&value));
    memset(value.ap_ssid, 's', sizeof(value.ap_ssid));
    assert(!wifi_settings_valid(&value));
    value = defaults;
    strcpy(value.ap_ssid, "Saved AP");
    strcpy(value.ap_password, "savedpass123");
    strcpy(value.sta_ssid, "Saved upstream");
    // One failed flash write/commit must not restart the device or report success.
    write_error = ESP_FAIL;
    assert(wifi_settings_save(&value) == ESP_FAIL && timer_starts == 0 && !exists);
    write_error = 0; commit_error = ESP_FAIL;
    assert(wifi_settings_save(&value) == ESP_FAIL && timer_starts == 0 && !exists);
    commit_error = 0;
    assert(wifi_settings_save(&value) == ESP_OK && timer_starts == 1 && exists);
    wifi_settings_t running = wifi_settings_get();
    assert(!memcmp(&running, &defaults, sizeof(running))); // Changes apply at reboot only.
    assert(wifi_settings_save(&value) == ESP_ERR_INVALID_STATE);
    restart_callback(NULL); assert(restarts == 1);
    boot(); running = wifi_settings_get();
    assert(wifi_settings_saved() && !memcmp(&running, &value, sizeof(running)));
    assert(random_calls==calls); // Saved custom passwords are never regenerated.
    assert(device_settings_configured());
    char key[ABUSEIPDB_KEY_CAPACITY] = "unchanged";
    assert(!abuseipdb_key_get(key) && !key[0]);
    assert(abuseipdb_key_parse("{\"key\":\"test-api-key\"}", key));
    const char *bad_keys[] = {"{}", "{\"key\":null}", "{\"key\":42}",
        "{\"key\":\"bad key\"}", "{\"key\":\"bad\\r\\nheader\"}",
        "{\"key\":\"bad\\u0000key\"}", "{\"key\":\"x\"} trailing", "[[[]]]"};
    for (size_t i = 0; i < sizeof(bad_keys) / sizeof(bad_keys[0]); ++i) {
        assert(!abuseipdb_key_parse(bad_keys[i], key));
        assert(!strcmp(key, "test-api-key"));
    }
    memset(key, 'a', 128); key[128] = 0;
    assert(abuseipdb_key_valid(key));
    memset(key, 'a', sizeof(key)); assert(!abuseipdb_key_valid(key));
    assert(abuseipdb_key_save("test-api-key") == ESP_OK && timer_starts == 1);
    assert(abuseipdb_key_get(key) && !strcmp(key, "test-api-key"));
    write_error = ESP_FAIL;
    assert(abuseipdb_key_save("replacement") == ESP_FAIL);
    write_error = 0; commit_error = ESP_FAIL;
    assert(abuseipdb_key_save("replacement") == ESP_FAIL);
    commit_error = 0;
    assert(abuseipdb_key_get(key) && !strcmp(key, "test-api-key"));
    boot(); running = wifi_settings_get();
    assert(abuseipdb_key_get(key) && !strcmp(key, "test-api-key"));
    assert(wifi_settings_saved() && !memcmp(&running, &value, sizeof(running)));
    assert(abuseipdb_key_save("") == ESP_OK && !abuseipdb_key_get(key));
    boot(); assert(!abuseipdb_key_get(key));
    assert(abuseipdb_key_save("test-api-key") == ESP_OK);
    // Missing or corrupt blobs fall back to initial setup defaults.
    exists = true; disk_size = 3; boot(); assert(!wifi_settings_saved());
    disk_size = sizeof(wifi_settings_t); memset(disk, 255, sizeof(disk));
    boot(); assert(!wifi_settings_saved());
    assert(abuseipdb_key_get(key) && !strcmp(key, "test-api-key"));
    assert(device_settings_configured()); // Valid key still counts if Wi-Fi blob is bad.
    wifi_settings_t fallback=wifi_settings_get();
    assert(!strcmp(fallback.ap_password,first.ap_password));
    // A new device/erased NVS gets a fresh default, without marking setup complete.
    exists=false;disk_size=0;disk_key[0]=0;disk_default[0]=0;
    boot(); second=wifi_settings_get();
    assert(!strcmp(second.ap_ssid,"TxLensAP") && !second.sta_ssid[0] && !second.sta_password[0]);
    assert(strlen(second.ap_password)==8 && strcmp(first.ap_password,second.ap_password));
    assert(!wifi_settings_saved() && !device_settings_configured());
    open_error = ESP_FAIL;
    assert(wifi_settings_save(&value) == ESP_FAIL && timer_starts == 1);
    puts("PASS: validation, password retention, empty STA, NVS failures, reboot persistence, corrupt storage");
    return 0;
}
'''


def main():
    root = Path(__file__).resolve().parents[1]
    cjson = root / "managed_components/espressif__cjson/cJSON"
    with tempfile.TemporaryDirectory(prefix="wifi-settings-") as directory:
        temp = Path(directory)
        (temp / "platform.h").write_text(PLATFORM, encoding="utf-8")
        for name in ("esp_err.h", "esp_log.h", "esp_system.h", "esp_timer.h", "nvs.h", "esp_random.h", "bootloader_random.h",
                     "freertos/FreeRTOS.h", "freertos/semphr.h"):
            path = temp / name
            path.parent.mkdir(exist_ok=True)
            path.write_text('#include "platform.h"\n', encoding="utf-8")
        (temp / "wifi_settings.c").write_text(
            (root / "main/wifi_settings.c").read_text(encoding="utf-8"), encoding="utf-8")
        (temp / "test.c").write_text(TEST, encoding="utf-8")
        subprocess.run(["cl", "/nologo", "/c", "/O2", "/D_CRT_SECURE_NO_WARNINGS",
                        str(cjson / "cJSON.c"), f"/Fo{temp / 'cjson.obj'}"], check=True)
        subprocess.run([
            "cl", "/nologo", "/std:c11", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
            f"/I{temp}", f"/I{root / 'main'}", f"/I{cjson}", str(temp / "test.c"),
            str(root / "main/wifi_settings_parse.c"), str(temp / "cjson.obj"),
            f"/Fo{temp}\\", f"/Fe{temp / 'test.exe'}", "/link", "/INCREMENTAL:NO",
        ], check=True)
        subprocess.run([str(temp / "test.exe")], check=True)


if __name__ == "__main__":
    main()
