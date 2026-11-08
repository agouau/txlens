#include "app_clock.h"
#include "app_config.h"
#include "router.h"

#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_clock";
static atomic_bool s_ready;

static void clock_task(void *arg)
{
    (void)arg;
    bool started = false;
    // Keep SNTP confined to this task. Once successful, deinitialize it so
    // neither periodic polling nor a Wi-Fi reconnection can resync the clock.
    while (true) {
        if (!started && router_get_status().state != ROUTER_READY) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        esp_err_t error = ESP_OK;
        if (!started) {
            ESP_LOGI(TAG, "Synchronizing startup clock");
            error = esp_netif_sntp_start();
            started = error == ESP_OK;
        }
        if (error == ESP_OK) {
            error = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
        }
        if (error == ESP_OK) {
            esp_netif_sntp_deinit();
            atomic_store(&s_ready, true);
            ESP_LOGI(TAG, "Clock synchronized; network time service stopped");
            break;
        }
        // SNTP owns DNS/request retries, including recovery after a disconnect.
        // Keep waiting without restarting an already running synchronization.
        ESP_LOGW(TAG, "Clock synchronization pending: %s", esp_err_to_name(error));
        if (!started) vTaskDelay(pdMS_TO_TICKS(3000));
    }
    vTaskDelete(NULL);
}

esp_err_t app_clock_init(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(APP_CLOCK_NTP_SERVER);
    config.start = false;
    esp_err_t error = esp_netif_sntp_init(&config);
    if (error != ESP_OK) return error;
    if (xTaskCreate(clock_task, "app_clock", 4096, NULL, 3, NULL) != pdPASS) {
        esp_netif_sntp_deinit();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool app_clock_is_ready(void)
{
    return atomic_load(&s_ready);
}

void app_clock_format(char *buffer, size_t size)
{
    if (!app_clock_is_ready()) {
        snprintf(buffer, size, "Not synchronized");
        return;
    }
    // Only shift the displayed time; keep the system clock in UTC for HTTPS.
    const int offset_minutes = (int)(APP_CLOCK_UTC_OFFSET_HOURS * 60);
    time_t now = time(NULL) + offset_minutes * 60;
    struct tm local;
    char date[32];
    if (gmtime_r(&now, &local) == NULL ||
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local) == 0) {
        snprintf(buffer, size, "Unavailable");
        return;
    }
    const int magnitude = offset_minutes < 0 ? -offset_minutes : offset_minutes;
    if (magnitude % 60)
        snprintf(buffer, size, "%s UTC%c%d:%02d", date,
                 offset_minutes < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
    else
        snprintf(buffer, size, "%s UTC%c%d", date,
                 offset_minutes < 0 ? '-' : '+', magnitude / 60);
}
