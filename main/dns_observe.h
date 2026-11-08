#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef void (*dns_name_callback_t)(uint32_t ip, const char *name);
// Extract a single IN/A question from a standard query or response.
bool dns_observe_question(const uint8_t *data, size_t size, bool response,
                          uint16_t *id, char question[256]);
// IPv4 A answers, including CNAME chains. Returns false for malformed/truncated replies.
bool dns_observe_response(const uint8_t *data, size_t size, dns_name_callback_t save);
