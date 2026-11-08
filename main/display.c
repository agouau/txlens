#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "display.h"
#include "device_status.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "src/libs/qrcode/qrcodegen.h"
#include "app_config.h"
#include "router.h"
#include "wifi_settings.h"
#include "traffic_meter.h"
#include "ip_info.h"
#include "ip_info_parse.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

_Static_assert(UI_ROTATE_CCW_90 == 0 || UI_ROTATE_CCW_90 == 1, "UI_ROTATE_CCW_90 must be 0 or 1");
_Static_assert(UI_WIDTH > 0 && UI_HEIGHT > 0 &&
               UI_WIDTH <= (UI_ROTATE_CCW_90 ? LCD_HEIGHT : LCD_WIDTH) &&
               UI_HEIGHT <= (UI_ROTATE_CCW_90 ? LCD_WIDTH : LCD_HEIGHT),
               "UI dimensions exceed the panel in the selected orientation");

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_frame_lock;
static atomic_bool s_capture_ready;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_expander_output;
static void init_ui(i2c_master_bus_handle_t bus);

// CH422G uses separate I2C command addresses, not register numbers.
#define CH422G_MODE_ADDR   0x24
#define CH422G_OUTPUT_ADDR 0x38
#define TOUCH_RESET       (1U << 1)
#define LCD_BACKLIGHT     (1U << 2)
#define LCD_RESET         (1U << 3)
#define SD_CS             (1U << 4)

esp_err_t display_sd_select(bool selected)
{
    uint8_t value = LCD_RESET | LCD_BACKLIGHT | TOUCH_RESET;
    if (!selected) value |= SD_CS;
    return i2c_master_transmit(s_expander_output, &value, 1, 100);
}

static i2c_master_dev_handle_t init_expander(void)
{
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

    s_bus = bus;
    ESP_ERROR_CHECK(gpio_set_direction(TOUCH_INT_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(TOUCH_INT_GPIO, 0)); // Select GT911 address 0x5D.

    i2c_master_dev_handle_t mode;
    i2c_master_dev_handle_t output;
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CH422G_MODE_ADDR,
        .scl_speed_hz = BOARD_I2C_CLOCK_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &device_config, &mode));
    device_config.device_address = CH422G_OUTPUT_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &device_config, &output));

    // Keep the backlight off and LCD in reset; deselect the SD card.
    // EXIO5 stays low (USB mode). Hold touch reset while INT is low.
    uint8_t value = SD_CS;
    ESP_ERROR_CHECK(i2c_master_transmit(output, &value, 1, 100));
    value = 0x01; // Enable IO0..IO7 as push-pull outputs.
    ESP_ERROR_CHECK(i2c_master_transmit(mode, &value, 1, 100));
    ESP_ERROR_CHECK(i2c_master_bus_rm_device(mode));
    vTaskDelay(pdMS_TO_TICKS(10));

    value = SD_CS | LCD_RESET | TOUCH_RESET;
    ESP_ERROR_CHECK(i2c_master_transmit(output, &value, 1, 100));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(gpio_set_direction(TOUCH_INT_GPIO, GPIO_MODE_INPUT));
    return output;
}

static void configure_panel_rotation(esp_lcd_panel_handle_t panel)
{
#if UI_ROTATE_CCW_90
    // Driver maps (u,v) to (v,LCD_HEIGHT-1-u) while copying into its framebuffer.
    // Offset logical X so our smaller UI remains at the physical top left.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, LCD_HEIGHT - UI_WIDTH, 0));
#else
    (void)panel;
#endif
}

