#pragma once

#include <stddef.h>
#include <stdint.h>
#include "app_config.h"
#include "esp_err.h"
#include "esp_netif.h"

// One slot per whole second of the monotonic clock; retain a completed minute.
#define TRAFFIC_BUCKET_US 1000000ULL
#define TRAFFIC_BUCKET_COUNT 60

typedef struct {
    uint32_t ip; // Network byte order.
    uint64_t last_seen_us;
    uint32_t bytes[TRAFFIC_BUCKET_COUNT]; // Oldest first, completed buckets only.
} traffic_row_t;

typedef struct {
    uint64_t now_us;
    uint64_t untracked_bytes; // New IPs rejected while every history slot is occupied.
    size_t count;
    traffic_row_t rows[TRAFFIC_MAX_IPS];
} traffic_snapshot_t;

// Initialize once before starting the router. History is allocated in PSRAM.
esp_err_t traffic_meter_init(void);
esp_err_t traffic_meter_attach(esp_netif_t *ap, esp_netif_t *sta);
// Caller supplies storage. Copies on the lwIP thread, without a spinlock.
esp_err_t traffic_meter_snapshot(traffic_snapshot_t *snapshot);
// Observer runs on the lwIP thread for each successfully forwarded TX packet.
typedef void (*traffic_tx_observer_t)(uint32_t ip, uint16_t bytes);
esp_err_t traffic_meter_set_observer(traffic_tx_observer_t observer);
