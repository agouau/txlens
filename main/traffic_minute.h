#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "app_config.h"

// Independent of the shorter UI history; capacity is set in app_config.h.
typedef struct {
    uint32_t ip;
    uint64_t bytes[60];
} traffic_minute_row_t;

typedef struct {
    time_t minute; // UTC epoch minutes; -1 means unused.
    size_t count;
    uint64_t dropped_bytes;
    traffic_minute_row_t rows[TRAFFIC_MINUTE_MAX_IPS];
} traffic_minute_t;

void traffic_minute_reset(traffic_minute_t *data, time_t minute);
void traffic_minute_record(traffic_minute_t *data, time_t second,
                           uint32_t ip, uint16_t bytes);