void display_init(void)
{
    s_frame_lock = xSemaphoreCreateMutex();
    assert(s_frame_lock);
    i2c_master_dev_handle_t output = init_expander();

    // Panel timing and board wiring are configured in app_config.h.
    const esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PIXEL_CLOCK_HZ,
            .h_res = LCD_WIDTH,
            .v_res = LCD_HEIGHT,
            .hsync_pulse_width = LCD_HSYNC_PULSE_WIDTH,
            .hsync_back_porch = LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = LCD_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = LCD_VSYNC_PULSE_WIDTH,
            .vsync_back_porch = LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = LCD_VSYNC_FRONT_PORCH,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .bounce_buffer_size_px = LCD_WIDTH * LCD_BOUNCE_BUFFER_LINES,
        .hsync_gpio_num = LCD_HSYNC_GPIO,
        .vsync_gpio_num = LCD_VSYNC_GPIO,
        .de_gpio_num = LCD_DE_GPIO,
        .pclk_gpio_num = LCD_PCLK_GPIO,
        .disp_gpio_num = -1,
        .data_gpio_nums = { LCD_DATA_GPIOS },
        .flags.fb_in_psram = true,
    };
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel));

    // Fill the driver's framebuffer before starting continuous RGB output.
    uint16_t *framebuffer;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 1, (void **)&framebuffer));
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; ++i) {
        framebuffer[i] = 0; // Leave the unused area black.
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    configure_panel_rotation(panel);
    vTaskDelay(pdMS_TO_TICKS(50));

    const uint8_t value = SD_CS | LCD_RESET | LCD_BACKLIGHT | TOUCH_RESET;
    ESP_ERROR_CHECK(i2c_master_transmit(output, &value, 1, 100));
    s_panel = panel;
    s_expander_output = output;
    init_ui(s_bus);
    atomic_store_explicit(&s_capture_ready, true, memory_order_release);

    ESP_LOGI("display", "LCD ready: %dx%d logical UI, CCW90=%d, top left of %dx%d panel, RGB565",
             UI_WIDTH, UI_HEIGHT, UI_ROTATE_CCW_90, LCD_WIDTH, LCD_HEIGHT);
}

esp_err_t display_capture_rgb565(uint16_t **pixels, int *width, int *height)
{
    if (!pixels || !width || !height) return ESP_ERR_INVALID_ARG;
    *pixels = NULL;
    if (!atomic_load_explicit(&s_capture_ready, memory_order_acquire))
        return ESP_ERR_INVALID_STATE;
    size_t bytes = (size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
    uint16_t *copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return ESP_ERR_NO_MEM;
    uint16_t *frame;
    xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    esp_err_t err = esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, (void **)&frame);
    if (err == ESP_OK) memcpy(copy, frame, bytes);
    xSemaphoreGive(s_frame_lock);
    if (err != ESP_OK) {
        free(copy);
        return err;
    }
    *pixels = copy;
    *width = LCD_WIDTH;
    *height = LCD_HEIGHT;
    return ESP_OK;
}


// The UI is owned by the main task, including timer and input callbacks.
LV_FONT_DECLARE(app_font_mono_12);
enum { ROW_ADDRESS_WIDTH = UI_WIDTH * 3 / 5 };
static lv_obj_t *s_list;
static lv_obj_t *s_empty;
static lv_obj_t *s_empty_hero;
static lv_obj_t *s_empty_guide;
static lv_obj_t *s_empty_message;
static lv_obj_t *s_empty_status;
static lv_obj_t *s_empty_hint;
static lv_obj_t *s_empty_arrow;
static traffic_snapshot_t *s_snapshot;
static esp_lcd_touch_handle_t s_touch;
static uint32_t s_pressed_ip;
static lv_point_t s_press_point;
static size_t s_pressed_index;
static size_t s_pressed_count;
static lv_obj_t *s_picker;
static uint32_t s_picker_ips[TRAFFIC_MAX_IPS];
static size_t s_picker_count;
static bool s_press_valid;
static lv_obj_t *s_dialog;
static lv_obj_t *s_details;
static ip_info_result_t s_lookup;
static char s_abuse[384];
static lv_obj_t *s_tabs;
static lv_obj_t *s_settings_text;
static lv_obj_t *s_settings_qr;
static char s_settings_payload[256];
static lv_obj_t *s_status_text;

static void refresh_rows(void);

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                    area->x2 + 1, area->y2 + 1, pixels));
    xSemaphoreGive(s_frame_lock);
    lv_display_flush_ready(display);
}

static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;
    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) {
        return;
    }
    esp_lcd_touch_point_data_t point;
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(s_touch, &point, &count, 1) != ESP_OK || count == 0) return;
#if UI_ROTATE_CCW_90
    if (point.x >= UI_HEIGHT || point.y >= UI_WIDTH) return;
    data->point.x = UI_WIDTH - 1 - point.y;
    data->point.y = point.x;
#else
    if (point.x >= UI_WIDTH || point.y >= UI_HEIGHT) return;
    data->point.x = point.x;
    data->point.y = point.y;
#endif
    data->state = LV_INDEV_STATE_PRESSED;
}

