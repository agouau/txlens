#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char sta_ssid[33];
    char sta_password[64];
    char ap_ssid[33];
    char ap_password[64];
} wifi_settings_t;

bool wifi_settings_valid(const wifi_settings_t *settings);
// Missing password fields keep their current values; an empty STA password means open Wi-Fi.
bool wifi_settings_parse(const char *json, wifi_settings_t *settings);
esp_err_t wifi_settings_init(void);
wifi_settings_t wifi_settings_get(void);
bool wifi_settings_saved(void);
// Home onboarding is complete with saved Wi-Fi OR a configured optional key.
bool device_settings_configured(void);
// Successful writes schedule a reboot. The running configuration stays unchanged until then.
esp_err_t wifi_settings_save(const wifi_settings_t *settings);
esp_err_t wifi_settings_http_start(void);
// Device-screen only: never return this secret through an unauthenticated API.
const char *wifi_settings_setup_password(void);

#define ABUSEIPDB_KEY_CAPACITY 129
// Optional key is stored separately from Wi-Fi settings, active immediately.
bool abuseipdb_key_valid(const char *key);
bool abuseipdb_key_parse(const char *json, char key[ABUSEIPDB_KEY_CAPACITY]);
bool abuseipdb_key_get(char key[ABUSEIPDB_KEY_CAPACITY]);
esp_err_t abuseipdb_key_save(const char *key);
