// Real lookup/publish code with a deterministic HTTPS boundary.
#include <assert.h>
#include <setjmp.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ip_cache.h"
#include "ip_info_parse.h"
#include "router.h"
#include "wifi_settings.h"

#define LOOKUP_ERROR_CAPACITY 96
#define portMAX_DELAY 1
#define atomic_load(p) (*(p))
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
typedef struct { uint32_t ip, id; } request_t;
static unsigned s_generation = 7;
static int s_results = 1, s_cache_lock = 1, lock_depth;
static ip_cache_entry_t entries[100];
static ip_cache_t s_cache = {.entries = entries, .count = 2};
static ip_info_detail_t displayed;
static unsigned publishes, abuse_requests, metadata_requests;
static bool online = true, clock_ready = true, cancel_in_http;
static const char *configured_key = "test-key";
static const char *http_error;
static const char *abuse_body = "{\"data\":{\"ipAddress\":\"8.8.8.8\","
    "\"abuseConfidenceScore\":42,\"totalReports\":5,\"numDistinctUsers\":3,\"isTor\":false}}";
static void xQueueOverwrite(int queue, const void *result)
{
    assert(queue == 1); displayed = *(const ip_info_detail_t *)result; ++publishes;
}
static void xSemaphoreTake(int lock, int ticks)
{
    assert(lock == 1 && ticks == portMAX_DELAY && lock_depth == 0); ++lock_depth;
}
static void xSemaphoreGive(int lock) { assert(lock == 1 && lock_depth == 1); --lock_depth; }
static int64_t fake_now_us = 1000000, fake_utc = 1800000000;
static int64_t esp_timer_get_time(void) { return fake_now_us; }
static int64_t cache_utc(void) { return clock_ready ? fake_utc : 0; }
static int s_requests=2;
static unsigned receive_count, stop_after, names_drained;
static bool queued;
static request_t queued_request;
static jmp_buf worker_stop;
static void drain_names(void) { ++names_drained; }
static int xQueueReceive(int queue, void *out, int ticks) {
    assert(queue==s_requests && ticks==100);
    if (receive_count++>=stop_after) longjmp(worker_stop,1);
    if (!queued) return 0;
    *(request_t *)out=queued_request; queued=false; return pdTRUE;
}
bool app_clock_is_ready(void) { return clock_ready; }
router_status_t router_get_status(void) { return (router_status_t){.state = online ? ROUTER_READY : ROUTER_CONNECTING}; }
bool abuseipdb_key_get(char key[ABUSEIPDB_KEY_CAPACITY])
{
    strcpy(key, configured_key); return key[0] != 0;
}
static void log_response(const char *service, const char *json) { (void)service; (void)json; }
static char *get_json(const char *service, const char *url, const char *key, char error[LOOKUP_ERROR_CAPACITY])
{
    assert(!lock_depth && online && clock_ready);
    const char *body;
    if (!strcmp(service, "AbuseIPDB")) {
        ++abuse_requests;
        assert(key && !strcmp(key, "test-key"));
        assert(!strcmp(url, "https://api.abuseipdb.com/api/v2/check?ipAddress=8.8.8.8&maxAgeInDays=90"));
        if (cancel_in_http) ++s_generation;
        if (http_error) { strcpy(error, http_error); return NULL; }
        body = abuse_body;
    } else {
        ++metadata_requests;
        assert(!key);
        body = !strcmp(service, "Google DNS") ? "{\"Status\":0,\"Answer\":[{\"type\":12,\"data\":\"dns.google.\"}]}" :
            "{\"ip\":\"8.8.8.8\",\"success\":true,\"country\":\"US\",\"region\":\"California\","
            "\"city\":\"Mountain View\",\"connection\":{\"isp\":\"Google\",\"org\":\"Google\",\"asn\":15169}}";
    }
    size_t size = strlen(body) + 1;
    char *copy = malloc(size);
    assert(copy); memcpy(copy, body, size); return copy;
}

#include "lookup.inc"

int main(void)
{
    request_t request = {.ip = 0x08080808, .id = 7};
    // A visible/visited IP and an idle worker must never trigger online queries.
    ip_info_visit(request.ip,1);
    stop_after=100;
    if (!setjmp(worker_stop)) lookup_task(NULL);
    assert(metadata_requests==0 && abuse_requests==0 && publishes==0 && names_drained>0);
    queued_request=request;queued=true;receive_count=0;stop_after=1;
    if (!setjmp(worker_stop)) lookup_task(NULL);
    assert(abuse_requests==1 && metadata_requests==2);
    ip_cache_info_t cached = ip_cache_find(&s_cache, request.ip)->info;
    lookup(&request); // A complete metadata cache must not skip fresh AbuseIPDB lookup.
    assert(abuse_requests == 2 && metadata_requests == 2);
    assert(strstr(displayed.abuse,"Score: 42/100"));
    configured_key = "";
    lookup(&request);
    assert(abuse_requests == 2 && strstr(displayed.abuse, "API key via Settings"));
    configured_key = "test-key"; clock_ready = false;
    lookup(&request);
    assert(abuse_requests == 2 && strstr(displayed.abuse, "Offline or waiting for clock"));
    clock_ready = true; online = false;
    lookup(&request); assert(abuse_requests == 2);
    online = true;
    http_error = "API key rejected; update it in Settings";
    lookup(&request);
    assert(abuse_requests == 3 && strstr(displayed.abuse, "API key rejected"));
    http_error = "Rate limit reached; try again later";
    lookup(&request); assert(strstr(displayed.abuse, "Rate limit reached"));
    http_error = NULL; abuse_body = "{}";
    lookup(&request); assert(strstr(displayed.abuse, "Invalid provider response"));
    assert(!memcmp(&cached, &ip_cache_find(&s_cache, request.ip)->info, sizeof(cached)));
    cancel_in_http = true;
    lookup(&request);
    assert(strstr(displayed.abuse, "Looking up...")); // Late response cannot replace the dialog.
    unsigned calls = abuse_requests, sent = publishes;
    lookup(&request); // Already canceled request does not spend another API call.
    assert(abuse_requests == calls && publishes == sent);
    request = (request_t){.ip = 0x0100000a, .id = s_generation};
    lookup(&request);
    assert(abuse_requests == calls && strstr(displayed.abuse, "local/reserved"));
    // Both PTR and complete provider metadata expire at one day, only refreshing on click.
    request = (request_t){.ip = 0x08080808, .id = s_generation};
    cancel_in_http = false;
    fake_now_us += 86400000000LL - 1;
    fake_utc += 86399;
    unsigned metadata_before = metadata_requests;
    lookup(&request);
    assert(metadata_requests == metadata_before);
    ++fake_now_us; ++fake_utc;
    // Expiry/traffic/idle processing itself must not trigger a refresh.
    ip_info_visit(request.ip,2);
    receive_count=0;stop_after=100;
    if (!setjmp(worker_stop)) lookup_task(NULL);
    assert(metadata_requests==metadata_before);
    lookup(&request);
    assert(metadata_requests == metadata_before + 2);
    assert(!strcmp(ip_cache_find(&s_cache, request.ip)->info.result.ptr, "dns.google"));

    assert(ip_cache_find(&s_cache,request.ip)->info.provider_expires_at==fake_utc+86400);
    puts("PASS: click-only queries, idle/expiry never queries, one-day provider/PTR cache, fresh AbuseIPDB, offline/clock and cancellation");
    return 0;
}