// Shared logarithmic scale: nonzero traffic is always visible. Each doubling
// above 1280 bytes/second adds one pixel, up to 15 pixels. No per-row rescaling.
static int thickness(uint32_t bytes)
{
    if (bytes == 0) {
        return 0;
    }
    int height = 1;
    for (uint32_t value = bytes / 1280; value != 0 && height < 15; value >>= 1) {
        ++height;
    }
    return height;
}

// Compress the whole minute to the available width; preserve short bursts by
// taking the maximum bucket in each pixel column. Keep every graph inside its row.
static void draw_history(lv_layer_t *layer, const lv_area_t *area,
                         const traffic_row_t *row)
{
    int width = lv_area_get_width(area);
    int height_limit = lv_area_get_height(area);
    int center = (area->y1 + area->y2 + 1) / 2;
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = lv_color_hex(0x21323e);
    lv_area_t baseline = {area->x1, center, area->x2, center};
    lv_draw_rect(layer, &rect, &baseline);
    rect.bg_color = lv_color_hex(0x57c9de);
    int start = 0;
    int previous = 0;
    for (int x = 0; x <= width; ++x) {
        uint32_t peak = 0;
        if (x < width) {
            int first = x * TRAFFIC_BUCKET_COUNT / width;
            int end = LV_MAX(first + 1, (x + 1) * TRAFFIC_BUCKET_COUNT / width);
            for (int j = first; j < end; ++j) {
                if (row->bytes[j] > peak) peak = row->bytes[j];
            }
        }
        int height = LV_MIN(thickness(peak), height_limit);
        if (height == previous) continue;
        // Merge equal columns to avoid a draw task for every pixel.
        if (previous > 0) {
            lv_area_t segment = {area->x1 + start, center - previous / 2,
                                 area->x1 + x - 1, center - previous / 2 + previous - 1};
            lv_draw_rect(layer, &rect, &segment);
        }
        start = x;
        previous = height;
    }
}

