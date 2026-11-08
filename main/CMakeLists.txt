idf_component_register(SRCS "sd_log.c" "sd_log_retention.c" "traffic_csv.c" "traffic_minute.c" "main.c" "display.c" "fonts/app_font_mono_12.c" "traffic_meter.c" "router.c"
                            "ip_info.c" "ip_info_parse.c" "ip_cache.c" "ip_cache_db.c" "dns_observe.c" "app_clock.c"
                            "wifi_settings.c" "wifi_settings_parse.c" "settings_http.c" "device_status.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "settings.html"
                       PRIV_REQUIRES esp_lcd esp_driver_i2c esp_wifi esp_event
                                     esp_netif nvs_flash esp_timer lwip esp_driver_gpio
                                     lvgl esp_lcd_touch_gt911 esp_http_client esp_http_server mbedtls cjson
                                     app_update bootloader_support spi_flash fatfs sdmmc esp_driver_sdspi esp_driver_spi sqlite)

# Track the incoming interface while lwIP performs NAT, without patching ESP-IDF.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ip4_input")
