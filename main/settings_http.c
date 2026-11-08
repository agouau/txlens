#include "wifi_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "router.h"
#include "display.h"

static char s_setup_password[7];
static int64_t s_retry_after;
static struct ifreq s_ap_interface;
extern const char settings_html_start[] asm("_binary_settings_html_start");
extern const char settings_html_end[] asm("_binary_settings_html_end");

const char *wifi_settings_setup_password(void) { return s_setup_password; }

static bool allowed(httpd_req_t *req, bool authenticate)
{
    router_status_t status = router_get_status();
    struct sockaddr_in local;
    socklen_t size = sizeof(local);
    char host[32], expected[32];
    snprintf(expected, sizeof(expected), "%s:80", status.ap_ip);
    // Serve only through the downstream AP and its literal address, including before STA connects.
    bool ok = getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &size) == 0 &&
        local.sin_family == AF_INET && local.sin_addr.s_addr == inet_addr(status.ap_ip) &&
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK &&
        (!strcmp(host, status.ap_ip) || !strcmp(host, expected));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    if (!ok) httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Connect to the device hotspot to configure Wi-Fi.");
    if (ok && authenticate) {
        int64_t now = esp_timer_get_time();
        // Apply the interval before comparing any candidate, including correct codes.
        // Rejected requests do not extend the interval.
        if (now < s_retry_after) {
            httpd_resp_set_status(req, "429 Too Many Requests");
            httpd_resp_set_hdr(req, "Retry-After", "1");
            httpd_resp_sendstr(req, "Please wait half a second before trying again.");
            return false;
        }
        s_retry_after = now + 500000;
        char password[sizeof(s_setup_password)] = {0};
        bool received = httpd_req_get_hdr_value_str(req, "X-Setup-Password", password, sizeof(password)) == ESP_OK;
        unsigned difference = 0;
        for (size_t i = 0; i < sizeof(s_setup_password) - 1; ++i)
            difference |= (unsigned char)password[i] ^ (unsigned char)s_setup_password[i];
        if (!received || difference || !s_setup_password[0]) {
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Enter the setup code shown on the device screen.");
            return false;
        }
    }
    return ok;
}

static esp_err_t page(httpd_req_t *req)
{
    if (!allowed(req, false)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    return httpd_resp_send(req, settings_html_start, settings_html_end - settings_html_start - 1);
}

static esp_err_t state(httpd_req_t *req)
{
    if (!allowed(req, true)) return ESP_OK;
    wifi_settings_t settings = wifi_settings_get();
    router_status_t status = router_get_status();
    const char *connection = "Connecting to upstream Wi-Fi";
    switch (status.state) {
    case ROUTER_NOT_CONFIGURED: connection = "Upstream not configured"; break;
    case ROUTER_READY: connection = "Upstream ready"; break;
    case ROUTER_WAITING_IP: connection = "Waiting for upstream IP"; break;
    case ROUTER_SUBNET_CONFLICT: connection = "AP subnet conflict"; break;
    default: break;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    cJSON_AddStringToObject(root, "sta_ssid", settings.sta_ssid);
    cJSON_AddStringToObject(root, "ap_ssid", settings.ap_ssid);
    cJSON_AddBoolToObject(root, "sta_has_password", settings.sta_password[0] != 0);
    cJSON_AddBoolToObject(root, "saved", wifi_settings_saved());
    char key[ABUSEIPDB_KEY_CAPACITY];
    cJSON_AddBoolToObject(root, "abuseipdb_configured", abuseipdb_key_get(key));
    cJSON_AddStringToObject(root, "state", connection);
    cJSON_AddStringToObject(root, "ap_ip", status.ap_ip);
    cJSON_AddStringToObject(root, "sta_ip", status.sta_ip);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static void bmp_u32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (i * 8));
}

static esp_err_t screenshot(httpd_req_t *req)
{
    if (!allowed(req, false)) return ESP_OK;
    // Address-bar navigation cannot supply a custom header. Ask locally, then
    // fetch the image with the same authentication used by the settings APIs.
    if (!httpd_req_get_hdr_value_len(req, "X-Setup-Password")) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Content-Security-Policy",
            "default-src 'none'; script-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'");
        return httpd_resp_sendstr(req,
            "<!doctype html><meta charset=utf-8><title>LCD screenshot</title>"
            "<p id=m>Enter the setup code shown on the device screen to download a screenshot.</p>"
            "<script>(async()=>{const p=prompt('Setup code (6 digits from the device screen)');"
            "if(p===null)return;const m=document.getElementById('m');"
            "if(!/^[0-9]{6}$/.test(p)){m.textContent='Invalid code. Reload to try again.';return;}"
            "try{const r=await fetch('/api/screenshot.bmp',{headers:{'X-Setup-Password':p}});"
            "if(!r.ok){m.textContent=await r.text();return;}"
            "const u=URL.createObjectURL(await r.blob()),a=document.createElement('a');"
            "a.href=u;a.download='screenshot.bmp';document.body.append(a);a.click();a.remove();"
            "setTimeout(()=>URL.revokeObjectURL(u),60000);"
            "m.textContent='Screenshot downloaded. Reload to take another.';"
            "}catch(e){m.textContent='Download failed. Reload to retry.';}})();</script>");
    }
    if (!allowed(req, true)) return ESP_OK;
    uint16_t *pixels = NULL;
    int width, height;
    esp_err_t err = display_capture_rgb565(&pixels, &width, &height);
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not capture display. Please retry.");
    size_t stride = ((size_t)width * 3 + 3) & ~(size_t)3;
    uint8_t *row = calloc(1, stride);
    if (!row) {
        free(pixels);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    // Uncompressed 24-bit BGR BMP, negative height means top-down rows.
    uint8_t header[54] = {'B', 'M'};
    bmp_u32(header + 2, (uint32_t)(sizeof(header) + stride * height));
    bmp_u32(header + 10, sizeof(header));
    bmp_u32(header + 14, 40);
    bmp_u32(header + 18, (uint32_t)width);
    bmp_u32(header + 22, (uint32_t)-height);
    header[26] = 1;
    header[28] = 24;
    bmp_u32(header + 34, (uint32_t)(stride * height));
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"screenshot.bmp\"");
    err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    for (int y = 0; err == ESP_OK && y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint16_t color = pixels[(size_t)y * width + x];
            unsigned r = (color >> 11) & 31, g = (color >> 5) & 63, b = color & 31;
            row[x * 3] = (uint8_t)((b << 3) | (b >> 2));
            row[x * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
            row[x * 3 + 2] = (uint8_t)((r << 3) | (r >> 2));
        }
        err = httpd_resp_send_chunk(req, (const char *)row, stride);
    }
    free(row);
    free(pixels);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static bool receive_body(httpd_req_t *req, char *body, size_t capacity)
{
    if (!req->content_len || req->content_len >= capacity) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request size");
        return false;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) {
            if (count == HTTPD_SOCK_ERR_TIMEOUT)
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Request timed out");
            return false;
        }
        received += count;
    }
    body[received] = 0;
    if (memchr(body, 0, received)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return false;
    }
    return true;
}

