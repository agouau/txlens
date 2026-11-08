#include "ip_info.h"
#include "ip_info_parse.h"
#include "app_clock.h"
#include "app_config.h"
#include "ip_cache.h"
#include "wifi_settings.h"

#include <stdatomic.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "router.h"

#define RESPONSE_CAPACITY 4096
#define LOOKUP_ERROR_CAPACITY 96

static const char *TAG = "ip_info";

typedef struct {
    uint32_t ip;
    uint32_t id;
} request_t;

typedef struct {
    uint32_t ip;
    char name[256];
    uint64_t received_us;
} observed_name_t;

static QueueHandle_t s_requests;
static QueueHandle_t s_results;
static QueueHandle_t s_names;
static SemaphoreHandle_t s_cache_lock;
static ip_cache_t s_cache;
static atomic_uint s_generation;

_Static_assert(IP_INFO_CACHE_COUNT > 0, "IP cache must have entries");

static int64_t cache_utc(void)
{
    return app_clock_is_ready() ? (int64_t)time(NULL) : 0;
}

static void drain_names(void)
{
    observed_name_t observed;
    while (xQueueReceive(s_names, &observed, 0) == pdTRUE) {
        xSemaphoreTake(s_cache_lock, portMAX_DELAY);
        ip_cache_entry_t *entry = ip_cache_acquire(&s_cache, observed.ip);
        strcpy(entry->info.result.ptr, observed.name);
        entry->info.dns_checked = true;
        entry->info.dns_expires_at = 0;
        entry->dns_expires_us = observed.received_us + IP_CACHE_DNS_TTL_SECONDS * 1000000ULL;
        ip_cache_expire_dns(entry, (uint64_t)esp_timer_get_time(), cache_utc());
        ip_cache_touch(&s_cache, entry);
        xSemaphoreGive(s_cache_lock);
    }
}

void ip_info_observe_name(uint32_t ip, const char *name)
{
    if (!s_names || !ip_info_is_public(ip) || !name[0]) return;
    observed_name_t observed = {.ip = ip, .received_us = (uint64_t)esp_timer_get_time()};
    snprintf(observed.name, sizeof(observed.name), "%s", name);
    xQueueSend(s_names, &observed, 0);
}

void ip_info_visit(uint32_t ip, uint64_t last_seen_us)
{
    if (!ip_info_is_public(ip)) return;
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    ip_cache_entry_t *entry = ip_cache_acquire(&s_cache, ip);
    ip_cache_expire_dns(entry, (uint64_t)esp_timer_get_time(), cache_utc());
    ip_cache_expire_provider(entry, cache_utc());
    if (entry->last_seen_us != last_seen_us) {
        entry->last_seen_us = last_seen_us;
        ip_cache_touch(&s_cache, entry);
    }
    xSemaphoreGive(s_cache_lock);
}

bool ip_info_name(uint32_t ip, char name[256])
{
    name[0] = 0;
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    ip_cache_entry_t *entry = ip_cache_find(&s_cache, ip);
    if (entry) {
        uint64_t now = (uint64_t)esp_timer_get_time();
        int64_t utc = cache_utc();
        ip_cache_expire_dns(entry, now, utc);
        if (ip_cache_dns_valid(entry, now, utc)) strcpy(name, entry->info.result.ptr);
    }
    xSemaphoreGive(s_cache_lock);
    return name[0] != 0;
}

void ip_info_cache_snapshot(ip_cache_t *snapshot)
{
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    uint64_t now = (uint64_t)esp_timer_get_time();
    int64_t utc = cache_utc();
    for (size_t i = 0; i < s_cache.count; ++i) {
        ip_cache_expire_dns(&s_cache.entries[i], now, utc);
        ip_cache_expire_provider(&s_cache.entries[i], utc);
    }
    memcpy(snapshot->entries, s_cache.entries, s_cache.count * sizeof(*s_cache.entries));
    for (size_t i = 0; i < snapshot->count; ++i) {
        if (!snapshot->entries[i].info.dns_expires_at) {
            // An unsynchronized boot has no portable expiration timestamp.
            snapshot->entries[i].info.result.ptr[0] = 0;
            snapshot->entries[i].info.dns_checked = false;
        }
    }
    snapshot->sequence = s_cache.sequence;
    xSemaphoreGive(s_cache_lock);
}