static void format_row_address(uint32_t ip, char address[256])
{
    if (!ip_info_name(ip, address)) ip_info_format_ip(ip, address);
    // Keep the address suffix within three fifths of the screen width.
    const char *suffix = address;
    lv_point_t extent;
    lv_text_get_size(&extent, address, &app_font_mono_12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (extent.x > ROW_ADDRESS_WIDTH) {
        int marker_width = lv_text_get_width("*", 1, &app_font_mono_12, 0);
        do {
            ++suffix;
            lv_text_get_size(&extent, suffix, &app_font_mono_12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        } while (extent.x + marker_width > ROW_ADDRESS_WIDTH && suffix[0]);
        memmove(address + 1, suffix, strlen(suffix) + 1);
        address[0] = '*';
    }
}

static void draw_row(lv_layer_t *layer, const lv_area_t *area,
                     size_t index)
{
    const traffic_row_t *row = &s_snapshot->rows[index];
    char address[256];
    format_row_address(row->ip, address);
    uint64_t age = s_snapshot->now_us - row->last_seen_us;
    const uint64_t history_us = TRAFFIC_BUCKET_COUNT * TRAFFIC_BUCKET_US;
    uint32_t fade = age >= history_us ? 255 : (uint32_t)(age * 255 / history_us);
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = &app_font_mono_12;
    label.text = address;
    label.text_local = 1;
    label.color = lv_color_mix(lv_color_hex(0x667d8c),
                              lv_color_hex(0xe4f0f7), (lv_opa_t)fade);
    int y = (area->y1 + area->y2 + 1 - label.font->line_height) / 2;
    // Dense overview labels may overlap, but the first and last stay on screen.
    lv_area_t list_area;
    lv_obj_get_coords(s_list, &list_area);
    if (y < list_area.y1) y = list_area.y1;
    if (y + label.font->line_height - 1 > list_area.y2)
        y = list_area.y2 - label.font->line_height + 1;
    lv_area_t text_area = {area->x1, y, area->x1 + ROW_ADDRESS_WIDTH - 1, y + label.font->line_height - 1};
    lv_draw_label(layer, &label, &text_area);
    lv_area_t graph = {area->x1 + ROW_ADDRESS_WIDTH + 4, area->y1, area->x2, area->y2};
    draw_history(layer, &graph, row);
}

static int overview_height(int available)
{
    // Keep sparse rows together at the top; compress only when they no longer fit.
    return LV_MIN(available, (int)s_snapshot->count * 44);
}

static void draw_overview(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(s_list, &area);
    int height = overview_height(lv_area_get_height(&area));
    for (size_t i = 0; i < s_snapshot->count; ++i) {
        lv_area_t row = {area.x1, area.y1 + i * height / s_snapshot->count,
                         area.x2, area.y1 + (i + 1) * height / s_snapshot->count - 1};
        draw_row(lv_event_get_layer(event), &row, i);
    }
}

static void draw_choice(lv_event_t *event)
{
    uint32_t ip = *(uint32_t *)lv_event_get_user_data(event);
    for (size_t i = 0; i < s_snapshot->count; ++i) {
        if (s_snapshot->rows[i].ip != ip) continue;
        lv_area_t area;
        lv_obj_get_coords(lv_event_get_target_obj(event), &area);
        draw_row(lv_event_get_layer(event), &area, i);
        return;
    }
    // Keep an expired choice identifiable without letting it select a new IP.
    char address[16];
    ip_info_format_ip(ip, address);
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = &app_font_mono_12;
    label.text = address;
    label.text_local = 1;
    label.color = lv_color_hex(0x667d8c);
    lv_draw_label(lv_event_get_layer(event), &label, &area);
}

static void refresh_details(void)
{
    if (s_dialog == NULL) return;
    uint64_t total = 0;
    for (size_t i = 0; i < s_snapshot->count; ++i) {
        const traffic_row_t *row = &s_snapshot->rows[i];
        if (row->ip != s_lookup.ip) continue;
        for (size_t j = 0; j < TRAFFIC_BUCKET_COUNT; ++j) total += row->bytes[j];
        break;
    }
    char text[1408];
    snprintf(text, sizeof(text), "%s%sTX (60s): %.2f KB\n\n%s\nLocation: %s\n\n%s",
             s_lookup.ptr, s_lookup.ptr[0] ? "\n\n" : "",
             (double)total / 1024.0,
             s_lookup.network[0] ? s_lookup.network : "ISP: --\nOrg: --\nASN: --",
             s_lookup.location,
             s_abuse);
    lv_label_set_text(s_details, text);
}

static void close_details(lv_event_t *event)
{
    (void)event;
    if (s_dialog == NULL) return;
    ip_info_cancel();
    s_abuse[0] = 0;
    lv_msgbox_close_async(s_dialog);
    s_dialog = NULL;
    s_details = NULL;
}

static void style_action_button(lv_obj_t *button)
{
    lv_obj_t *footer = lv_obj_get_parent(button);
    lv_obj_set_height(footer, 72);
    lv_obj_set_style_pad_all(footer, 12, 0);
    lv_obj_set_style_pad_column(footer, 10, 0);
    lv_obj_set_width(button, 0);
    lv_obj_set_height(button, 48);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x25313a), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x35434d), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_hex(0xd0d8de), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x3d4a54), 0);
}

static void open_details(uint32_t ip)
{
    s_lookup = (ip_info_result_t){ .ip = ip };
    strcpy(s_lookup.location, "Looking up...");
    strcpy(s_abuse, "AbuseIPDB: Looking up...");
    char address[16];
    ip_info_format_ip(s_lookup.ip, address);
    s_dialog = lv_msgbox_create(NULL);
    lv_obj_set_style_bg_color(lv_obj_get_parent(s_dialog), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_obj_get_parent(s_dialog), LV_OPA_60, 0);
    lv_obj_set_size(s_dialog, UI_WIDTH - 24, UI_HEIGHT - 24);
    lv_obj_set_style_bg_color(s_dialog, lv_color_hex(0x14232f), 0);
    lv_obj_set_style_text_color(s_dialog, lv_color_hex(0xdce8ef), 0);
    lv_msgbox_add_title(s_dialog, address);
    s_details = lv_msgbox_add_text(s_dialog, "");
    lv_obj_t *content = lv_msgbox_get_content(s_dialog);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_style_text_line_space(s_details, 3, 0);
    lv_obj_t *close = lv_msgbox_add_footer_button(s_dialog, "Close");
    style_action_button(close);
    lv_obj_add_event_cb(close, close_details, LV_EVENT_CLICKED, NULL);
    lv_obj_t *parts[] = {lv_msgbox_get_header(s_dialog), content, lv_msgbox_get_footer(s_dialog)};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        lv_obj_set_style_bg_color(parts[i], lv_color_hex(0x14232f), 0);
        lv_obj_set_style_text_color(parts[i], lv_color_hex(0xdce8ef), 0);
    }
    s_lookup.request_id = ip_info_request(s_lookup.ip);
    refresh_details();
}