static esp_err_t save(httpd_req_t *req)
{
    if (!allowed(req, true)) return ESP_OK;
    char body[1537];
    if (!receive_body(req, body, sizeof(body))) return ESP_FAIL;
    wifi_settings_t settings = wifi_settings_get();
    if (!wifi_settings_parse(body, &settings))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "Invalid settings. SSID: 1-32 bytes (STA may be empty). Password: 8-63 bytes (STA may be empty).");
    esp_err_t err = wifi_settings_save(&settings);
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings. Please retry.");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"message\":\"Saved. Restarting in 2 seconds. Reconnect using the device screen, then reopen this page.\"}");
}

static esp_err_t save_abuseipdb(httpd_req_t *req)
{
    if (!allowed(req, true)) return ESP_OK;
    char body[1025], key[ABUSEIPDB_KEY_CAPACITY];
    if (!receive_body(req, body, sizeof(body))) return ESP_FAIL;
    if (!abuseipdb_key_parse(body, key))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "API key: up to 128 ASCII characters without spaces; empty disables lookup.");
    if (abuseipdb_key_save(key) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save API key. Please retry.");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, key[0] ? "{\"message\":\"API key saved. Reopen IP details to query.\"}" :
                                          "{\"message\":\"API key removed. AbuseIPDB lookup disabled.\"}");
}

esp_err_t wifi_settings_http_start(void)
{
    uint32_t random;
    do { random = esp_random(); } while (random >= UINT32_MAX - UINT32_MAX % 1000000U);
    snprintf(s_setup_password, sizeof(s_setup_password), "%06lu", (unsigned long)(random % 1000000U));
    s_retry_after = 0;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_netif_get_netif_impl_name(ap, s_ap_interface.ifr_name);
    if (err != ESP_OK) return err;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.if_name = &s_ap_interface;
    config.stack_size = 8192;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    httpd_handle_t server;
    err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = state},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = save},
        {.uri = "/api/abuseipdb", .method = HTTP_POST, .handler = save_abuseipdb},
        {.uri = "/api/screenshot.bmp", .method = HTTP_GET, .handler = screenshot},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) { httpd_stop(server); return err; }
    }
    return ESP_OK;
}