void ip_info_cache_restore(const ip_cache_t *saved)
{
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    ip_cache_merge(&s_cache, saved);
    xSemaphoreGive(s_cache_lock);
}

static bool current(const request_t *request)
{
    return atomic_load(&s_generation) == request->id;
}

static void publish(const request_t *request, const ip_info_result_t *result, const char *abuse)
{
    if (!current(request)) return;
    ip_info_detail_t detail = {.info = *result};
    snprintf(detail.abuse, sizeof(detail.abuse), "%s", abuse);
    xQueueOverwrite(s_results, &detail);
}

static void log_response(const char *service, const char *body)
{
    char preview[257];
    size_t i = 0;
    for (; body[i] && i < sizeof(preview) - 1; ++i) {
        unsigned char ch = (unsigned char)body[i];
        preview[i] = ch >= 32 && ch < 127 ? (char)ch : ' ';
    }
    preview[i] = '\0';
    ESP_LOGW(TAG, "%s response preview: %s", service, preview);
}

static char *get_json(const char *service, const char *url, const char *key,
                      char error_text[LOOKUP_ERROR_CAPACITY])
{
    error_text[0] = '\0';
    char *body = calloc(1, RESPONSE_CAPACITY);
    if (body == NULL) {
        snprintf(error_text, LOOKUP_ERROR_CAPACITY, "Out of memory");
        ESP_LOGE(TAG, "%s: response allocation failed", service);
        return NULL;
    }
    ESP_LOGI(TAG, "%s GET %s; UTC epoch=%lld, internal heap=%u, largest block=%u",
             service, url, (long long)time(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = IP_INFO_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .addr_type = HTTP_ADDR_TYPE_INET,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        snprintf(error_text, LOOKUP_ERROR_CAPACITY, "HTTP client initialization failed");
        ESP_LOGE(TAG, "%s: HTTP client initialization failed", service);
        free(body);
        return NULL;
    }
    size_t used = 0;
    bool complete = false;
    const char *stage = "request headers";
    int status = 0;
    int read_result = 0;
    int socket_errno = 0;
    esp_err_t error = esp_http_client_set_header(client, "Accept", "application/json");
    if (error == ESP_OK && key) error = esp_http_client_set_header(client, "Key", key);
    uint64_t deadline = (uint64_t)esp_timer_get_time() + IP_INFO_HTTP_DEADLINE_MS * 1000ULL;
    if (error != ESP_OK) goto done;
    stage = "connect (DNS/TCP/TLS)";
    error = esp_http_client_open(client, 0);
    socket_errno = errno;
    if (error != ESP_OK) goto done;
    stage = "response headers";
    int64_t length = esp_http_client_fetch_headers(client);
    socket_errno = errno;
    if (length < 0) {
        error = ESP_FAIL;
        snprintf(error_text, LOOKUP_ERROR_CAPACITY, "HTTP headers failed (%" PRId64 ")", length);
        goto done;
    }
    status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "%s HTTP %d, content length=%" PRId64 ", chunked=%d",
             service, status, length, esp_http_client_is_chunked_response(client));
    stage = "response body";
    // Read error responses as well, so HTTP 403/429 and provider messages can
    // be diagnosed. Never hand incomplete or oversized JSON to the parser.
    while (used < RESPONSE_CAPACITY - 1 && (uint64_t)esp_timer_get_time() < deadline) {
        read_result = esp_http_client_read(client, body + used, RESPONSE_CAPACITY - 1 - used);
        socket_errno = errno;
        if (read_result > 0) used += (size_t)read_result;
        complete = esp_http_client_is_complete_data_received(client);
        if (complete || read_result <= 0) break;
    }
    if (status != 200) {
        snprintf(error_text, LOOKUP_ERROR_CAPACITY, "HTTP %d (see serial log)", status);
        if (key && (status == 401 || status == 403))
            snprintf(error_text, LOOKUP_ERROR_CAPACITY, "API key rejected; update it in Settings");
        else if (key && status == 429)
            snprintf(error_text, LOOKUP_ERROR_CAPACITY, "Rate limit reached; try again later");
    } else if (!complete || used == 0) {
        snprintf(error_text, LOOKUP_ERROR_CAPACITY, "HTTP body %s (%u bytes, read=%d)",
                 used == RESPONSE_CAPACITY - 1 ? "too large" : "incomplete/empty",
                 (unsigned)used, read_result);
    }

done:
    if (error != ESP_OK || error_text[0]) {
        int tls_code = 0, tls_flags = 0;
        esp_err_t tls_error = esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
        int client_errno = esp_http_client_get_errno(client);
        if (!error_text[0]) {
            snprintf(error_text, LOOKUP_ERROR_CAPACITY, "%s: %s", stage, esp_err_to_name(error));
        }
        ESP_LOGE(TAG, "%s failed at %s: %s; HTTP=%d, err=%s, errno=%d, client_errno=%d, "
                 "TLS=%s, mbedtls=%d, cert_flags=0x%x, bytes=%u, read=%d",
                 service, stage, error_text, status, esp_err_to_name(error), socket_errno,
                 client_errno, esp_err_to_name(tls_error), tls_code, (unsigned)tls_flags,
                 (unsigned)used, read_result);
        if (used && !key) log_response(service, body);
    } else {
        ESP_LOGI(TAG, "%s received complete response: %u bytes", service, (unsigned)used);
    }
    esp_http_client_cleanup(client);
    if (error != ESP_OK || error_text[0]) {
        free(body);
        return NULL;
    }
    body[used] = '\0';
    return body;
}

