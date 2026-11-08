#pragma once

// Router. One client by default; the AP channel follows the upstream network.
#define ROUTER_AP_CHANNEL         6
#define ROUTER_AP_MAX_CONNECTIONS 1
#define ROUTER_AP_IP              "192.168.50.1"
#define ROUTER_AP_IP_CANDIDATES ROUTER_AP_IP, "192.168.51.1", "172.31.250.1", "10.42.0.1"
#define ROUTER_AP_NETMASK         "255.255.255.0"
#define ROUTER_FALLBACK_DNS       "1.1.1.1"
#define ROUTER_RETRY_MS           3000

// Independent capacities for the live history and per-minute SD logs.
#define TRAFFIC_MAX_IPS        128
#define TRAFFIC_MINUTE_MAX_IPS 1000
#define DNS_PENDING_COUNT      64
#define DNS_PENDING_TIMEOUT_MS 10000

// IP/domain/company LRU in PSRAM; periodically saved to SD when available.
#define IP_INFO_CACHE_COUNT           1000
#define IP_CACHE_DNS_TTL_SECONDS      86400
#define IP_CACHE_PROVIDER_TTL_SECONDS 86400
#define IP_INFO_DNS_QUEUE_COUNT       64
#define IP_INFO_HTTP_TIMEOUT_MS       5000
#define IP_INFO_HTTP_DEADLINE_MS      15000

// SD logging. Below the free-space threshold, delete the oldest hour of CSVs.
#define SD_LOG_MIN_FREE_MIB    100
#define SD_LOG_CACHE_SAVE_MS   60000
#define SD_LOG_SPACE_CHECK_MS  60000
#define SD_LOG_CARD_CHECK_MS   5000
#define SD_LOG_POLL_MS         200

// Hours ahead of UTC: Beijing = 8, UTC = 0, UTC-8 = -8, India = 5.5.
// Fixed offset; change this number manually when daylight saving time changes.
#define APP_CLOCK_UTC_OFFSET_HOURS -8
#define APP_CLOCK_NTP_SERVER  "time.google.com"

// Logical UI size, anchored at the physical top left; rebuild after changing.
#define UI_WIDTH  320
#define UI_HEIGHT 480

// 0 = no rotation; 1 = counterclockwise 90 degrees (also rotates touch).
#define UI_ROTATE_CCW_90      0
#define UI_POLL_MS            10
#define UI_DRAW_BUFFER_LINES  32

// Waveshare ESP32-S3-Touch-LCD-7 Rev 1.2: 800x480 RGB565, GT911, CH422G.
// These dimensions match the 7-inch display used during development; the UI only needs a smaller area.
#define LCD_WIDTH  800
#define LCD_HEIGHT 480
#define LCD_PIXEL_CLOCK_HZ 16000000
#define LCD_HSYNC_PULSE_WIDTH 4
#define LCD_HSYNC_BACK_PORCH  8
#define LCD_HSYNC_FRONT_PORCH 8
#define LCD_VSYNC_PULSE_WIDTH 4
#define LCD_VSYNC_BACK_PORCH  8
#define LCD_VSYNC_FRONT_PORCH 8
#define LCD_BOUNCE_BUFFER_LINES 10
#define LCD_HSYNC_GPIO 46
#define LCD_VSYNC_GPIO 3
#define LCD_DE_GPIO    5
#define LCD_PCLK_GPIO  7
// B0..B4, G0..G5, R0..R4.
#define LCD_DATA_GPIOS 14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40

#define BOARD_I2C_SDA_GPIO 8
#define BOARD_I2C_SCL_GPIO 9
#define BOARD_I2C_CLOCK_HZ 100000
#define TOUCH_INT_GPIO     4
#define SD_MOSI_GPIO       11
#define SD_MISO_GPIO       13
#define SD_SCLK_GPIO       12
