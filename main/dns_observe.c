#include "dns_observe.h"
#include <stdbool.h>
#include <string.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static bool name(const uint8_t *data, size_t size, size_t *offset, char out[256])
{
    size_t pos = *offset, used = 0;
    bool jumped = false;
    for (unsigned steps = 0; steps < 128 && pos < size; ++steps) {
        uint8_t length = data[pos++];
        if ((length & 0xc0) == 0xc0) {
            if (pos >= size) return false;
            size_t target = ((length & 0x3f) << 8) | data[pos++];
            if (target >= pos - 2) return false;
            if (!jumped) *offset = pos;
            jumped = true;
            pos = target;
            continue;
        }
        if (length & 0xc0) return false;
        if (!length) {
            if (!jumped) *offset = pos;
            out[used] = 0;
            return true;
        }
        if (pos + length > size || used + length + (used != 0) > 253) return false;
        if (used) out[used++] = '.';
        for (unsigned i = 0; i < length; ++i) {
            uint8_t ch = data[pos++];
            if (ch <= 32 || ch >= 127 || ch == '.') return false;
            out[used++] = ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : (char)ch;
        }
    }
    return false;
}

bool dns_observe_question(const uint8_t *data, size_t size, bool response,
                          uint16_t *id, char question[256])
{
    if (size < 12 || (u16(data + 2) & 0xfa0f) != (response ? 0x8000 : 0) ||
        u16(data + 4) != 1) return false;
    size_t pos = 12;
    if (!name(data, size, &pos, question) || !question[0] || pos + 4 > size ||
        u16(data + pos) != 1 || u16(data + pos + 2) != 1) return false;
    *id = u16(data);
    return true;
}

bool dns_observe_response(const uint8_t *data, size_t size, dns_name_callback_t save)
{
    if (size < 12 || !save || (u16(data + 2) & 0xfa0f) != 0x8000 ||
        u16(data + 4) != 1 || u16(data + 6) > 64) return false;
    char query[256], canonical[256], owner[256], target[256];
    size_t start = 12;
    if (!name(data, size, &start, query) || !query[0] || start + 4 > size ||
        u16(data + start) != 1 || u16(data + start + 2) != 1) return false;
    start += 4;
    unsigned answers = u16(data + 6);
    // Validate the complete answer section before emitting any mapping.
    size_t pos = start;
    for (unsigned i = 0; i < answers; ++i) {
        if (!name(data, size, &pos, owner) || pos + 10 > size) return false;
        uint16_t type = u16(data + pos), length = u16(data + pos + 8);
        pos += 10;
        if (pos + length > size || (type == 1 && length != 4)) return false;
        if (type == 5) {
            size_t end = pos;
            if (!name(data, size, &end, target) || end != pos + length) return false;
        }
        pos += length;
    }
    strcpy(canonical, query);
    for (unsigned depth = 0; depth < 16; ++depth) {
        bool changed = false;
        pos = start;
        for (unsigned i = 0; i < answers; ++i) {
            if (!name(data, size, &pos, owner)) return false;
            uint16_t type = u16(data + pos), cls = u16(data + pos + 2);
            uint16_t length = u16(data + pos + 8);
            pos += 10;
            if (cls == 1 && strcmp(owner, canonical) == 0 && type == 5) {
                size_t end = pos;
                if (!name(data, size, &end, target)) return false;
                strcpy(canonical, target);
                changed = true;
                break;
            }
            pos += length;
        }
        if (!changed) break;
        if (depth == 15) return false;
    }
    pos = start;
    for (unsigned i = 0; i < answers; ++i) {
        if (!name(data, size, &pos, owner)) return false;
        uint16_t type = u16(data + pos), cls = u16(data + pos + 2);
        uint16_t length = u16(data + pos + 8);
        pos += 10;
        if (type == 1 && cls == 1 && strcmp(owner, canonical) == 0) {
            uint32_t ip;
            memcpy(&ip, data + pos, sizeof(ip));
            save(ip, query);
        }
        pos += length;
    }
    return true;
}