static void lookup(const request_t *request)
{
    if (!current(request)) return;
    ip_info_result_t result = {.ip = request->ip, .request_id = request->id};
    char key[ABUSEIPDB_KEY_CAPACITY] = "";
    char abuse[384] = "";
    abuseipdb_key_get(key);
    snprintf(abuse, sizeof(abuse), "%s", key[0] ? "AbuseIPDB: Looking up..." :
             "AbuseIPDB (optional)\nAdd an API key via Settings to enable lookup.");
    if (!ip_info_is_public(request->ip)) {
        strcpy(result.location, "Local / reserved network");
        publish(request, &result, "AbuseIPDB: Not queried for local/reserved IPs");
        return;
    }
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    ip_cache_entry_t *entry = ip_cache_acquire(&s_cache, request->ip);
    ip_cache_expire_dns(entry, (uint64_t)esp_timer_get_time(), cache_utc());
    ip_cache_expire_provider(entry, cache_utc());
    bool provider_valid = ip_cache_provider_valid(entry, cache_utc());
    ip_cache_info_t cached = entry->info;
    uint64_t dns_deadline = entry->dns_expires_us;
    if (!ip_cache_dns_valid(entry, (uint64_t)esp_timer_get_time(), cache_utc())) {
        cached.result.ptr[0] = 0;
        cached.dns_checked = false;
    }
    ip_cache_touch(&s_cache, entry);
    xSemaphoreGive(s_cache_lock);
    result = cached.result;
    result.request_id = request->id;
    if (!provider_valid) { result.location[0] = 0; result.network[0] = 0; }
    bool need_dns = !result.ptr[0];
    bool need_provider = !provider_valid || cached.provider_missing;
    if (!result.location[0]) strcpy(result.location, "Looking up...");
    publish(request, &result, abuse);
    if (!need_dns && !need_provider) goto abuse_lookup;
    if (router_get_status().state != ROUTER_READY || !app_clock_is_ready()) {
        if (need_provider) strcpy(result.location, "Unavailable: offline or waiting for clock");
    } else {
        char url[192], error_text[LOOKUP_ERROR_CAPACITY];
        if (need_dns) {
            const uint8_t *b = (const uint8_t *)&request->ip;
            snprintf(url, sizeof(url),
                     "https://dns.google/resolve?name=%u.%u.%u.%u.in-addr.arpa&type=PTR&edns_client_subnet=0.0.0.0/0",
                     b[3], b[2], b[1], b[0]);
            char *json = get_json("Google DNS", url, NULL, error_text);
            bool ok = json && ip_info_parse_ptr(json, &result);
            if (json && !ok) log_response("Google DNS", json);
            free(json);
            if (ok) {
                cached.dns_checked = true;
                dns_deadline = (uint64_t)esp_timer_get_time() + IP_CACHE_DNS_TTL_SECONDS * 1000000ULL;
                cached.dns_expires_at = cache_utc() + IP_CACHE_DNS_TTL_SECONDS;
                snprintf(cached.result.ptr, sizeof(cached.result.ptr), "%s",
                         result.ptr);
            }
            publish(request, &result, abuse);
        }
        if (need_provider && current(request)) {
            char address[16];
            ip_info_format_ip(request->ip, address);
            snprintf(url, sizeof(url),
                     "https://ipwho.is/%s?fields=ip,success,message,country,region,city,connection", address);
            char *json = get_json("ipwho.is", url, NULL, error_text);
            bool ok = json && ip_info_parse_location(json, &result);
            if (json && !ok) { strcpy(error_text, "Invalid provider response"); log_response("ipwho.is", json); }
            free(json);
            if (ok) {
                cached.provider_checked = true;
                cached.provider_expires_at = cache_utc() + IP_CACHE_PROVIDER_TTL_SECONDS;
                cached.provider_missing = strstr(result.network, "Unknown") != NULL ||
                                          strstr(result.location, "Unknown") != NULL ||
                                          strstr(result.location, " / -") != NULL;
                strcpy(cached.result.location, result.location);
                strcpy(cached.result.network, result.network);
            } else snprintf(result.location, sizeof(result.location), "Unavailable: %s", error_text);
        }
    }
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    // The UI can evict a slot while HTTPS is running; find it again by IP.
    entry = ip_cache_acquire(&s_cache, request->ip);
    entry->info.dns_checked = cached.dns_checked;
    entry->info.provider_checked = cached.provider_checked;
    entry->info.provider_missing = cached.provider_missing;
    entry->info.result = cached.result;
    entry->info.dns_expires_at = cached.dns_expires_at;
    entry->info.provider_expires_at = cached.provider_expires_at;
    entry->dns_expires_us = dns_deadline;
    ip_cache_touch(&s_cache, entry);
    xSemaphoreGive(s_cache_lock);
abuse_lookup:
    if (!current(request)) return;
    publish(request, &result, abuse);
    if (!key[0]) return;
    if (router_get_status().state != ROUTER_READY || !app_clock_is_ready()) {
        publish(request, &result, "AbuseIPDB: Offline or waiting for clock");
        return;
    }
    char address[16], url[160], error_text[LOOKUP_ERROR_CAPACITY];
    ip_info_format_ip(request->ip, address);
    snprintf(url, sizeof(url), "https://api.abuseipdb.com/api/v2/check?ipAddress=%s&maxAgeInDays=90", address);
    char *json = get_json("AbuseIPDB", url, key, error_text);
    ip_info_detail_t response = {.info = result};
    if (!json || !ip_info_parse_abuse(json, &response))
        snprintf(response.abuse, sizeof(response.abuse), "AbuseIPDB: %s",
                 json ? "Invalid provider response" : error_text);
    free(json);
    publish(request, &result, response.abuse);
}

