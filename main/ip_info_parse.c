#include "ip_info_parse.h"

#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

void ip_info_format_ip(uint32_t ip, char text[16])
{
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(text, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

bool ip_info_is_public(uint32_t ip)
{
    const uint8_t *b = (const uint8_t *)&ip;
    return !(b[0] == 0 || b[0] == 10 || b[0] == 127 || b[0] >= 224 ||
             (b[0] == 100 && b[1] >= 64 && b[1] <= 127) ||
             (b[0] == 169 && b[1] == 254) ||
             (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
             (b[0] == 192 && b[1] == 168) ||
             (b[0] == 192 && b[1] == 0 && (b[2] == 0 || b[2] == 2)) ||
             (b[0] == 198 && (b[1] == 18 || b[1] == 19)) ||
             (b[0] == 198 && b[1] == 51 && b[2] == 100) ||
             (b[0] == 203 && b[1] == 0 && b[2] == 113));
}

// Bound parser recursion before invoking cJSON on a network response.
static cJSON *parse(const char *json)
{
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const char *p = json; *p; ++p) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (*p == '\\') escaped = true;
            else if (*p == '"') quoted = false;
        } else if (*p == '"') quoted = true;
        else if (*p == '{' || *p == '[') {
            if (++depth > 12) return NULL;
        } else if (*p == '}' || *p == ']') {
            if (--depth < 0) return NULL;
        }
    }
    if (depth != 0 || quoted) return NULL;
    return cJSON_ParseWithOpts(json, NULL, true);
}

// These fonts are ASCII. Replace controls/non-ASCII rather than allowing a
// provider string to insert arbitrary lines or unsupported glyphs into the UI.
static void copy_text(char *out, size_t size, const char *text)
{
    size_t i = 0;
    if (text != NULL) {
        for (; text[i] && i + 1 < size; ++i) {
            unsigned char ch = (unsigned char)text[i];
            out[i] = ch >= 32 && ch < 127 ? (char)ch : '?';
        }
    }
    out[i] = '\0';
}

static const char *string_field(const cJSON *object, const char *key)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(field) ? field->valuestring : NULL;
}

bool ip_info_parse_ptr(const char *json, ip_info_result_t *result)
{
    cJSON *root = parse(json);
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "Status");
    const cJSON *answers = cJSON_GetObjectItemCaseSensitive(root, "Answer");
    bool valid = cJSON_IsNumber(status) && (status->valueint == 0 || status->valueint == 3);
    if (answers != NULL && !cJSON_IsArray(answers)) valid = false;
    if (valid) {
        result->ptr[0] = '\0';
        const cJSON *answer;
        cJSON_ArrayForEach(answer, answers) {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(answer, "type");
            const char *data = string_field(answer, "data");
            if (cJSON_IsNumber(type) && type->valueint == 12 && data != NULL && data[0]) {
                copy_text(result->ptr, sizeof(result->ptr), data);
                size_t length = strlen(result->ptr);
                if (length && result->ptr[length - 1] == '.') result->ptr[length - 1] = '\0';
                break;
            }
        }
    }
    cJSON_Delete(root);
    return valid;
}

bool ip_info_parse_location(const char *json, ip_info_result_t *result)
{
    cJSON *root = parse(json);
    char expected[16];
    ip_info_format_ip(result->ip, expected);
    const char *ip = string_field(root, "ip");
    bool valid = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "success")) &&
                 ip != NULL && strcmp(ip, expected) == 0;
    if (valid) {
        char country[48], region[48], city[48], isp[80], org[80];
        copy_text(country, sizeof(country), string_field(root, "country"));
        copy_text(region, sizeof(region), string_field(root, "region"));
        copy_text(city, sizeof(city), string_field(root, "city"));
        snprintf(result->location, sizeof(result->location), "%.40s / %.40s / %.40s",
                 country[0] ? country : "Unknown", region[0] ? region : "-", city[0] ? city : "-");
        const cJSON *connection = cJSON_GetObjectItemCaseSensitive(root, "connection");
        copy_text(isp, sizeof(isp), string_field(connection, "isp"));
        copy_text(org, sizeof(org), string_field(connection, "org"));
        const cJSON *asn = cJSON_GetObjectItemCaseSensitive(connection, "asn");
        char asn_text[32] = "Unknown";
        if (cJSON_IsNumber(asn) && asn->valuedouble > 0 && asn->valuedouble <= UINT32_MAX) {
            snprintf(asn_text, sizeof(asn_text), "AS%lu", (unsigned long)(uint32_t)asn->valuedouble);
        }
        snprintf(result->network, sizeof(result->network), "ISP: %s\nOrg: %s\nASN: %s",
                 isp[0] ? isp : "Unknown", org[0] ? org : "Unknown", asn_text);
    }
    cJSON_Delete(root);
    return valid;
}

static bool whole_number(const cJSON *field, double maximum)
{
    return cJSON_IsNumber(field) && field->valuedouble >= 0 &&
           field->valuedouble <= maximum && floor(field->valuedouble) == field->valuedouble;
}

bool ip_info_parse_abuse(const char *json, ip_info_detail_t *result)
{
    cJSON *root = parse(json);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const char *ip = string_field(data, "ipAddress");
    char expected[16];
    ip_info_format_ip(result->info.ip, expected);
    const cJSON *score = cJSON_GetObjectItemCaseSensitive(data, "abuseConfidenceScore");
    const cJSON *reports = cJSON_GetObjectItemCaseSensitive(data, "totalReports");
    const cJSON *users = cJSON_GetObjectItemCaseSensitive(data, "numDistinctUsers");
    const cJSON *tor = cJSON_GetObjectItemCaseSensitive(data, "isTor");
    const cJSON *last = cJSON_GetObjectItemCaseSensitive(data, "lastReportedAt");
    const cJSON *usage = cJSON_GetObjectItemCaseSensitive(data, "usageType");
    bool valid = ip && strcmp(ip, expected) == 0 && whole_number(score, 100) &&
                 whole_number(reports, UINT32_MAX) && whole_number(users, UINT32_MAX) &&
                 (!tor || cJSON_IsBool(tor)) &&
                 (!last || cJSON_IsNull(last) || cJSON_IsString(last)) &&
                 (!usage || cJSON_IsNull(usage) || cJSON_IsString(usage));
    if (valid) {
        char timestamp[11], type[96]; // YYYY-MM-DD from the provider timestamp.
        copy_text(timestamp, sizeof(timestamp), string_field(data, "lastReportedAt"));
        copy_text(type, sizeof(type), string_field(data, "usageType"));
        snprintf(result->abuse, sizeof(result->abuse),
                 "AbuseIPDB (90 days)\nScore: %.0f/100\nReports: %.0f\nReporters: %.0f\n"
                 "Last report: %s\nUsage: %s\nTor: %s",
                 score->valuedouble, reports->valuedouble, users->valuedouble,
                 timestamp[0] ? timestamp : "None", type[0] ? type : "Unknown",
                 !tor ? "Unknown" : cJSON_IsTrue(tor) ? "Yes" : "No");
    }
    cJSON_Delete(root);
    return valid;
}
