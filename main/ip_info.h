#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t ip;
    uint32_t request_id;
    char ptr[256]; // Resolved domain only; empty while pending, missing or unavailable.
    char location[128];
    char network[224];
} ip_info_result_t;

// One transient detail response; this is never part of the IP cache or SD data.
typedef struct {
    ip_info_result_t info;
    char abuse[384];
} ip_info_detail_t;

// Call after router_start() has initialized esp-netif.
esp_err_t ip_info_init(void);
uint32_t ip_info_request(uint32_t ip);
void ip_info_cancel(void);
bool ip_info_poll(ip_info_detail_t *result);

// UI task: track cache usage only; visiting or reading names never starts an online lookup.
// last_seen_us is the traffic timestamp, so repainting does not alter LRU order.
void ip_info_visit(uint32_t ip, uint64_t last_seen_us);
bool ip_info_name(uint32_t ip, char name[256]);
// lwIP thread: enqueue only; never do Flash writes or network requests here.
void ip_info_observe_name(uint32_t ip, const char *name);
