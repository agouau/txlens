#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ip_info.h"

void ip_info_format_ip(uint32_t ip, char text[16]);
bool ip_info_is_public(uint32_t ip);
bool ip_info_parse_ptr(const char *json, ip_info_result_t *result);
bool ip_info_parse_location(const char *json, ip_info_result_t *result);
bool ip_info_parse_abuse(const char *json, ip_info_detail_t *result);
