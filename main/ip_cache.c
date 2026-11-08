#include "ip_cache.h"
#include <string.h>

ip_cache_entry_t *ip_cache_find(ip_cache_t *cache, uint32_t ip)
{
    for (size_t i = 0; i < cache->count; ++i) {
        ip_cache_entry_t *entry = &cache->entries[i];
        if (entry->used && entry->info.result.ip == ip) return entry;
    }
    return NULL;
}

void ip_cache_touch(ip_cache_t *cache, ip_cache_entry_t *entry)
{
    entry->accessed = ++cache->sequence;
}

bool ip_cache_dns_valid(const ip_cache_entry_t *entry, uint64_t now_us, int64_t now_utc)
{
    if (!entry->info.dns_checked) return false;
    if (entry->dns_expires_us) return now_us < entry->dns_expires_us;
    return now_utc > 0 && entry->info.dns_expires_at > now_utc &&
           entry->info.dns_expires_at - now_utc <= IP_CACHE_DNS_TTL_SECONDS;
}

void ip_cache_expire_dns(ip_cache_entry_t *entry, uint64_t now_us, int64_t now_utc)
{
    if (!entry->info.dns_checked) return;
    // Preserve disk timestamps until SNTP supplies a clock with which to check them.
    if (!entry->dns_expires_us && now_utc <= 0 && entry->info.dns_expires_at > 0) return;
    if (!ip_cache_dns_valid(entry, now_us, now_utc)) {
        entry->info.result.ptr[0] = 0;
        entry->info.dns_checked = false;
        entry->info.dns_expires_at = 0;
        entry->dns_expires_us = 0;
    } else if (entry->dns_expires_us && !entry->info.dns_expires_at && now_utc > 0) {
        // Persist the remaining lifetime, rather than renewing it after a reboot.
        entry->info.dns_expires_at = now_utc +
            (int64_t)((entry->dns_expires_us - now_us + 999999) / 1000000);
    }
}

bool ip_cache_provider_valid(const ip_cache_entry_t *entry, int64_t now_utc)
{
    return entry->info.provider_checked && now_utc > 0 &&
           entry->info.provider_expires_at > now_utc &&
           entry->info.provider_expires_at - now_utc <= IP_CACHE_PROVIDER_TTL_SECONDS;
}

void ip_cache_expire_provider(ip_cache_entry_t *entry, int64_t now_utc)
{
    // Keep restored timestamps while waiting for SNTP, but do not use them yet.
    if (now_utc <= 0 || ip_cache_provider_valid(entry, now_utc)) return;
    entry->info.provider_checked = false;
    entry->info.provider_missing = false;
    entry->info.provider_expires_at = 0;
    entry->info.result.location[0] = 0;
    entry->info.result.network[0] = 0;
}

ip_cache_entry_t *ip_cache_acquire(ip_cache_t *cache, uint32_t ip)
{
    ip_cache_entry_t *entry = ip_cache_find(cache, ip);
    if (entry != NULL) return entry;
    if (cache->count == 0) return NULL;
    entry = &cache->entries[0];
    for (size_t i = 0; i < cache->count; ++i) {
        ip_cache_entry_t *candidate = &cache->entries[i];
        if (!candidate->used) { entry = candidate; break; }
        if (candidate->accessed < entry->accessed) entry = candidate;
    }
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->info.result.ip = ip;
    ip_cache_touch(cache, entry);
    return entry;
}

void ip_cache_merge(ip_cache_t *cache, const ip_cache_t *saved)
{
    // Reserve lower LRU ranks for disk data, below every existing live entry.
    for (size_t i = 0; i < cache->count; ++i)
        if (cache->entries[i].used) cache->entries[i].accessed += saved->count;
    cache->sequence += saved->count;
    for (size_t i = 0; i < saved->count; ++i) {
        const ip_cache_entry_t *source = &saved->entries[i];
        if (!source->used) continue;
        ip_cache_entry_t *entry = ip_cache_find(cache, source->info.result.ip);
        if (entry) {
            if (!entry->info.dns_checked && !entry->info.result.ptr[0]) {
                strcpy(entry->info.result.ptr, source->info.result.ptr);
                entry->info.dns_checked = source->info.dns_checked;
                entry->info.dns_expires_at = source->info.dns_expires_at;
            }
            if (!entry->info.provider_checked) {
                strcpy(entry->info.result.location, source->info.result.location);
                strcpy(entry->info.result.network, source->info.result.network);
                entry->info.provider_checked = source->info.provider_checked;
                entry->info.provider_missing = source->info.provider_missing;
                entry->info.provider_expires_at = source->info.provider_expires_at;
            }
            continue;
        }
        for (size_t j = 0; j < cache->count; ++j) {
            entry = &cache->entries[j];
            if (entry->used) continue;
            *entry = (ip_cache_entry_t){.info = source->info, .used = true,
                                        .accessed = saved->count - i};
            entry->info.result.request_id = 0;
            break;
        }
    }
}
