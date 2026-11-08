#include <stdio.h>
#include <string.h>
#include "ip_cache.h"
#include "dns_observe.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
static unsigned found;
static uint32_t found_ip;
static char found_name[256];
static void observe(uint32_t ip, const char *name)
{
    ++found;
    found_ip = ip;
    snprintf(found_name, sizeof(found_name), "%s", name);
}

int main(void)
{
    ip_cache_entry_t entries[2] = {0};
    ip_cache_t cache = {.entries = entries, .count = 2};
    ip_cache_entry_t *a = ip_cache_acquire(&cache, 1);
    strcpy(a->info.result.ptr, "example.com");
    a->info.dns_checked = true;
    a->info.provider_checked = true;
    strcpy(a->info.result.network, "Org: Example");
    ip_cache_acquire(&cache, 2);
    ip_cache_touch(&cache, a);
    ip_cache_acquire(&cache, 3);
    CHECK(ip_cache_find(&cache, 2) == NULL);
    CHECK(strcmp(ip_cache_find(&cache, 1)->info.result.ptr, "example.com") == 0);
    CHECK(ip_cache_find(&cache, 3)->info.result.ptr[0] == 0);
    a = ip_cache_find(&cache, 3);
    a->info.dns_checked = true; // Negative answers are cached too.
    CHECK(!a->info.result.ptr[0] && a->info.dns_checked);
    ip_cache_entry_t empty[2] = {0};
    ip_cache_t reboot = {.entries = empty, .count = 2};
    CHECK(ip_cache_find(&reboot, 1) == NULL);
    CHECK(ip_cache_find(&reboot, 3) == NULL);

    ip_cache_entry_t disk_entries[3] = {0};
    ip_cache_t disk = {.entries = disk_entries, .count = 3};
    disk_entries[0] = (ip_cache_entry_t){.used = true, .info.result.ip = 9};
    strcpy(disk_entries[0].info.result.ptr, "newest.disk");
    disk_entries[1] = (ip_cache_entry_t){.used = true, .info.result.ip = 1};
    strcpy(disk_entries[1].info.result.ptr, "stale.disk");
    disk_entries[1].info.provider_checked = true;
    disk_entries[1].info.provider_expires_at = 1800086400;
    strcpy(disk_entries[1].info.result.network, "Saved provider");
    disk_entries[2] = (ip_cache_entry_t){.used = true, .info.result.ip = 8};
    ip_cache_merge(&reboot, &disk);
    CHECK(ip_cache_find(&reboot, 9) && ip_cache_find(&reboot, 1));
    CHECK(!ip_cache_find(&reboot, 8)); // Capacity: newest disk entries win.
    CHECK(ip_cache_find(&reboot, 9)->accessed > ip_cache_find(&reboot, 1)->accessed);
    ip_cache_acquire(&reboot, 7);
    CHECK(!ip_cache_find(&reboot, 1)); // Restored LRU survives a reboot.
    ip_cache_entry_t live_entries[3] = {0};
    ip_cache_t live = {.entries = live_entries, .count = 3};
    a = ip_cache_acquire(&live, 1);
    strcpy(a->info.result.ptr, "live.name");
    a->info.dns_checked = true;
    ip_cache_acquire(&live, 7);
    ip_cache_merge(&live, &disk);
    CHECK(strcmp(a->info.result.ptr, "live.name") == 0);
    CHECK(strcmp(a->info.result.network, "Saved provider") == 0);
    CHECK(a->info.provider_expires_at == 1800086400);
    a->info.result.ptr[0] = 0; // A live negative answer also wins over disk.
    strcpy(a->info.result.network, "Live provider");
    ip_cache_merge(&live, &disk);
    CHECK(!a->info.result.ptr[0] && a->info.dns_checked);
    CHECK(strcmp(a->info.result.network, "Live provider") == 0);
    CHECK(ip_cache_find(&live, 7) && ip_cache_find(&live, 9) && !ip_cache_find(&live, 8));
    ip_cache_acquire(&live, 6);
    CHECK(!ip_cache_find(&live, 9)); // Disk entries rank below all live entries.

    a = ip_cache_find(&live, 6);
    strcpy(a->info.result.ptr, "fresh.example");
    a->info.dns_checked = true;
    a->dns_expires_us = 86400000000ULL;
    CHECK(ip_cache_dns_valid(a, 86399999999ULL, 0));
    ip_cache_expire_dns(a, 86400000000ULL, 0);
    CHECK(!a->info.dns_checked && !a->info.result.ptr[0]);
    // A name learned before SNTP retains its remaining lifetime after clock sync.
    a->info.dns_checked = true;
    a->dns_expires_us = 86400000000ULL;
    ip_cache_expire_dns(a, 10000000, 1800000000);
    CHECK(a->info.dns_expires_at == 1800086390);
    a->dns_expires_us = 0; // Reboot restores only UTC, never the old uptime.
    CHECK(!ip_cache_dns_valid(a, 0, 0)); // Cannot trust saved ages before SNTP.
    ip_cache_expire_dns(a, 0, 0);
    CHECK(a->info.dns_checked); // Do not destroy the entry while waiting for time.
    CHECK(ip_cache_dns_valid(a, 100, 1800086389));
    ip_cache_expire_dns(a, 100, 1800086390);
    CHECK(!a->info.dns_checked);

    // Provider ages survive reboot; neither using the cache nor merging renews them.
    a->info.provider_checked = true;
    a->info.provider_missing = true;
    a->info.provider_expires_at = 1800086400;
    strcpy(a->info.result.network, "Org: Saved");
    strcpy(a->info.result.location, "Saved location");
    CHECK(!ip_cache_provider_valid(a, 0));
    ip_cache_expire_provider(a, 0);
    CHECK(a->info.provider_checked && a->info.provider_expires_at == 1800086400);
    CHECK(ip_cache_provider_valid(a, 1800086399));
    ip_cache_expire_provider(a, 1800086399);
    CHECK(a->info.provider_checked);
    ip_cache_expire_provider(a, 1800086400);
    CHECK(!a->info.provider_checked && !a->info.provider_missing && !a->info.provider_expires_at);
    CHECK(!a->info.result.location[0] && !a->info.result.network[0]);
    a->info.provider_checked = true;
    a->info.provider_expires_at = 1800172801;
    CHECK(!ip_cache_provider_valid(a, 1800086400)); // Reject future dates over one day away.

    // api.example.com -> edge.example.com -> 1.2.3.4, compressed names.
    const uint8_t reply[] = {
        0x12,0x34,0x81,0x80,0,1,0,2,0,0,0,0,
        3,'a','p','i',7,'e','x','a','m','p','l','e',3,'c','o','m',0,0,1,0,1,
        0xc0,12,0,5,0,1,0,0,0,60,0,7,4,'e','d','g','e',0xc0,16,
        0xc0,45,0,1,0,1,0,0,0,60,0,4,1,2,3,4
    };
    dns_observe_response(reply, sizeof(reply), observe);
    CHECK(found == 1 && strcmp(found_name, "api.example.com") == 0);
    CHECK(memcmp(&found_ip, "\1\2\3\4", 4) == 0);
    for (size_t i = 0; i < sizeof(reply); ++i) {
        found = 0;
        dns_observe_response(reply, i, observe);
        CHECK(found == 0);
    }
    uint8_t bad[sizeof(reply)];
    memcpy(bad, reply, sizeof(reply));
    bad[2] |= 2; // Truncated flag.
    dns_observe_response(bad, sizeof(bad), observe);
    CHECK(found == 0);
    memcpy(bad, reply, sizeof(reply));
    bad[12] = 0xc0; bad[13] = 12; // Compression loop.
    dns_observe_response(bad, sizeof(bad), observe);
    CHECK(found == 0);
    memcpy(bad, reply, sizeof(reply));
    bad[sizeof(bad) - 6] = 0; bad[sizeof(bad) - 5] = 3; // Invalid A length.
    dns_observe_response(bad, sizeof(bad), observe);
    CHECK(found == 0);
    puts("PASS: LRU/merge, DNS/provider expiry across reboot, CNAME+A, truncation, loops, bad lengths");
    return 0;
}
