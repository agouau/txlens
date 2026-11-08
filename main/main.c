#include "display.h"
#include "app_config.h"
#include "app_clock.h"
#include "ip_info.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "router.h"
#include "traffic_meter.h"
#include "sd_log.h"

void app_main(void)
{
    ESP_ERROR_CHECK(traffic_meter_init());
    router_start();
    ESP_ERROR_CHECK(app_clock_init());
    ESP_ERROR_CHECK(ip_info_init());
    display_init();
    ESP_ERROR_CHECK(sd_log_init());

    // All LVGL calls run on this task. Packet timestamps are independent of UI.
    while (true) {
        display_process();
        vTaskDelay(pdMS_TO_TICKS(UI_POLL_MS));
    }
}
