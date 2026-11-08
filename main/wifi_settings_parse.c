#include "wifi_settings.h"

#include <string.h>
#include "cJSON.h"

static bool valid_text(const char *text, size_t capacity, size_t minimum)
{
    size_t length = 0;
    while (length < capacity && text[length]) {
        if ((unsigned char)text[length] < 32 || (unsigned char)text[length] == 127) return false;
        ++length;
    }
    return length >= minimum && length < capacity;
}

bool wifi_settings_valid(const wifi_settings_t *s)
{
    return valid_text(s->sta_ssid, sizeof(s->sta_ssid), 0) &&
           valid_text(s->sta_password, sizeof(s->sta_password), 0) &&
           (!s->sta_password[0] || strlen(s->sta_password) >= 8) &&
           valid_text(s->ap_ssid, sizeof(s->ap_ssid), 1) &&
           valid_text(s->ap_password, sizeof(s->ap_password), 8);
}

static bool field(cJSON *root, const char *name, char *out, size_t size, bool required)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) return !required;
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= size) return false;
    strcpy(out, item->valuestring);
    return true;
}

static cJSON *parse_object(const char *json)
{
    // Settings is a flat object. Bound recursion before cJSON reads network input,
    // and reject escaped NULs that would otherwise silently truncate C strings.
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (const char *p = json; *p; ++p) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (*p == '\\') {
                if (!strncmp(p, "\\u0000", 6)) return NULL;
                escaped = true;
            } else if (*p == '"') quoted = false;
        } else if (*p == '"') quoted = true;
        else if (*p == '{' || *p == '[') {
            if (++depth > 1) return NULL;
        } else if (*p == '}' || *p == ']') {
            if (!depth) return NULL;
            --depth;
        }
    }
    if (depth || quoted) return NULL;
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }
    return root;
}

bool wifi_settings_parse(const char *json, wifi_settings_t *settings)
{
    cJSON *root = parse_object(json);
    if (!root) return false;
    wifi_settings_t next = *settings;
    bool ok = cJSON_IsObject(root) &&
        field(root, "sta_ssid", next.sta_ssid, sizeof(next.sta_ssid), true) &&
        field(root, "sta_password", next.sta_password, sizeof(next.sta_password), false) &&
        field(root, "ap_ssid", next.ap_ssid, sizeof(next.ap_ssid), true) &&
        field(root, "ap_password", next.ap_password, sizeof(next.ap_password), false) &&
        wifi_settings_valid(&next);
    cJSON_Delete(root);
    if (ok) *settings = next;
    return ok;
}

bool abuseipdb_key_valid(const char *key)
{
    size_t n = 0;
    for (; n < ABUSEIPDB_KEY_CAPACITY && key[n]; ++n)
        if ((unsigned char)key[n] <= 32 || (unsigned char)key[n] >= 127) return false;
    return n < ABUSEIPDB_KEY_CAPACITY; // Empty disables the optional lookup.
}

bool abuseipdb_key_parse(const char *json, char key[ABUSEIPDB_KEY_CAPACITY])
{
    cJSON *root = parse_object(json);
    char next[ABUSEIPDB_KEY_CAPACITY] = {0};
    bool ok = root && field(root, "key", next, sizeof(next), true) && abuseipdb_key_valid(next);
    cJSON_Delete(root);
    if (ok) strcpy(key, next);
    return ok;
}
