#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "app_config.h"
#include "ip_info.h"

typedef struct {
    ip_info_result_t result;
    bool dns_checked;
    bool provider_checked;
    bool provider_missing;
    int64_t dns_expires_at; // UTC seconds; zero until the clock is synchronized.
    int64_t provider_expires_at; // UTC seconds; provider lookups require a synchronized clock.
} ip_cache_info_t;

typedef struct {
    ip_cache_info_t info;
    uint64_t accessed;
    bool used;
    uint64_t last_seen_us;
    uint64_t dns_expires_us; // Monotonic deadline for names learned during this boot.
} ip_cache_entry_t;

typedef struct {
    ip_cache_entry_t *entries;
    size_t count;
    uint64_t sequence;
} ip_cache_t;

ip_cache_entry_t *ip_cache_find(ip_cache_t *cache, uint32_t ip);
ip_cache_entry_t *ip_cache_acquire(ip_cache_t *cache, uint32_t ip);
void ip_cache_touch(ip_cache_t *cache, ip_cache_entry_t *entry);
bool ip_cache_dns_valid(const ip_cache_entry_t *entry, uint64_t now_us, int64_t now_utc);
void ip_cache_expire_dns(ip_cache_entry_t *entry, uint64_t now_us, int64_t now_utc);
bool ip_cache_provider_valid(const ip_cache_entry_t *entry, int64_t now_utc);
void ip_cache_expire_provider(ip_cache_entry_t *entry, int64_t now_utc);
// Import newest first, filling gaps without evicting or overriding live data.
void ip_cache_merge(ip_cache_t *cache, const ip_cache_t *saved);

// SD worker bridge: copies/merges under the live cache mutex; no SD I/O inside.
void ip_info_cache_snapshot(ip_cache_t *snapshot);
void ip_info_cache_restore(const ip_cache_t *saved);