static void lookup_task(void *arg)
{
    (void)arg;
    for (;;) {
        drain_names();
        request_t request;
        if (xQueueReceive(s_requests, &request, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (current(&request)) lookup(&request);
        }
    }
}

esp_err_t ip_info_init(void)
{
    s_cache.count = IP_INFO_CACHE_COUNT;
    s_cache.entries = heap_caps_calloc(s_cache.count, sizeof(*s_cache.entries), MALLOC_CAP_SPIRAM);
    s_cache_lock = xSemaphoreCreateMutex();
    s_requests = xQueueCreate(1, sizeof(request_t));
    s_results = xQueueCreate(1, sizeof(ip_info_detail_t));
    s_names = xQueueCreate(IP_INFO_DNS_QUEUE_COUNT, sizeof(observed_name_t));
    if (!s_cache.entries || !s_cache_lock || !s_requests || !s_results || !s_names) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "In-memory IP cache: %u entries, %u bytes PSRAM",
             (unsigned)s_cache.count, (unsigned)(s_cache.count * sizeof(*s_cache.entries)));
    return xTaskCreate(lookup_task, "ip_lookup", 16384, NULL, 3, NULL) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}

uint32_t ip_info_request(uint32_t ip)
{
    request_t request = { .ip = ip, .id = atomic_fetch_add(&s_generation, 1) + 1 };
    xQueueOverwrite(s_requests, &request);
    return request.id;
}

void ip_info_cancel(void)
{
    atomic_fetch_add(&s_generation, 1);
    xQueueReset(s_results);
}

bool ip_info_poll(ip_info_detail_t *result)
{
    return xQueueReceive(s_results, result, 0) == pdTRUE;
}
