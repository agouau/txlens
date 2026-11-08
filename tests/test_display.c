// Headless interaction tests using the real LVGL library and extracted UI code.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "src/libs/qrcode/qrcodegen.h"
#include "traffic_meter.h"
#include "ip_info_parse.h"
#include "router.h"
#include "wifi_settings.h"
#include "device_status.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)
#define ESP_ERROR_CHECK(error) CHECK((error) == ESP_OK)
#undef UI_ROTATE_CCW_90
#define UI_ROTATE_CCW_90 TEST_UI_ROTATION

static uint32_t test_generation;
static unsigned test_requests;
static ip_info_detail_t test_result;
static bool test_result_pending;
static const char *test_clock = "Not synchronized";
static bool test_settings_saved = true;
static bool test_abuse_configured;
static unsigned test_status_samples;
static router_status_t test_router = {.state = ROUTER_READY, .clients = 0, .ap_ip = "192.168.50.1"};
void device_status_sample(void) { ++test_status_samples; }
void device_status_format(char *text, size_t size)
{
    snprintf(text, size,
             "CPU: C0 %u%%, C1 10%%\n"
             "RAM: 100.0 / 300.0 KB\n"
             "PSRAM: 2.00 / 8.00 MB\n"
             "Flash: 3.49 / 16.00 MB\nFirmware: 1322 / 1500 KB\n\n"
             "SD card: Ready\nUsage: 1.25 / 32.00 GB\nLogging: Recording\n\n"
             "STA IP: 192.168.1.20\nAP: Test hotspot\nAP IP: 192.168.50.1\nClients: 1\n\nTime: %s",
             test_status_samples, test_clock);
}
wifi_settings_t wifi_settings_get(void)
{
    return (wifi_settings_t){.ap_ssid = "Test hotspot", .ap_password = "testpass123"};
}
const char *wifi_settings_setup_password(void) { return "123456"; }
bool wifi_settings_saved(void) { return test_settings_saved; }
bool device_settings_configured(void) { return test_settings_saved || test_abuse_configured; }
static lv_indev_data_t test_pointer;
static uint8_t test_frame[UI_WIDTH * UI_HEIGHT * 3];

static uint32_t tick_ms(void) { return lv_tick_get(); }
void ip_info_visit(uint32_t ip, uint64_t last_seen_us) { (void)ip; (void)last_seen_us; }
static const char *test_domain = "";
bool ip_info_name(uint32_t ip, char name[256]) { (void)ip; snprintf(name, 256, "%s", test_domain); return name[0] != 0; }
uint32_t ip_info_request(uint32_t ip) { (void)ip; ++test_requests; return ++test_generation; }
void ip_info_cancel(void) { ++test_generation; }
bool ip_info_poll(ip_info_detail_t *result)
{
    if (!test_result_pending) return false;
    *result = test_result;
    test_result_pending = false;
    return true;
}
static unsigned test_snapshots;
static int64_t esp_timer_get_time(void) { return (int64_t)tick_ms() * 1000; }
esp_err_t traffic_meter_snapshot(traffic_snapshot_t *snapshot)
{
    ++test_snapshots;
    snapshot->now_us = (uint64_t)esp_timer_get_time();
    return ESP_OK;
}
router_status_t router_get_status(void)
{
    return test_router;
}



