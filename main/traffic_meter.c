#include "traffic_meter.h"
#include "dns_observe.h"
#include "ip_info.h"

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_netif_net_stack.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"

// Keep the current slot alongside a full minute of completed slots.
#define HISTORY_BUCKET_COUNT (TRAFFIC_BUCKET_COUNT + 1)

typedef struct {
    bool used;
    uint32_t ip;
    uint64_t bucket;
    uint64_t last_seen_us;
    uint32_t bytes[HISTORY_BUCKET_COUNT];
} history_t;

// History and attachment are accessed exclusively on the lwIP thread.
static history_t *s_history;
static struct netif *s_ap;
static struct netif *s_sta;
static netif_output_fn s_sta_output;
static struct netif *s_input;
static uint64_t s_untracked_bytes;
static traffic_tx_observer_t s_observer;

// Queries are captured after NAPT chooses the upstream source port. Responses
// must reverse that exact tuple and repeat the client's ID and question.
typedef struct {
    uint32_t local_ip, resolver_ip;
    uint16_t port, id;
    uint64_t expires_us;
    char question[256];
} dns_request_t;
static dns_request_t s_dns_pending[DNS_PENDING_COUNT];
static size_t s_dns_cursor;

static uint32_t checksum_add(uint32_t sum, const uint8_t *data, size_t length)
{
    while (length >= 2) {
        sum += (uint32_t)((data[0] << 8) | data[1]);
        data += 2;
        length -= 2;
    }
    if (length) sum += (uint32_t)data[0] << 8;
    return sum;
}

static bool checksum_valid(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return sum == 0xffff;
}

static bool dns_packet(struct pbuf *packet, bool response, dns_request_t *request,
                        uint8_t **body, size_t *body_size)
{
    uint8_t header[68];
    if (packet->tot_len < 28 || pbuf_copy_partial(packet, header, 20, 0) != 20) return false;
    unsigned ihl = (header[0] & 15) * 4;
    if ((header[0] >> 4) != 4 || ihl < 20 || header[9] != 17 ||
        (header[6] & 0x3f) || header[7] || packet->tot_len < ihl + 8 ||
        pbuf_copy_partial(packet, header, (uint16_t)(ihl + 8), 0) != ihl + 8) return false;
    unsigned source = (header[ihl] << 8) | header[ihl + 1];
    unsigned destination = (header[ihl + 2] << 8) | header[ihl + 3];
    if ((response ? source : destination) != 53) return false;
    unsigned udp = (header[ihl + 4] << 8) | header[ihl + 5];
    unsigned total = (header[2] << 8) | header[3];
    if (udp < 20 || udp > 4104 || total > packet->tot_len || total != ihl + udp) return false;
    // Outbound headers may still be awaiting driver checksum generation.
    if (response && !checksum_valid(checksum_add(0, header, ihl))) return false;
    size_t size = udp - 8;
    uint8_t *dns = malloc(size);
    if (!dns) return false;
    bool valid = pbuf_copy_partial(packet, dns, (uint16_t)size, (uint16_t)(ihl + 8)) == size;
    if (valid && response && (header[ihl + 6] || header[ihl + 7])) {
        uint32_t sum = checksum_add(17 + udp, header + 12, 8);
        sum = checksum_add(sum, header + ihl, 8);
        valid = checksum_valid(checksum_add(sum, dns, size));
    }
    valid = valid && dns_observe_question(dns, size, response, &request->id, request->question);
    if (!valid) { free(dns); return false; }
    memcpy(&request->local_ip, header + (response ? 16 : 12), 4);
    memcpy(&request->resolver_ip, header + (response ? 12 : 16), 4);
    request->port = (uint16_t)(response ? destination : source);
    *body = dns;
    *body_size = size;
    return true;
}

static void remember_dns(dns_request_t *request)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    size_t slot = s_dns_cursor;
    for (size_t i = 0; i < DNS_PENDING_COUNT; ++i) {
        dns_request_t *pending = &s_dns_pending[i];
        if (pending->expires_us <= now ||
            (pending->local_ip == request->local_ip && pending->resolver_ip == request->resolver_ip &&
             pending->port == request->port && pending->id == request->id &&
             !strcmp(pending->question, request->question))) { slot = i; break; }
    }
    request->expires_us = now + DNS_PENDING_TIMEOUT_MS * 1000ULL;
    s_dns_pending[slot] = *request;
    s_dns_cursor = (slot + 1) % DNS_PENDING_COUNT;
}