static void close_picker(lv_event_t *event)
{
    (void)event;
    if (s_picker == NULL) return;
    lv_msgbox_close_async(s_picker);
    s_picker = NULL;
    s_picker_count = 0;
}

static void choose_ip(lv_event_t *event)
{
    uint32_t ip = *(uint32_t *)lv_event_get_user_data(event);
    close_picker(NULL);
    open_details(ip);
}

static void open_picker(size_t first, size_t end)
{
    s_picker = lv_msgbox_create(NULL);
    int height = LV_MIN(UI_HEIGHT - 24, 144 + (end - first) * 52);
    int row_height = (height - 144) / (end - first);
    lv_obj_set_size(s_picker, UI_WIDTH - 24, height);
    lv_obj_set_style_bg_color(s_picker, lv_color_hex(0x14232f), 0);
    lv_obj_set_style_text_color(s_picker, lv_color_hex(0xdce8ef), 0);
    lv_msgbox_add_title(s_picker, "Choose IP");
    lv_obj_t *content = lv_msgbox_get_content(s_picker);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(content, 0, 0);
    s_picker_count = end - first;
    for (size_t i = 0; i < s_picker_count; ++i) {
        s_picker_ips[i] = s_snapshot->rows[first + i].ip;
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), row_height);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, draw_choice, LV_EVENT_DRAW_MAIN, &s_picker_ips[i]);
        lv_obj_add_event_cb(row, choose_ip, LV_EVENT_CLICKED, &s_picker_ips[i]);
    }
    lv_obj_t *close = lv_msgbox_add_footer_button(s_picker, "Close");
    style_action_button(close);
    lv_obj_add_event_cb(close, close_picker, LV_EVENT_CLICKED, NULL);
    lv_obj_t *parts[] = {lv_msgbox_get_header(s_picker), content, lv_msgbox_get_footer(s_picker)};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        lv_obj_set_style_bg_color(parts[i], lv_color_hex(0x14232f), 0);
        lv_obj_set_style_text_color(parts[i], lv_color_hex(0xdce8ef), 0);
    }
}

static void list_event(lv_event_t *event)
{
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    lv_area_t area;
    lv_obj_get_coords(s_list, &area);
    int height = overview_height(lv_area_get_height(&area));
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        s_press_valid = s_snapshot->count > 0 && point.y >= area.y1 &&
                        point.y < area.y1 + height;
        if (!s_press_valid) return;
        s_press_point = point;
        s_pressed_count = s_snapshot->count;
        s_pressed_index = ((point.y - area.y1 + 1) * s_snapshot->count - 1) / height;
        if (s_pressed_index >= s_snapshot->count) {
            s_press_valid = false;
            return;
        }
        s_pressed_ip = s_snapshot->rows[s_pressed_index].ip;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (LV_ABS(point.x - s_press_point.x) > 12 || LV_ABS(point.y - s_press_point.y) > 12)
            s_press_valid = false;
        return;
    }
    if (code != LV_EVENT_CLICKED || s_dialog != NULL || s_picker != NULL || !s_press_valid)
        return;
    s_press_valid = false;
    if (s_pressed_count != s_snapshot->count || s_snapshot->rows[s_pressed_index].ip != s_pressed_ip)
        return;
    if (height / s_snapshot->count >= 44) {
        open_details(s_pressed_ip);
        return;
    }
    // Limit the neighborhood so the expanded choices retain readable spacing.
    size_t choices = (UI_HEIGHT - 144) / 20;
    int radius = LV_MIN(22, height * (choices - 1) / (2 * s_snapshot->count));
    int top = LV_MAX(0, s_press_point.y - area.y1 - radius);
    int bottom = LV_MIN(height - 1, s_press_point.y - area.y1 + radius);
    size_t first = ((top + 1) * s_snapshot->count - 1) / height;
    size_t end = ((bottom + 1) * s_snapshot->count - 1) / height + 1;
    open_picker(first, end);
}