#include "rgb_rotation.inc"
typedef int esp_lcd_panel_handle_t;
typedef int esp_lcd_touch_handle_t;
typedef struct { uint16_t x, y; } esp_lcd_touch_point_data_t;
static int s_panel;
// Host tests run serially; the device serializes snapshot copies and flushes.
static int s_frame_lock;
#define portMAX_DELAY 0
static void xSemaphoreTake(int lock, int timeout) { (void)lock; (void)timeout; }
static void xSemaphoreGive(int lock) { (void)lock; }
typedef struct { int rotate_mask; int x_gap; int y_gap; } test_panel_t;
static test_panel_t test_panel;
static uint16_t test_panel_frame[LCD_WIDTH * LCD_HEIGHT];
static lv_area_t test_panel_area;
static uint16_t test_panel_pixels[6];
static esp_err_t esp_lcd_panel_swap_xy(int panel, bool swap)
{
    (void)panel;
    test_panel.rotate_mask = swap ? ROTATE_MASK_SWAP_XY : 0;
    return ESP_OK;
}
static esp_err_t esp_lcd_panel_mirror(int panel, bool x, bool y)
{
    (void)panel;
    if (x) test_panel.rotate_mask |= ROTATE_MASK_MIRROR_X;
    if (y) test_panel.rotate_mask |= ROTATE_MASK_MIRROR_Y;
    return ESP_OK;
}
static esp_err_t esp_lcd_panel_set_gap(int panel, int x, int y)
{
    (void)panel;
    test_panel.x_gap = x;
    test_panel.y_gap = y;
    return ESP_OK;
}
static esp_err_t esp_lcd_panel_draw_bitmap(int panel, int x_start, int y_start,
                                         int x_end, int y_end, const void *pixels)
{
    (void)panel;
    test_panel_area = (lv_area_t){x_start, y_start, x_end - 1, y_end - 1};
    CHECK((x_end - x_start) * (y_end - y_start) == 6);
    memcpy(test_panel_pixels, pixels, sizeof(test_panel_pixels));
    // Same inputs and copy macro as ESP-IDF rgb_panel_draw_bitmap().
    x_start += test_panel.x_gap;
    x_end += test_panel.x_gap;
    y_start += test_panel.y_gap;
    y_end += test_panel.y_gap;
    const int h_res = LCD_WIDTH, v_res = LCD_HEIGHT, bytes_per_pixel = 2;
    uint32_t bytes_per_line = h_res * bytes_per_pixel;
    uint32_t copy_bytes_per_line = (x_end - x_start) * bytes_per_pixel;
    size_t offset = y_start * copy_bytes_per_line + x_start * bytes_per_pixel;
    uint8_t *fb = (uint8_t *)test_panel_frame, *to = fb, *flush_ptr = fb;
    const uint8_t *from = pixels;
    size_t bytes_to_flush = sizeof(test_panel_frame);
    test_panel_t *rgb_panel = &test_panel;
    COPY_PIXEL_CODE_BLOCK(16)
    (void)flush_ptr;
    (void)bytes_to_flush;
    return ESP_OK;
}
static esp_err_t esp_lcd_touch_read_data(int touch) { (void)touch; return ESP_OK; }
static esp_err_t esp_lcd_touch_get_data(int touch, esp_lcd_touch_point_data_t *point,
                                      uint8_t *count, int max_count)
{
    (void)touch;
    (void)max_count;
    point->x = test_pointer.point.x;
    point->y = test_pointer.point.y;
    *count = test_pointer.state == LV_INDEV_STATE_PRESSED ? 1 : 0;
    return ESP_OK;
}

#include "display_ui.inc"

static void test_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    CHECK(area->x1 == 0 && area->y1 == 0 && area->x2 == UI_WIDTH - 1 && area->y2 == UI_HEIGHT - 1);
    memcpy(test_frame, pixels, sizeof(test_frame));
    lv_display_flush_ready(display);
}

static void check_fonts(lv_obj_t *obj)
{
    const lv_font_t *font = lv_obj_check_type(obj, &lv_label_class) &&
                           !strcmp(lv_label_get_text(obj), "Tx Lens") ?
                           &lv_font_montserrat_24 : &app_font_mono_12;
    CHECK(lv_obj_get_style_text_font(obj, LV_PART_MAIN) == font);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        check_fonts(lv_obj_get_child(obj, i));
}

static void dump_frame(const char *name)
{
    check_fonts(lv_screen_active());
    check_fonts(lv_layer_top());
    lv_refr_now(NULL);
    FILE *file = fopen(name, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(test_frame, 1, sizeof(test_frame), file) == sizeof(test_frame));
    fclose(file);
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    read_touch(indev, data);
}

static void point(lv_indev_t *input, int x, int y, bool pressed)
{
#if UI_ROTATE_CCW_90
    test_pointer.point = (lv_point_t){y, UI_WIDTH - 1 - x};
#else
    test_pointer.point = (lv_point_t){x, y};
#endif
    test_pointer.state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_tick_inc(40);
    lv_indev_read(input);
    lv_timer_handler();
}

