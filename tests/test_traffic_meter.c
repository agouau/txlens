// Exercise the actual packet hook and history with a deterministic clock.
#include <stdio.h>
#include "traffic_meter.c"
#include "dns_observe.c"

static unsigned observed_dns;
static uint32_t observed_ip;
static char observed_name[256];
void ip_info_observe_name(uint32_t ip, const char *name)
{
    ++observed_dns;
    observed_ip = ip;
    snprintf(observed_name, sizeof(observed_name), "%s", name);
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

static struct netif ap, sta;
static int route_mode = 1;
static err_t output_result;
static unsigned output_calls;
static uint64_t observed_tx_bytes;
static uint32_t observed_tx_ip;
static ip4_addr_t remote = {0x08080808};
static traffic_snapshot_t snapshot;

static void observe_tx(uint32_t ip, uint16_t bytes)
{
    observed_tx_ip = ip;
    observed_tx_bytes += bytes;
}

static err_t driver_output(struct netif *netif, struct pbuf *packet, const ip4_addr_t *destination)
{
    ++output_calls;
    packet->tot_len += 14; // Ethernet headers must be excluded.
    return output_result;
}

err_t __real_ip4_input(struct pbuf *packet, struct netif *input)
{
    if (route_mode == 0) {
        return ERR_OK; // Local delivery/drop before forwarding.
    }
    if (route_mode == 2) {
        struct pbuf fragments[] = {{1020}, {1020}, {520}};
        for (unsigned i = 0; i < 3; ++i) {
            sta.output(&sta, &fragments[i], &remote);
        }
        return ERR_OK;
    }
    if (route_mode == 3 && input == &ap) {
        // Simulate NAPT rewriting both the client address and UDP source port.
        uint8_t *raw = (uint8_t *)packet->payload;
        memcpy(raw + 12, "\xc0\xa8\x01\x14", 4);
        raw[20] = 0xaf; raw[21] = 0xc8; // upstream port 45000
    }
    return sta.output(&sta, packet, &remote);
}

static void upload(uint32_t ip, uint16_t length, uint64_t time)
{
    fake_time = (int64_t)time;
    remote.addr = ip;
    struct pbuf packet = {length};
    __wrap_ip4_input(&packet, &ap);
}

static uint64_t row_total(const traffic_row_t *row)
{
    uint64_t total = 0;
    for (size_t i = 0; i < TRAFFIC_BUCKET_COUNT; ++i) {
        total += row->bytes[i];
    }
    return total;
}

static void fix_ip_checksum(uint8_t *raw)
{
    raw[10] = raw[11] = 0;
    unsigned sum = 0;
    for (unsigned i = 0; i < 20; i += 2) sum += (raw[i] << 8) | raw[i+1];
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    sum = (~sum) & 65535;
    raw[10] = (uint8_t)(sum >> 8); raw[11] = (uint8_t)sum;
}

static void fix_udp_checksum(uint8_t *raw, size_t size)
{
    raw[26] = raw[27] = 0;
    unsigned sum = 17 + (unsigned)size - 20;
    for (unsigned i = 12; i < 20; i += 2) sum += (raw[i] << 8) | raw[i + 1];
    for (size_t i = 20; i < size; i += 2)
        sum += (raw[i] << 8) | (i + 1 < size ? raw[i + 1] : 0);
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    sum = (~sum) & 65535;
    if (!sum) sum = 65535;
    raw[26] = (uint8_t)(sum >> 8); raw[27] = (uint8_t)sum;
}

int main(void)
{
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    CHECK(traffic_meter_init() == ESP_OK);
    CHECK(traffic_meter_init() == ESP_ERR_INVALID_STATE);
    CHECK(traffic_meter_attach(NULL, &sta) == ESP_ERR_INVALID_ARG);
    CHECK(traffic_meter_attach(&ap, &sta) == ESP_ERR_INVALID_STATE);
    sta.output = driver_output;
    CHECK(traffic_meter_attach(&ap, &sta) == ESP_OK);
    CHECK(traffic_meter_attach(&ap, &sta) == ESP_OK); // No recursive hook.
    CHECK(traffic_meter_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    CHECK(traffic_meter_set_observer(observe_tx) == ESP_OK);

    upload(1, 100, 999999);
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(snapshot.count == 1 && row_total(&snapshot.rows[0]) == 0);
    CHECK(output_calls == 1); // The current slot is not published early.
    CHECK(observed_tx_bytes == 100 && observed_tx_ip == 1);

    upload(1, 200, 1000000); // Exact whole-second boundary: belongs to the new slot.
    upload(1, 50, 1999999);
    upload(2, 400, 1999999);
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(snapshot.count == 2 && snapshot.rows[0].bytes[59] == 100);
    CHECK(row_total(&snapshot.rows[1]) == 0);

    struct pbuf packet = {500};
    sta.output(&sta, &packet, &remote); // Device-originated.
    packet.tot_len = 500;
    __wrap_ip4_input(&packet, &sta); // Download/upstream input.
    output_result = -1;
    packet.tot_len = 500;
    CHECK(__wrap_ip4_input(&packet, &ap) == -1);
    output_result = ERR_OK;
    route_mode = 0;
    __wrap_ip4_input(&packet, &ap); // Local/drop.
    route_mode = 2;
    CHECK(observed_tx_bytes == 750); // Local, RX and failed TX excluded.
    remote.addr = 1;
    __wrap_ip4_input(&packet, &ap); // Actual output fragments: 2560 bytes.
    route_mode = 1;
    CHECK(s_input == NULL);
    CHECK(observed_tx_bytes == 3310 && observed_tx_ip == 1);
    fake_time = 2000000;
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(snapshot.rows[0].bytes[58] == 100 && snapshot.rows[0].bytes[59] == 2810);
    CHECK(row_total(&snapshot.rows[1]) == 400);
    fake_time = 4000000;
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(snapshot.rows[0].bytes[56] == 100 && snapshot.rows[0].bytes[57] == 2810);
    CHECK(snapshot.rows[0].bytes[58] == 0 && snapshot.rows[0].bytes[59] == 0);

    fake_time = 60000000; // A full minute of completed slots includes slot 0.
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(snapshot.rows[0].bytes[0] == 100 && snapshot.rows[0].bytes[1] == 2810);
    fake_time = 61000000; // Slot 0 expires; slot 1 remains.
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(snapshot.count == 2 && snapshot.rows[0].bytes[0] == 2810);
    fake_time = 62000000;
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK && snapshot.count == 0);
    upload(1, 77, 200000000);
    fake_time = 201000000;
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(snapshot.count == 1 && row_total(&snapshot.rows[0]) == 77);

    fake_time = 300000000;
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    for (uint32_t i = 1; i <= TRAFFIC_MAX_IPS; ++i) upload(i, 10, 300000000);
    upload(999, 123, 300000000);
    CHECK(observed_tx_ip == 999); // Logging is independent of UI capacity.
    upload(1, 25, 301000000); // Existing IPs still update at capacity.
    fake_time = 302000000;
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(snapshot.count == TRAFFIC_MAX_IPS && snapshot.untracked_bytes == 123);
    CHECK(row_total(&snapshot.rows[0]) == 35);
    upload(999, 15, 361500000); // Reclaim expired slot, retain last completed slot of IP 1.
    fake_time = 361999999;
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(snapshot.count == 2 && snapshot.untracked_bytes == 123);
    CHECK(snapshot.rows[0].ip == 1 && row_total(&snapshot.rows[0]) == 25);
    fake_time = 362000000;
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(snapshot.count == 1 && snapshot.rows[0].ip == 999 && row_total(&snapshot.rows[0]) == 15);

    fake_time = 500000000;
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    for (unsigned i = 0; i < 70000; ++i)
        upload(42, 65500, 500000000ULL + (i / 1400) * TRAFFIC_BUCKET_US);
    fake_time = 550000000;
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(snapshot.count == 1 && row_total(&snapshot.rows[0]) == 70000ULL * 65500ULL);
    CHECK(traffic_meter_attach(&ap, &sta) == ESP_OK);
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(row_total(&snapshot.rows[0]) == 70000ULL * 65500ULL);
    fake_time = 1000000000000LL;
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK && snapshot.count == 0);
    // Filtering is based on AP input, not a DNS-provider blacklist: a client's
    // upload to Google DNS counts, but the device's own query to that SAME IP does not.
    upload(0x08080808, 50, 1000000000000ULL);
    struct pbuf own_dns = {90};
    sta.output(&sta, &own_dns, &remote);
    fake_time += TRAFFIC_BUCKET_US;
    CHECK(traffic_meter_snapshot(&snapshot) == ESP_OK);
    CHECK(snapshot.count == 1 && snapshot.rows[0].ip == 0x08080808);
    CHECK(row_total(&snapshot.rows[0]) == 50);
    // Exercise the real incoming UDP/IP hook and DNS parser together.
    const uint8_t dns[] = {
        0,1,0x81,0x80,0,1,0,1,0,0,0,0,
        1,'a',3,'c','o','m',0,0,1,0,1,
        0xc0,12,0,1,0,1,0,0,0,60,0,4,1,2,3,4
    };
    uint8_t raw[28 + sizeof(dns)] = {0};
    raw[0] = 0x45; raw[3] = sizeof(raw); raw[9] = 17;
    memcpy(raw + 12, "\x08\x08\x08\x08\xc0\xa8\x01\x14", 8);
    raw[21] = 53; raw[22] = 0xaf; raw[23] = 0xc8;
    raw[25] = 8 + sizeof(dns);
    memcpy(raw + 28, dns, sizeof(dns));
    fix_ip_checksum(raw);
    struct pbuf response = {sizeof(raw), raw};
    route_mode = 0;
    __wrap_ip4_input(&response, &sta);
    CHECK(observed_dns == 0); // Unsolicited answer is ignored.

    uint8_t query[28 + 23] = {0}; // DNS header + a.com IN/A question.
    query[0] = 0x45; query[3] = sizeof(query); query[9] = 17;
    memcpy(query + 12, "\xc0\xa8\x32\x02\x08\x08\x08\x08", 8);
    query[20] = 0xcf; query[21] = 8; query[23] = 53;
    query[25] = 31;
    memcpy(query + 28, dns, 23);
    query[30] = 1; query[31] = 0; query[35] = 0; // query, no answers
    struct pbuf request = {sizeof(query), query};
    // Device-originated queries cannot authorize observations.
    sta.output(&sta, &request, &remote);
    __wrap_ip4_input(&response, &sta);
    CHECK(observed_dns == 0);
    route_mode = 3; request.tot_len = sizeof(query);
    output_result = 1;
    __wrap_ip4_input(&request, &ap);
    output_result = 0; route_mode = 0;
    __wrap_ip4_input(&response, &sta);
    CHECK(observed_dns == 0); // Failed forwarding does not create a pending request.
    request.tot_len = sizeof(query); route_mode = 3;
    __wrap_ip4_input(&request, &ap);
    route_mode = 0;
    const unsigned wrong_fields[] = {12, 16, 22, 29, 41}; // resolver, destination, port, ID, name
    for (unsigned i = 0; i < sizeof(wrong_fields)/sizeof(*wrong_fields); ++i) {
        unsigned pos = wrong_fields[i]; raw[pos] ^= 1; fix_ip_checksum(raw);
        __wrap_ip4_input(&response, &sta);
        CHECK(observed_dns == 0);
        raw[pos] ^= 1; fix_ip_checksum(raw);
    }
    raw[10] ^= 1;
    __wrap_ip4_input(&response, &sta); CHECK(observed_dns == 0);
    raw[10] ^= 1; raw[26] = 1; // Incorrect nonzero UDP checksum.
    __wrap_ip4_input(&response, &sta); CHECK(observed_dns == 0);
    raw[26] = 0;
    raw[6] = 0x20; fix_ip_checksum(raw);
    __wrap_ip4_input(&response, &sta); CHECK(observed_dns == 0);
    raw[6] = 0; fix_ip_checksum(raw);
    raw[sizeof(raw) - 5] = 3; // Malformed A record must not consume a matching pending query.
    __wrap_ip4_input(&response, &sta); CHECK(observed_dns == 0);
    raw[sizeof(raw) - 5] = 4;
    fix_udp_checksum(raw, sizeof(raw));
    __wrap_ip4_input(&response, &sta);
    CHECK(observed_dns == 1 && strcmp(observed_name, "a.com") == 0);
    CHECK(memcmp(&observed_ip, "\1\2\3\4", 4) == 0);
    __wrap_ip4_input(&response, &sta); CHECK(observed_dns == 1); // replay
    request.tot_len = sizeof(query); route_mode = 3;
    __wrap_ip4_input(&request, &ap); route_mode = 0;
    fake_time += 10000000;
    __wrap_ip4_input(&response, &sta); CHECK(observed_dns == 1); // expired
    __wrap_ip4_input(&response, &ap); CHECK(observed_dns == 1);
    puts("PASS: traffic history, exclusions, fragments, capacity, reconnect, >4GiB, incoming DNS observation");
    return 0;
}