static void refresh_settings(void)
{
    wifi_settings_t settings = wifi_settings_get();
    router_status_t status = router_get_status();
    char payload[64], text[256];
    const char *password = wifi_settings_setup_password();
    snprintf(payload, sizeof(payload), "http://%s/#password=%s", status.ap_ip, password);
    snprintf(text, sizeof(text),
             "1. Join device hotspot\nSSID: %s\nPassword: %s\n\n2. Open settings\nhttp://%s/\nSetup code: %s",
             settings.ap_ssid, settings.ap_password, status.ap_ip, password);
    lv_label_set_text(s_settings_text, text);
    if (strcmp(payload, s_settings_payload)) {
        int version = qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, strlen(payload));
        int modules = qrcodegen_version2size(version);
        int scale = LV_MAX(1, LV_MIN(160, UI_WIDTH - 112) / modules);
        // Exact module multiples avoid extra white margins inside LVGL's canvas.
        lv_qrcode_set_size(s_settings_qr, modules * scale);
        lv_obj_set_size(lv_obj_get_parent(s_settings_qr),
                        (modules + 2) * scale, (modules + 2) * scale);
        lv_result_t result = lv_qrcode_update(s_settings_qr, payload, strlen(payload));
        lv_obj_set_style_opa(s_settings_qr, result == LV_RESULT_OK ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        snprintf(s_settings_payload, sizeof(s_settings_payload), "%s", payload);
    }
}

static void create_settings_page(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(page, 12, 0);
    lv_obj_t *intro = lv_label_create(page);
    lv_obj_set_width(intro, LV_PCT(100));
    lv_label_set_text(intro, "Use your phone to configure Wi-Fi and an optional AbuseIPDB key.");
    s_settings_text = lv_label_create(page);
    lv_obj_set_width(s_settings_text, LV_PCT(100));
    // One module of white on each side; refresh_settings sizes it with the code.
    lv_obj_t *surround = lv_obj_create(page);
    lv_obj_set_style_bg_color(surround, lv_color_white(), 0);
    lv_obj_set_style_border_width(surround, 0, 0);
    lv_obj_set_style_radius(surround, 0, 0);
    lv_obj_set_style_pad_all(surround, 0, 0);
    lv_obj_remove_flag(surround, LV_OBJ_FLAG_SCROLLABLE);
    s_settings_qr = lv_qrcode_create(surround);
    lv_qrcode_set_dark_color(s_settings_qr, lv_color_black());
    lv_qrcode_set_light_color(s_settings_qr, lv_color_white());
    lv_obj_center(s_settings_qr);
    refresh_settings();
}

static void refresh_status(void)
{
    char text[768];
    device_status_format(text, sizeof(text));
    lv_label_set_text(s_status_text, text);
}

static void refresh_empty(void)
{
    if (s_snapshot->count != 0) return;
    bool configured = device_settings_configured();
    if (configured) lv_obj_add_flag(s_empty_guide, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_empty_guide, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_empty_hero, LV_ALIGN_TOP_MID, 0, configured ? 50 : 18);
    router_status_t status = router_get_status();
    const char *upstream = "Connecting";
    switch (status.state) {
    case ROUTER_NOT_CONFIGURED: upstream = "Set up Wi-Fi"; break;
    case ROUTER_READY: upstream = "Connected"; break;
    case ROUTER_WAITING_IP: upstream = "Getting an IP"; break;
    case ROUTER_SUBNET_CONFLICT: upstream = "Address conflict"; break;
    default: break;
    }
    lv_label_set_text_fmt(s_empty_status, "Upstream: %s\nDevice: %s", upstream,
                          status.clients ? "Connected" : "Not connected");
    lv_label_set_text(s_empty_message, status.clients ? "Outgoing traffic appears here." :
                      "Connect a device to the hotspot.\nOutgoing traffic appears here.");
}