static void tap(lv_indev_t *input, int x, int y)
{
    point(input, x, y, true);
    point(input, x, y, false);
}

static void close_popup(void)
{
    lv_obj_t *footer = lv_msgbox_get_footer(s_dialog);
    lv_obj_t *button = lv_obj_get_child(footer, 0);
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    lv_tick_inc(40);
    lv_timer_handler();
    CHECK(s_dialog == NULL);
}

int main(void)
{
    lv_init();
    char address[256];
    format_row_address(0x08080808, address);
    CHECK(strcmp(address, "8.8.8.8") == 0);
    test_domain = "example.com";
    format_row_address(0x08080808, address);
    CHECK(strcmp(address, test_domain) == 0);
    test_domain = "api.example.com";
    format_row_address(0x08080808, address);
    CHECK(strcmp(address, test_domain) == 0);
    test_domain = "a-very-long-subdomain.of-another-long-domain.example.com";
    format_row_address(0x08080808, address);
    CHECK(address[0] == '*');
    CHECK(strcmp(address + strlen(address) - 11, "example.com") == 0);
    lv_point_t measured;
    lv_text_get_size(&measured, address, &app_font_mono_12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    CHECK(measured.x <= UI_WIDTH * 3 / 5);
    CHECK(measured.x > UI_WIDTH * 3 / 5 - 8);
    int digit_width = lv_text_get_width("0", 1, &app_font_mono_12, 0);
    size_t digits = ROW_ADDRESS_WIDTH / digit_width;
    char edge_domain[256];
    CHECK(digits + 2 <= sizeof(edge_domain));
    memset(edge_domain, '0', digits);
    edge_domain[digits] = 0;
    test_domain = edge_domain;
    format_row_address(0x08080808, address);
    CHECK(strcmp(address, test_domain) == 0);
    edge_domain[digits] = '1';
    edge_domain[digits + 1] = 0;
    format_row_address(0x08080808, address);
    CHECK(address[0] == '*' && strcmp(address + 1, edge_domain + 2) == 0);
    test_domain = "";
    lv_display_t *display = lv_display_create(UI_WIDTH, UI_HEIGHT);
    configure_panel_rotation(s_panel);
    // Pixel order stays untouched until the RGB driver copies to its framebuffer.
    uint16_t source[] = {1, 2, 3, 4, 5, 6};
    lv_area_t partial = {10, 20, 12, 21};
    flush(display, &partial, (uint8_t *)source);
    CHECK(memcmp(test_panel_pixels, source, sizeof(source)) == 0);
    CHECK(test_panel_area.x1 == 10 && test_panel_area.x2 == 12);
    CHECK(test_panel_area.y1 == 20 && test_panel_area.y2 == 21);
    for (int y = 0; y < LCD_HEIGHT; ++y) {
        for (int x = 0; x < LCD_WIDTH; ++x) {
            uint16_t expected = 0;
            for (int v = 0; v < 2; ++v) {
                for (int u = 0; u < 3; ++u) {
#if UI_ROTATE_CCW_90
                    int px = 20 + v, py = UI_WIDTH - 1 - (10 + u);
#else
                    int px = 10 + u, py = 20 + v;
#endif
                    if (x == px && y == py) expected = source[v * 3 + u];
                }
            }
            CHECK(test_panel_frame[y * LCD_WIDTH + x] == expected);
        }
    }
    // Real touch callback: all four logical corners and inactive physical area.
    for (int y = 0; y < UI_HEIGHT; y += UI_HEIGHT - 1) {
        for (int x = 0; x < UI_WIDTH; x += UI_WIDTH - 1) {
#if UI_ROTATE_CCW_90
            test_pointer.point = (lv_point_t){y, UI_WIDTH - 1 - x};
#else
            test_pointer.point = (lv_point_t){x, y};
#endif
            test_pointer.state = LV_INDEV_STATE_PRESSED;
            lv_indev_data_t data = {0};
            read_touch(NULL, &data);
            CHECK(data.state == LV_INDEV_STATE_PRESSED && data.point.x == x && data.point.y == y);
        }
    }
    test_pointer.point = (lv_point_t){799, 479};
    lv_indev_data_t outside = {0};
    read_touch(NULL, &outside);
    CHECK(outside.state == LV_INDEV_STATE_RELEASED);
    test_pointer.state = LV_INDEV_STATE_RELEASED;


    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    void *buffer = malloc(sizeof(test_frame));
    CHECK(buffer != NULL);
    lv_display_set_buffers(display, buffer, NULL, sizeof(test_frame), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, test_flush);
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, pointer_read);
    s_snapshot = calloc(1, sizeof(*s_snapshot));
    CHECK(s_snapshot != NULL);
    create_ui();
    lv_obj_update_layout(lv_screen_active());
    lv_area_t empty;
    lv_obj_get_coords(s_empty, &empty);
    lv_area_t list;
    lv_obj_get_coords(s_list, &list);
    CHECK(abs(empty.x1 + empty.x2 - list.x1 - list.x2) <= 1);
    CHECK(abs(empty.y1 + empty.y2 - list.y1 - list.y2) <= 1);
    CHECK(strstr(lv_label_get_text(s_empty_message), "Connect a device") != NULL);
    lv_obj_t *home_content = lv_obj_get_child(s_empty, 0);
    CHECK(!strcmp(lv_label_get_text(lv_obj_get_child(home_content, 1)), "See where your data goes"));
    CHECK(!strcmp(lv_label_get_text(lv_obj_get_child(home_content, 2)), "https://txlens.pages.dev"));
    CHECK(strstr(lv_label_get_text(s_empty_status), "Upstream: Connected") != NULL);
    CHECK(lv_obj_has_flag(lv_obj_get_parent(s_empty_hint), LV_OBJ_FLAG_HIDDEN));
    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(s_tabs);
    CHECK(strcmp(lv_label_get_text(lv_obj_get_child(lv_obj_get_child(tab_bar, 0), 0)), "TX Traffic") == 0);
    lv_area_t arrow, settings_button, hero, guide;
    lv_obj_get_coords(lv_obj_get_child(s_empty, 0), &hero);
    int configured_hero_top = hero.y1;
    CHECK(hero.y1 >= empty.y1 && hero.y2 <= empty.y2);
    dump_frame("empty.bgr");
    test_router.clients = 1;
    refresh_rows();
    CHECK(strstr(lv_label_get_text(s_empty_status), "Device: Connected") != NULL);
    CHECK(!strcmp(lv_label_get_text(s_empty_message), "Outgoing traffic appears here."));
    test_router.state = ROUTER_CONNECTING;
    refresh_rows();
    CHECK(strstr(lv_label_get_text(s_empty_status), "Upstream: Connecting") != NULL);
    test_router.state = ROUTER_READY;
    test_router.clients = 0;

    // Refresh follows slot boundaries, even after a delayed UI iteration.
    unsigned snapshots = test_snapshots;
    unsigned slot_samples = test_status_samples;
    uint32_t boundary = ((tick_ms() / 1000) + 1) * 1000;
    lv_tick_inc(boundary - tick_ms() - 1);
    s_snapshot->now_us = (uint64_t)esp_timer_get_time();
    display_process();
    CHECK(test_snapshots == snapshots);
    CHECK(test_status_samples == slot_samples);
    lv_tick_inc(1);
    display_process();
    CHECK(test_snapshots == snapshots + 1);
    CHECK(test_status_samples == slot_samples + 1);
    lv_tick_inc(999);
    display_process();
    CHECK(test_snapshots == snapshots + 1);
    CHECK(test_status_samples == slot_samples + 1);
    lv_tick_inc(17); // Slightly late: do not shift subsequent boundaries.
    display_process();
    CHECK(test_snapshots == snapshots + 2);
    CHECK(test_status_samples == slot_samples + 2);
    lv_tick_inc(984);
    display_process();
    CHECK(test_snapshots == snapshots + 3);
    CHECK(test_status_samples == slot_samples + 3);
    lv_tick_inc(3200); // Skip several slots: refresh once, then return to the boundary.
    display_process();
    CHECK(test_snapshots == snapshots + 4);
    CHECK(test_status_samples == slot_samples + 4);
    lv_tick_inc(800);
    display_process();
    CHECK(test_snapshots == snapshots + 5);
    CHECK(test_status_samples == slot_samples + 5);

    s_snapshot->count = LV_MIN(6, (UI_HEIGHT - 24) / 44);
    size_t sample_count = s_snapshot->count;
    int first_y = 12 + 44 / 2;
    s_snapshot->now_us = 30000000;
    for (size_t i = 0; i < s_snapshot->count; ++i) {
        traffic_row_t *row = &s_snapshot->rows[i];
        row->ip = 0x08080808 + (uint32_t)(i << 24);
        row->last_seen_us = 10000000 + i * 500000;
        for (size_t j = 0; j < TRAFFIC_BUCKET_COUNT; ++j) {
            row->bytes[j] = j % 55 < 25 ? (uint32_t)((i + 1) * (j % 25 + 1) * 100) : 0;
        }
    }
    refresh_rows();
    lv_obj_update_layout(lv_screen_active());
    dump_frame("traffic.bgr");
    tap(input, UI_WIDTH / 2, 12 + (int)sample_count * 44 + 10);
    CHECK(s_dialog == NULL && s_picker == NULL && test_requests == 0);
    tap(input, UI_WIDTH / 2, first_y);
    CHECK(s_dialog != NULL && s_lookup.ip == s_snapshot->rows[0].ip && test_requests == 1);
    CHECK(strncmp(lv_label_get_text(s_details), "TX (60s):", 9) == 0);
    CHECK(strstr(lv_label_get_text(s_details), "ISP: --\nOrg: --\nASN: --") != NULL);
    strcpy(s_lookup.ptr, "dns.google");
    strcpy(s_lookup.location, "United States / California / Mountain View");
    strcpy(s_lookup.network, "ISP: Google LLC\nOrg: Google LLC\nASN: AS15169");
    refresh_details();
    CHECK(strncmp(lv_label_get_text(s_details), "dns.google\n\nTX (60s):", 21) == 0);
    CHECK(strstr(lv_label_get_text(s_details), "Last upload") == NULL);
    CHECK(strstr(lv_label_get_text(s_details), "ASN: AS15169\nLocation:") != NULL);
    dump_frame("details.bgr");
    lv_obj_t *footer = lv_msgbox_get_footer(s_dialog);
    CHECK(lv_obj_get_child_count(footer) == 1);
    lv_area_t button_area[1];
    for (uint32_t i = 0; i < 1; ++i) {
        lv_obj_t *button = lv_obj_get_child(footer, i);
        lv_obj_get_coords(button, &button_area[i]);
        CHECK(lv_area_get_height(&button_area[i]) >= 48);
        CHECK(button_area[i].y2 < UI_HEIGHT - 12);
        lv_area_t label_area;
        lv_obj_get_coords(lv_obj_get_child(button, 0), &label_area);
        CHECK(abs(label_area.x1 + label_area.x2 - button_area[i].x1 - button_area[i].x2) <= 1);
        CHECK(abs(label_area.y1 + label_area.y2 - button_area[i].y1 - button_area[i].y2) <= 1);
    }
    s_lookup.ptr[0] = 0;
    refresh_details();
    CHECK(strncmp(lv_label_get_text(s_details), "TX (60s):", 9) == 0);
    uint32_t old_id = s_lookup.request_id;
    tap(input, (button_area[0].x1 + button_area[0].x2) / 2,
        (button_area[0].y1 + button_area[0].y2) / 2);
    CHECK(s_dialog == NULL);

    // Dragging the overview must neither scroll nor query details.
    point(input, UI_WIDTH / 2, UI_HEIGHT * 4 / 5, true);
    point(input, UI_WIDTH / 2, UI_HEIGHT * 3 / 5, true);
    point(input, UI_WIDTH / 2, UI_HEIGHT * 2 / 5, true);
    point(input, UI_WIDTH / 2, UI_HEIGHT / 5, true);
    point(input, UI_WIDTH / 2, UI_HEIGHT / 5, false);
    CHECK(s_dialog == NULL && test_requests == 1);
    CHECK(lv_obj_get_scroll_y(s_list) == 0);
    lv_anim_delete_all();
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());

    // Expiry/reordering between press and release cannot open a different IP.
    point(input, UI_WIDTH / 2, first_y, true);
    s_snapshot->rows[0].ip = 0x01010101;
    refresh_rows();
    point(input, UI_WIDTH / 2, first_y, false);
    CHECK(s_dialog == NULL && test_requests == 1);
    tap(input, UI_WIDTH / 2, first_y);
    CHECK(s_dialog != NULL && test_requests == 2);
    test_result = (ip_info_detail_t){ .info = {.ip = 0x08080808, .request_id = old_id} };
    strcpy(test_result.info.ptr, "STALE RESULT");
    strcpy(test_result.abuse, "STALE ABUSE");
    test_result_pending = true;
    display_process();
    CHECK(s_lookup.ip == 0x01010101 && strcmp(s_lookup.ptr, "STALE RESULT") != 0);
    CHECK(strcmp(s_abuse, "STALE ABUSE") != 0);
    test_result = (ip_info_detail_t){ .info = {.ip = s_lookup.ip, .request_id = s_lookup.request_id} };
    strcpy(test_result.info.ptr, "one.one.one.one");
    strcpy(test_result.abuse, "AbuseIPDB (optional)\nAdd an API key via Settings to enable lookup.");
    test_result_pending = true;
    display_process();
    CHECK(strcmp(s_lookup.ptr, "one.one.one.one") == 0);
    CHECK(strstr(lv_label_get_text(s_details), "API key via Settings"));
    close_popup();
    // At maximum capacity all 128 rows fit; a dense tap only opens choices.
    s_snapshot->count = TRAFFIC_MAX_IPS;
    for (size_t i = 2; i < s_snapshot->count; ++i) {
        s_snapshot->rows[i] = s_snapshot->rows[i % sample_count];
        s_snapshot->rows[i].ip = 0x08080808 + (uint32_t)(i << 24);
    }
    refresh_rows();
    lv_obj_update_layout(lv_screen_active());
    CHECK(lv_obj_get_scroll_bottom(s_list) <= 0);
    CHECK(lv_obj_get_scroll_top(s_list) <= 0);
    CHECK(!lv_obj_has_flag(s_list, LV_OBJ_FLAG_SCROLLABLE));
    dump_frame("dense.bgr");
    unsigned requests = test_requests;
    // The native panel's unused right side has no interactive UI.
    tap(input, UI_WIDTH + 10, UI_HEIGHT / 2);
    CHECK(s_picker == NULL && s_dialog == NULL && test_requests == requests);
    tap(input, UI_WIDTH / 2, UI_HEIGHT / 2);
    CHECK(s_picker != NULL && s_dialog == NULL && test_requests == requests);
    CHECK(s_picker_count > 1 && s_picker_count <= (UI_HEIGHT - 144) / 20 + 1);
    lv_obj_t *content = lv_msgbox_get_content(s_picker);
    lv_obj_update_layout(s_picker);
    CHECK(lv_obj_get_scroll_bottom(content) <= 0);
    lv_area_t popup_area;
    lv_obj_get_coords(s_picker, &popup_area);
    CHECK(popup_area.x1 >= 0 && popup_area.x2 < UI_WIDTH);
    CHECK(popup_area.y1 >= 0 && popup_area.y2 < UI_HEIGHT);
    lv_area_t last_choice;
    lv_obj_get_coords(lv_obj_get_child(content, s_picker_count - 1), &last_choice);
    CHECK(last_choice.y2 < UI_HEIGHT);
    dump_frame("choices.bgr");
    uint32_t choice = s_picker_ips[0];
    // Reorder the live list while the picker is open: choices retain their IPs.
    traffic_row_t swap = s_snapshot->rows[0];
    s_snapshot->rows[0] = s_snapshot->rows[64];
    s_snapshot->rows[64] = swap;
    refresh_rows();
    lv_area_t choice_area;
    lv_obj_get_coords(lv_obj_get_child(content, 0), &choice_area);
    tap(input, choice_area.x1 + 20, (choice_area.y1 + choice_area.y2) / 2);
    CHECK(s_picker == NULL && s_dialog != NULL && s_lookup.ip == choice);
    CHECK(test_requests == requests + 1);
    close_popup();

    // Both screen edges remain selectable, including the last IP.
    tap(input, 32, lv_obj_get_y(s_list) + lv_obj_get_height(s_list) - 1);
    CHECK(s_picker != NULL);
    CHECK(s_picker_ips[s_picker_count - 1] == s_snapshot->rows[TRAFFIC_MAX_IPS - 1].ip);
    close_picker(NULL);
    lv_tick_inc(40);
    lv_timer_handler();
    tap(input, UI_WIDTH / 2, 12);
    CHECK(s_picker != NULL && s_picker_ips[0] == s_snapshot->rows[0].ip);
    close_picker(NULL);
    lv_tick_inc(40);
    lv_timer_handler();
    CHECK(test_requests == requests + 1);

    // Shrinking back to one IP restores direct detail selection.
    s_snapshot->count = 1;
    refresh_rows();
    tap(input, UI_WIDTH / 2, UI_HEIGHT / 2);
    CHECK(s_picker == NULL && s_dialog == NULL);
    tap(input, UI_WIDTH / 2, first_y);
    CHECK(s_picker == NULL && s_dialog != NULL);
    close_popup();
    s_snapshot->count = 0;
    refresh_rows();
    CHECK(!lv_obj_has_flag(s_empty, LV_OBJ_FLAG_HIDDEN));
    tap(input, UI_WIDTH / 2, UI_HEIGHT / 2);
    CHECK(s_dialog == NULL && s_picker == NULL);
    // The three bottom tabs replace setup/status popups and stay below traffic.
    CHECK(lv_tabview_get_tab_count(s_tabs) == 3);
    CHECK(lv_tabview_get_tab_active(s_tabs) == 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tabs);
    lv_area_t bar_area, list_area;
    lv_obj_get_coords(bar, &bar_area);
    lv_obj_get_coords(s_list, &list_area);
    CHECK(list_area.y2 < bar_area.y1);
    tap(input, UI_WIDTH / 2, (bar_area.y1 + bar_area.y2) / 2);
    lv_anim_delete_all();
    lv_obj_update_layout(s_tabs);
    CHECK(lv_tabview_get_tab_active(s_tabs) == 1);
    CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
    CHECK(strcmp(s_settings_payload, "http://192.168.50.1/#password=123456") == 0);
    const char *setup_text = lv_label_get_text(s_settings_text);
    CHECK(strstr(setup_text, "1. Join device hotspot") != NULL);
    CHECK(strstr(setup_text, "Test hotspot") != NULL);
    CHECK(strstr(setup_text, "testpass123") != NULL);
    CHECK(strstr(setup_text, "2. Open settings") != NULL);
    CHECK(strstr(setup_text, "Setup code: 123456"));
    lv_obj_t *surround = lv_obj_get_parent(s_settings_qr);
    int modules = qrcodegen_version2size(qrcodegen_getMinFitVersion(
                        qrcodegen_Ecc_MEDIUM, strlen(s_settings_payload)));
    int scale = lv_obj_get_width(s_settings_qr) / modules;
    CHECK(lv_obj_get_width(s_settings_qr) == modules * scale);
    CHECK(lv_obj_get_width(surround) == (modules + 2) * scale);
    CHECK(lv_obj_get_height(surround) == (modules + 2) * scale);
    lv_area_t qr_area;
    lv_obj_get_coords(surround, &qr_area);
    CHECK(abs(qr_area.x1 + qr_area.x2 - (UI_WIDTH - 1)) <= 1);
    lv_area_t code_area;
    lv_obj_get_coords(s_settings_qr, &code_area);
    CHECK(code_area.x1 - qr_area.x1 == scale && qr_area.x2 - code_area.x2 == scale);
    CHECK(code_area.y1 - qr_area.y1 == scale && qr_area.y2 - code_area.y2 == scale);
    CHECK(qr_area.y2 < bar_area.y1);
    dump_frame("settings.bgr");
    tap(input, UI_WIDTH * 5 / 6, (bar_area.y1 + bar_area.y2) / 2);
    lv_anim_delete_all();
    lv_obj_update_layout(s_tabs);
    CHECK(lv_tabview_get_tab_active(s_tabs) == 2);
    CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
    CHECK(strstr(lv_label_get_text(s_status_text), "CPU: C0") != NULL);
    CHECK(strstr(lv_label_get_text(s_status_text), "AP: Test hotspot") != NULL);
    CHECK(strstr(lv_label_get_text(s_status_text), "Time: Not synchronized") != NULL);
    CHECK(strstr(lv_label_get_text(s_status_text), "SD card: Ready\nUsage: 1.25 / 32.00 GB\nLogging: Recording") != NULL);
    CHECK(lv_obj_get_scroll_bottom(lv_obj_get_parent(s_status_text)) <= 0);
    test_clock = "2026-11-04 12:34:56 UTC-8";
    unsigned samples = test_status_samples;
    lv_tick_inc(1100);
    display_process();
    CHECK(test_status_samples > samples);
    char load[40];
    snprintf(load, sizeof(load), "C0 %u%%", test_status_samples);
    CHECK(strstr(lv_label_get_text(s_status_text), load) != NULL);
    CHECK(strstr(lv_label_get_text(s_status_text), test_clock) != NULL);
    dump_frame("status.bgr");
    // Returning to traffic keeps live rows and does not launch a query.
    s_snapshot->count = 1;
    refresh_rows();
    requests = test_requests;
    tap(input, UI_WIDTH / 6, (bar_area.y1 + bar_area.y2) / 2);
    lv_anim_delete_all();
    lv_obj_update_layout(s_tabs);
    CHECK(lv_tabview_get_tab_active(s_tabs) == 0);
    CHECK(s_dialog == NULL && s_picker == NULL && test_requests == requests);
    tap(input, UI_WIDTH / 2, first_y);
    CHECK(s_dialog != NULL);
    close_popup();
    // First boot stays on the home tab and points to Settings without a modal.
    test_settings_saved = false;
    test_router.state = ROUTER_NOT_CONFIGURED;
    s_snapshot->count = 0;
    lv_obj_clean(lv_screen_active());
    create_ui();
    lv_obj_update_layout(s_tabs);
    CHECK(lv_tabview_get_tab_active(s_tabs) == 0);
    CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
    CHECK(strstr(lv_label_get_text(s_empty_hint), "start using Tx Lens") != NULL);
    CHECK(strstr(lv_label_get_text(s_empty_status), "Upstream: Set up Wi-Fi") != NULL);
    CHECK(!lv_obj_has_flag(lv_obj_get_parent(s_empty_hint), LV_OBJ_FLAG_HIDDEN));
    tab_bar = lv_tabview_get_tab_bar(s_tabs);
    lv_obj_get_coords(s_empty_arrow, &arrow);
    lv_obj_get_coords(lv_obj_get_child(tab_bar, 1), &settings_button);
    lv_obj_get_coords(lv_obj_get_child(s_empty, 0), &hero);
    lv_obj_get_coords(lv_obj_get_parent(s_empty_arrow), &guide);
    CHECK(hero.y1 < configured_hero_top);
    CHECK(abs(arrow.x1 + arrow.x2 - settings_button.x1 - settings_button.x2) <= 2);
    CHECK(arrow.y2 < settings_button.y1 && hero.y2 < guide.y1);
    CHECK(hero.y1 >= empty.y1 && guide.y2 <= empty.y2);
    dump_frame("first_boot.bgr");
    int first_boot_top = hero.y1;
    test_abuse_configured = true; // Save only the key without restarting the UI.
    lv_tick_inc(1000);
    display_process();
    lv_obj_update_layout(s_tabs);
    CHECK(lv_obj_has_flag(s_empty_guide, LV_OBJ_FLAG_HIDDEN));
    lv_obj_get_coords(s_empty_hero, &hero);
    CHECK(hero.y1 > first_boot_top);
    test_abuse_configured = false; // With neither setting present, show onboarding.
    lv_tick_inc(1000);
    display_process();
    CHECK(!lv_obj_has_flag(s_empty_guide, LV_OBJ_FLAG_HIDDEN));
    printf("PASS: CCW90=%d, driver pixel rotation, touch corners, unused area, UI interaction\n", UI_ROTATE_CCW_90);
    return 0;
}