esp_err_t traffic_meter_init(void)
{
    if (s_history != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_history = heap_caps_calloc(TRAFFIC_MAX_IPS, sizeof(*s_history), MALLOC_CAP_SPIRAM);
    return s_history != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void advance(history_t *row, uint64_t bucket)
{
    uint64_t elapsed = bucket - row->bucket;
    if (elapsed >= HISTORY_BUCKET_COUNT) {
        memset(row->bytes, 0, sizeof(row->bytes));
    } else {
        for (uint64_t i = row->bucket + 1; i <= bucket; ++i) {
            row->bytes[i % HISTORY_BUCKET_COUNT] = 0;
        }
    }
    row->bucket = bucket;
}

static void record(uint32_t ip, uint16_t length)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    uint64_t bucket = now / TRAFFIC_BUCKET_US;
    history_t *available = NULL;
    for (size_t i = 0; i < TRAFFIC_MAX_IPS; ++i) {
        history_t *row = &s_history[i];
        if (row->used && row->ip == ip) {
            advance(row, bucket);
            row->last_seen_us = now;
            row->bytes[bucket % HISTORY_BUCKET_COUNT] += length;
            return;
        }
        if (available == NULL && (!row->used ||
            bucket - row->last_seen_us / TRAFFIC_BUCKET_US > TRAFFIC_BUCKET_COUNT)) {
            available = row;
        }
    }
    if (available == NULL) {
        s_untracked_bytes += length;
        return; // Preserve the full minute for existing destinations.
    }
    memset(available, 0, sizeof(*available));
    available->used = true;
    available->ip = ip;
    available->bucket = bucket;
    available->last_seen_us = now;
    available->bytes[bucket % HISTORY_BUCKET_COUNT] = length;
}

err_t __real_ip4_input(struct pbuf *packet, struct netif *input);
static void observe_dns(struct pbuf *packet)
{
    dns_request_t response = {0};
    uint8_t *dns;
    size_t size;
    if (!dns_packet(packet, true, &response, &dns, &size)) return;
    uint64_t now = (uint64_t)esp_timer_get_time();
    for (size_t i = 0; i < DNS_PENDING_COUNT; ++i) {
        dns_request_t *pending = &s_dns_pending[i];
        if (pending->expires_us > now && pending->local_ip == response.local_ip &&
            pending->resolver_ip == response.resolver_ip && pending->port == response.port &&
            pending->id == response.id && !strcmp(pending->question, response.question)) {
            if (dns_observe_response(dns, size, ip_info_observe_name)) pending->expires_us = 0;
            break;
        }
    }
    free(dns);
}

err_t __wrap_ip4_input(struct pbuf *packet, struct netif *input)
{
    if (input == s_sta) observe_dns(packet);
    struct netif *previous = s_input;
    s_input = input;
    err_t result = __real_ip4_input(packet, input);
    s_input = previous;
    return result;
}

static err_t metered_output(struct netif *netif, struct pbuf *packet,
                            const ip4_addr_t *destination)
{
    // Capture before the driver changes/frees the pbuf or prepends Ethernet.
    // lwIP supplies the remote IPv4 destination here, including after NAT.
    // Local DNS/HTTPS/SNTP requests have no AP input context, even when their
    // destination IP is also being visited by the client. No IP blacklist is used.
    const bool forwarded = s_ap != NULL && s_input == s_ap && destination != NULL;
    const uint32_t ip = destination != NULL ? destination->addr : 0;
    const uint16_t length = packet->tot_len;
    dns_request_t query = {0};
    uint8_t *dns = NULL;
    size_t dns_size;
    bool client_dns = forwarded && dns_packet(packet, false, &query, &dns, &dns_size);
    free(dns);
    err_t result = s_sta_output(netif, packet, destination);
    if (forwarded && result == ERR_OK) {
        if (client_dns) remember_dns(&query);
        if (s_observer != NULL) s_observer(ip, length);
        record(ip, length);
    }
    return result;
}

typedef struct {
    esp_netif_t *ap;
    esp_netif_t *sta;
} attach_context_t;

static esp_err_t attach_on_lwip_thread(void *arg)
{
    const attach_context_t *context = arg;
    struct netif *ap = esp_netif_get_netif_impl(context->ap);
    struct netif *sta = esp_netif_get_netif_impl(context->sta);
    if (ap == NULL || sta == NULL || sta->output == NULL || s_history == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ap = ap;
    s_sta = sta;
    memset(s_dns_pending, 0, sizeof(s_dns_pending));
    if (sta->output != metered_output) {
        s_sta_output = sta->output;
        sta->output = metered_output;
    }
    return ESP_OK;
}

esp_err_t traffic_meter_attach(esp_netif_t *ap, esp_netif_t *sta)
{
    if (ap == NULL || sta == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    attach_context_t context = { .ap = ap, .sta = sta };
    return esp_netif_tcpip_exec(attach_on_lwip_thread, &context);
}

static esp_err_t snapshot_on_lwip_thread(void *arg)
{
    traffic_snapshot_t *snapshot = arg;
    snapshot->now_us = (uint64_t)esp_timer_get_time();
    snapshot->untracked_bytes = s_untracked_bytes;
    snapshot->count = 0;
    uint64_t bucket = snapshot->now_us / TRAFFIC_BUCKET_US;
    for (size_t i = 0; i < TRAFFIC_MAX_IPS; ++i) {
        history_t *row = &s_history[i];
        if (!row->used) {
            continue;
        }
        if (bucket - row->last_seen_us / TRAFFIC_BUCKET_US > TRAFFIC_BUCKET_COUNT) {
            row->used = false;
            continue;
        }
        advance(row, bucket);
        traffic_row_t *out = &snapshot->rows[snapshot->count++];
        out->ip = row->ip;
        out->last_seen_us = row->last_seen_us;
        // Exclude the slot still collecting packets; pre-boot slots stay zero.
        size_t first = (bucket + HISTORY_BUCKET_COUNT - TRAFFIC_BUCKET_COUNT) % HISTORY_BUCKET_COUNT;
        for (size_t j = 0; j < TRAFFIC_BUCKET_COUNT; ++j)
            out->bytes[j] = row->bytes[(first + j) % HISTORY_BUCKET_COUNT];
    }
    return ESP_OK;
}

esp_err_t traffic_meter_snapshot(traffic_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_history == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_tcpip_exec(snapshot_on_lwip_thread, snapshot);
}

static esp_err_t set_observer_on_lwip_thread(void *arg)
{
    s_observer = *(traffic_tx_observer_t *)arg;
    return ESP_OK;
}

esp_err_t traffic_meter_set_observer(traffic_tx_observer_t observer)
{
    return esp_netif_tcpip_exec(set_observer_on_lwip_thread, &observer);
}
