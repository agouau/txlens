#include "traffic_minute.h"
#include <string.h>

void traffic_minute_reset(traffic_minute_t *data, time_t minute)
{
    data->minute = minute;
    data->count = 0;
    data->dropped_bytes = 0;
}

void traffic_minute_record(traffic_minute_t *data, time_t second,
                           uint32_t ip, uint16_t bytes)
{
    if (second < 0) return;
    if (data->minute != second / 60) traffic_minute_reset(data, second / 60);
    size_t i = 0;
    while (i < data->count && data->rows[i].ip != ip) ++i;
    if (i == data->count) {
        if (i == TRAFFIC_MINUTE_MAX_IPS) {
            data->dropped_bytes += bytes;
            return;
        }
        memset(&data->rows[i], 0, sizeof(data->rows[i]));
        data->rows[i].ip = ip;
        ++data->count;
    }
    data->rows[i].bytes[second % 60] += bytes;
}
