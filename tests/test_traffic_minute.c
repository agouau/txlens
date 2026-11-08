#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "traffic_minute.h"
#include "traffic_csv.h"

static bool name(uint32_t ip, char output[256])
{
    if (ip != 0x04030201) return false;
    strcpy(output, "a,\"b.example");
    return true;
}

int main(void)
{
    traffic_minute_t *data = calloc(1, sizeof(*data));
    assert(data);
    traffic_minute_reset(data, -1);
    traffic_minute_record(data, -1, 1, 10);
    assert(data->count == 0);
    traffic_minute_record(data, 120, 0x04030201, 10);
    traffic_minute_record(data, 120, 0x04030201, 20);
    traffic_minute_record(data, 179, 0x04030201, 40);
    traffic_minute_record(data, 121, 0x08080808, 50);
    assert(data->minute == 2 && data->count == 2);
    assert(data->rows[0].bytes[0] == 30 && data->rows[0].bytes[59] == 40);
    assert(data->rows[0].bytes[1] == 0 && data->rows[1].bytes[1] == 50);
    data->rows[0].bytes[0] = UINT64_C(4294967296);
    traffic_minute_record(data, 120, 0x04030201, 1);
    assert(data->rows[0].bytes[0] == UINT64_C(4294967297));

    FILE *file = tmpfile();
    assert(file && traffic_csv_write(file, data, name));
    rewind(file);
    char line[4096];
    assert(fgets(line, sizeof(line), file));
    char header[256] = "ip,domain";
    size_t used = strlen(header);
    for (unsigned i = 0; i < 60; ++i)
        used += (size_t)sprintf(header + used, ",%u", i);
    strcat(header, "\r\n");
    assert(strcmp(line, header) == 0);
    assert(fgets(line, sizeof(line), file));
    const char *prefix = "1.2.3.4,\"a,\"\"b.example\",4294967297,";
    assert(strncmp(line, prefix, strlen(prefix)) == 0);
    assert(strstr(line, ",40\r\n"));
    assert(fgets(line, sizeof(line), file));
    prefix = "8.8.8.8,\"\",,50,";
    assert(strncmp(line, prefix, strlen(prefix)) == 0);
    assert(strstr(line, ",0") == NULL);
    size_t commas = 0;
    for (const char *p = line; *p; ++p) if (*p == ',') ++commas;
    assert(commas == 61); // Empty trailing seconds still occupy all 60 columns.
    assert(!fgets(line, sizeof(line), file));
    fclose(file);

    traffic_minute_record(data, 180, 1, 3);
    assert(data->minute == 3 && data->count == 1);
    assert(data->rows[0].bytes[0] == 3 && data->rows[0].bytes[59] == 0);
    for (uint32_t i = 2; i <= TRAFFIC_MINUTE_MAX_IPS; ++i)
        traffic_minute_record(data, 180, i, 1);
    traffic_minute_record(data, 180, TRAFFIC_MINUTE_MAX_IPS + 1, 42);
    assert(data->count == TRAFFIC_MINUTE_MAX_IPS && data->dropped_bytes == 42);
    traffic_minute_record(data, 181, 1, 5);
    assert(data->rows[0].bytes[1] == 5);
    traffic_minute_record(data, 3600, 1, 7);
    assert(data->minute == 60 && data->count == 1 && data->dropped_bytes == 0);
    free(data);
    puts("traffic minute and CSV tests passed");
    return 0;
}