static lv_obj_t *empty_label(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static void draw_direction(lv_event_t *event)
{
    lv_area_t bounds;
    lv_obj_get_coords(lv_event_get_target(event), &bounds);
    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_color = lv_color_hex(0x57c9de);
    // Small, solid steps keep arrows crisp on the panel in either orientation.
    bool outgoing = lv_event_get_user_data(event) != NULL;
    for (int i = 0; i < (outgoing ? 20 : 8); ++i) {
        int x = outgoing ? 18 + i : 4 + i;
        int y = outgoing ? 26 - i : 13 + i;
        lv_area_t part = {bounds.x1 + x, bounds.y1 + y, bounds.x1 + x + 1, bounds.y1 + y + 1};
        lv_draw_rect(lv_event_get_layer(event), &ink, &part);
        if (!outgoing) {
            part.x1 = bounds.x1 + 18 - i;
            part.x2 = part.x1 + 1;
            lv_draw_rect(lv_event_get_layer(event), &ink, &part);
        }
    }
    lv_area_t stem = outgoing ? (lv_area_t){bounds.x1 + 28, bounds.y1 + 6, bounds.x1 + 39, bounds.y1 + 7} :
                               (lv_area_t){bounds.x1 + 11, bounds.y1, bounds.x1 + 12, bounds.y1 + 20};
    lv_draw_rect(lv_event_get_layer(event), &ink, &stem);
    if (outgoing) {
        stem = (lv_area_t){bounds.x1 + 38, bounds.y1 + 6, bounds.x1 + 39, bounds.y1 + 17};
        lv_draw_rect(lv_event_get_layer(event), &ink, &stem);
    }
}

static void create_empty_page(lv_obj_t *page)
{
    s_empty = lv_obj_create(page);
    lv_obj_remove_style_all(s_empty);
    lv_obj_set_size(s_empty, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hero = s_empty_hero = lv_obj_create(s_empty);
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(hero, 10, 0);
    lv_obj_remove_flag(hero, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *brand = lv_obj_create(hero);
    lv_obj_remove_style_all(brand);
    lv_obj_set_size(brand, LV_SIZE_CONTENT, 44);
    lv_obj_set_flex_flow(brand, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brand, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(brand, 10, 0);
    lv_obj_remove_flag(brand, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *mark = lv_obj_create(brand);
    lv_obj_remove_style_all(mark);
    lv_obj_set_size(mark, 54, 32);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    const int heights[] = {8, 20, 32, 20, 8};
    for (size_t i = 0; i < sizeof(heights) / sizeof(heights[0]); ++i) {
        lv_obj_t *bar = lv_obj_create(mark);
        lv_obj_remove_style_all(bar);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(bar, 6, heights[i]);
        lv_obj_set_pos(bar, i * 12, (32 - heights[i]) / 2);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x57c9de), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
    }
    lv_obj_t *title = lv_label_create(brand);
    lv_label_set_text(title, "Tx Lens");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xdce8ef), 0);
    empty_label(hero, "See where your data goes", 0xa8bdcc);
    empty_label(hero, "https://txlens.pages.dev", 0x8499a8);
    s_empty_message = empty_label(hero, "", 0x8499a8);
    lv_obj_set_style_pad_top(s_empty_message, 14, 0);
    s_empty_status = empty_label(hero, "", 0xa8bdcc);
    lv_obj_set_style_pad_top(s_empty_status, 8, 0);

    lv_obj_t *guide = s_empty_guide = lv_obj_create(s_empty);
    lv_obj_remove_style_all(guide);
    lv_obj_set_size(guide, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(guide, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(guide, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(guide, 8, 0);
    lv_obj_align(guide, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_remove_flag(guide, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_empty_hint = empty_label(guide, "Open Settings below\nto set up and start using Tx Lens", 0xa8bdcc);
    // Center of the page is also the center of the middle Settings tab.
    s_empty_arrow = lv_obj_create(guide);
    lv_obj_remove_style_all(s_empty_arrow);
    lv_obj_set_size(s_empty_arrow, 24, 24);
    lv_obj_remove_flag(s_empty_arrow, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_empty_arrow, draw_direction, LV_EVENT_DRAW_MAIN, NULL);
    refresh_empty();
}

static void create_ui(void)
{
    // Apply the same monospace font to every themed widget, including modal layers.
    lv_display_t *display = lv_display_get_default();
    lv_display_set_theme(display, lv_theme_default_init(display,
                         lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
                         true, &app_font_mono_12));
    // Message boxes are children of the top layer rather than the active screen.
    lv_obj_set_style_text_font(lv_layer_top(), &app_font_mono_12, 0);
    lv_obj_set_style_text_font(lv_layer_sys(), &app_font_mono_12, 0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0c151e), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xdce8ef), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    s_tabs = lv_tabview_create(screen);
    lv_obj_set_size(s_tabs, LV_PCT(100), LV_PCT(100));
    lv_tabview_set_tab_bar_position(s_tabs, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(s_tabs, 44);
    // Navigate with the tab bar; dragging traffic must not switch pages.
    lv_obj_remove_flag(lv_tabview_get_content(s_tabs), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *traffic = lv_tabview_add_tab(s_tabs, "TX Traffic");
    lv_obj_t *settings = lv_tabview_add_tab(s_tabs, "Settings");
    lv_obj_t *status = lv_tabview_add_tab(s_tabs, "Status");
    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tabs);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x171f26), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x303b44), 0);
    for (uint32_t i = 0; i < 3; ++i) {
        lv_obj_t *button = lv_obj_get_child(bar, i);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x171f26), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x25313a), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x303d47), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(button, lv_color_hex(0x8495a1), 0);
        lv_obj_set_style_text_color(button, lv_color_hex(0xdce4e9), LV_STATE_CHECKED);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_border_width(button, 2, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(button, LV_BORDER_SIDE_BOTTOM, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(button, lv_color_hex(0x81938e), LV_STATE_CHECKED);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_pad_all(button, 0, 0);
    }
    lv_obj_t *pages[] = {traffic, settings, status};
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        lv_obj_set_style_bg_color(pages[i], lv_color_hex(0x0c151e), 0);
        lv_obj_set_style_text_color(pages[i], lv_color_hex(0xdce8ef), 0);
        lv_obj_set_style_pad_all(pages[i], 12, 0);
        lv_obj_set_scroll_dir(pages[i], LV_DIR_VER);
    }
    lv_obj_remove_flag(traffic, LV_OBJ_FLAG_SCROLLABLE);
    s_list = lv_obj_create(traffic);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_list, draw_overview, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_list, list_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_list, list_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_list, list_event, LV_EVENT_CLICKED, NULL);
    create_empty_page(traffic);
    s_settings_payload[0] = 0;
    create_settings_page(settings);
    s_status_text = lv_label_create(status);
    lv_obj_set_width(s_status_text, LV_PCT(100));
    lv_obj_set_style_text_line_space(s_status_text, 3, 0);
    refresh_status();
}

static void refresh_rows(void)
{
    if (s_snapshot->count != 0) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        refresh_empty();
    }
    for (size_t i = 0; i < s_snapshot->count; ++i) {
        ip_info_visit(s_snapshot->rows[i].ip, s_snapshot->rows[i].last_seen_us);
    }
    lv_obj_invalidate(s_list);
    if (s_picker != NULL) lv_obj_invalidate(s_picker);
}

