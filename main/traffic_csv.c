#include "traffic_csv.h"
#include <inttypes.h>

bool traffic_csv_write(FILE *file, const traffic_minute_t *data,
                       bool (*lookup_name)(uint32_t ip, char name[256]))
{
    if (fputs("ip,domain", file) < 0) return false;
    for (unsigned s = 0; s < 60; ++s)
        if (fprintf(file, ",%u", s) < 0) return false;
    if (fputs("\r\n", file) < 0) return false;
    for (size_t i = 0; i < data->count; ++i) {
        const traffic_minute_row_t *row = &data->rows[i];
        const uint8_t *ip = (const uint8_t *)&row->ip;
        char name[256] = {0};
        if (!lookup_name(row->ip, name)) name[0] = '\0';
        if (fprintf(file, "%u.%u.%u.%u,\"", ip[0], ip[1], ip[2], ip[3]) < 0)
            return false;
        for (const char *p = name; *p; ++p) {
            if (*p == '"' && fputc('"', file) == EOF) return false;
            if (fputc(*p, file) == EOF) return false;
        }
        if (fputc('"', file) == EOF) return false;
        for (unsigned s = 0; s < 60; ++s) {
            if (fputc(',', file) == EOF) return false;
            if (row->bytes[s] && fprintf(file, "%" PRIu64, row->bytes[s]) < 0)
                return false;
        }
        if (fputs("\r\n", file) < 0) return false;
    }
    return !ferror(file);
}
