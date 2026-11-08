#include "wifi_settings.h"

#include <stdio.h>
#include <string.h>
#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static wifi_settings_t s_settings;
static bool s_saved;
static bool s_restarting;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_restart;
static char s_abuse_key[ABUSEIPDB_KEY_CAPACITY];

static void restart(void *arg)
{
    (void)arg;
    esp_restart();
}

esp_err_t wifi_settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    const esp_timer_create_args_t timer = {.callback = restart, .name = "settings_restart"};
    esp_err_t err = esp_timer_create(&timer, &s_restart);
    if (err != ESP_OK) return err;
    s_settings = (wifi_settings_t){.ap_ssid = "TxLensAP"};
    s_saved = false;
    nvs_handle_t handle;
    s_abuse_key[0] = 0;
    err = nvs_open("wifi_settings", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    wifi_settings_t stored = {0};
    size_t size = sizeof(stored);
    err = nvs_get_blob(handle, "config", &stored, &size);
    size_t key_size = sizeof(s_abuse_key);
    if (nvs_get_str(handle, "abuse_key", s_abuse_key, &key_size) != ESP_OK ||
        !abuseipdb_key_valid(s_abuse_key)) s_abuse_key[0] = 0;
    if (err == ESP_OK && size == sizeof(stored) && wifi_settings_valid(&stored)) {
        s_settings = stored;
        s_saved = true;
    } else {
        size_t password_size = sizeof(s_settings.ap_password);
        err = nvs_get_str(handle, "ap_default", s_settings.ap_password, &password_size);
        if (err != ESP_OK || strlen(s_settings.ap_password) != 8 || !wifi_settings_valid(&s_settings)) {
            // Called before Wi-Fi/ADC initialization: temporarily enable hardware entropy.
            static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
            bootloader_random_enable();
            for (size_t i = 0; i < 8; ++i) {
                uint32_t random;
                uint32_t count = sizeof(alphabet) - 1;
                do { random = esp_random(); } while (random >= UINT32_MAX - UINT32_MAX % count);
                s_settings.ap_password[i] = alphabet[random % count];
            }
            bootloader_random_disable();
            s_settings.ap_password[8] = 0;
            if (!wifi_settings_valid(&s_settings)) { nvs_close(handle); return ESP_ERR_INVALID_ARG; }
            err = nvs_set_str(handle, "ap_default", s_settings.ap_password);
            if (err == ESP_OK) err = nvs_commit(handle);
            if (err != ESP_OK) { nvs_close(handle); return err; }
        }
    }
    nvs_close(handle);
    return ESP_OK;
}

wifi_settings_t wifi_settings_get(void)
{
    // Immutable for this boot; saves become active only after restart.
    return s_settings;
}

bool wifi_settings_saved(void) { return s_saved; }

bool device_settings_configured(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool configured = s_saved || s_abuse_key[0] != 0;
    xSemaphoreGive(s_lock);
    return configured;
}

static esp_err_t persist(const wifi_settings_t *settings)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_restarting) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_settings", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "config", settings, sizeof(*settings));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        // Timer is allocated at startup; give the HTTP response time to reach the browser.
        err = esp_timer_start_once(s_restart, 2000000);
        s_restarting = err == ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t wifi_settings_save(const wifi_settings_t *settings)
{
    return wifi_settings_valid(settings) ? persist(settings) : ESP_ERR_INVALID_ARG;
}

bool abuseipdb_key_get(char key[ABUSEIPDB_KEY_CAPACITY])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strcpy(key, s_abuse_key);
    xSemaphoreGive(s_lock);
    return key[0] != 0;
}

esp_err_t abuseipdb_key_save(const char *key)
{
    if (!abuseipdb_key_valid(key)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_restarting) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_settings", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "abuse_key", key);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) strcpy(s_abuse_key, key);
    xSemaphoreGive(s_lock);
    return err;
}