static void init_ui(i2c_master_bus_handle_t bus)
{
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &io_config, &io));
    const esp_lcd_touch_config_t config = {
        .x_max = LCD_WIDTH,
        .y_max = LCD_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC, // Reset/address selection already handled by CH422G.
        .int_gpio_num = GPIO_NUM_NC, // Poll; do not install an interrupt handler.
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(io, &config, &s_touch));

    s_snapshot = heap_caps_calloc(1, sizeof(*s_snapshot), MALLOC_CAP_SPIRAM);
    assert(s_snapshot != NULL);
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *display = lv_display_create(UI_WIDTH, UI_HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    const size_t buffer_bytes = UI_WIDTH * UI_DRAW_BUFFER_LINES * sizeof(uint16_t);
    void *buffer = heap_caps_malloc(buffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(buffer != NULL);
    lv_display_set_buffers(display, buffer, NULL, buffer_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, read_touch);
    create_ui();
}

void display_process(void)
{
    // Statistics and UI share whole-second packet slots. A late iteration
    // refreshes once without shifting the next boundary.
    uint64_t bucket = (uint64_t)esp_timer_get_time() / TRAFFIC_BUCKET_US;
    if (bucket != s_snapshot->now_us / TRAFFIC_BUCKET_US) {
        ESP_ERROR_CHECK(traffic_meter_snapshot(s_snapshot));
        refresh_rows();
        refresh_details();
        device_status_sample();
        refresh_status();
        refresh_settings();
    }
    ip_info_detail_t result;
    if (ip_info_poll(&result) && s_dialog != NULL &&
        result.info.request_id == s_lookup.request_id && result.info.ip == s_lookup.ip) {
        s_lookup = result.info;
        snprintf(s_abuse, sizeof(s_abuse), "%s", result.abuse);
        refresh_details();
    }
    lv_timer_handler();
}
