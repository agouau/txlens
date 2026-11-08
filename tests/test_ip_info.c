#include <stdio.h>
#include <string.h>
#include "ip_info_parse.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

static uint32_t address(unsigned a, unsigned b, unsigned c, unsigned d)
{
    uint8_t bytes[4] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d};
    uint32_t ip;
    memcpy(&ip, bytes, sizeof(ip));
    return ip;
}

int main(void)
{
    ip_info_result_t result = { .ip = address(8, 8, 8, 8) };
    char text[16];
    ip_info_format_ip(result.ip, text);
    CHECK(strcmp(text, "8.8.8.8") == 0);
    CHECK(ip_info_is_public(result.ip));
    CHECK(ip_info_is_public(address(172, 32, 0, 1)));
    CHECK(!ip_info_is_public(address(10, 1, 2, 3)));
    CHECK(!ip_info_is_public(address(127, 0, 0, 1)));
    CHECK(!ip_info_is_public(address(100, 64, 0, 1)));
    CHECK(!ip_info_is_public(address(169, 254, 1, 2)));
    CHECK(!ip_info_is_public(address(172, 31, 0, 1)));
    CHECK(!ip_info_is_public(address(192, 168, 1, 1)));
    CHECK(!ip_info_is_public(address(192, 0, 2, 1)));
    CHECK(!ip_info_is_public(address(198, 18, 0, 1)));
    CHECK(!ip_info_is_public(address(198, 51, 100, 1)));
    CHECK(!ip_info_is_public(address(203, 0, 113, 1)));
    CHECK(!ip_info_is_public(address(224, 0, 0, 1)));

    CHECK(ip_info_parse_ptr("{\"Status\":0,\"Answer\":[{\"type\":5,\"data\":\"alias\"},"
                            "{\"type\":12,\"data\":\"dns.google.\"}]}", &result));
    CHECK(strcmp(result.ptr, "dns.google") == 0);
    CHECK(ip_info_parse_ptr("{\"Status\":3}", &result));
    CHECK(result.ptr[0] == '\0');
    CHECK(ip_info_parse_ptr("{\"Status\":0,\"Answer\":[]}", &result));
    CHECK(!ip_info_parse_ptr("{\"Status\":2}", &result));
    CHECK(!ip_info_parse_ptr("{}", &result));
    CHECK(!ip_info_parse_ptr("{\"Status\":0,\"Answer\":{}}", &result));
    CHECK(!ip_info_parse_ptr("{\"Status\":0} garbage", &result));
    CHECK(!ip_info_parse_ptr("{bad json", &result));
    CHECK(!ip_info_parse_ptr("[[[[[[[[[[[[[0]]]]]]]]]]]]]", &result));
    CHECK(ip_info_parse_ptr("{\"Status\":0,\"Answer\":[{\"type\":12,\"data\":\"line\\nbreak\"}]}", &result));
    CHECK(strcmp(result.ptr, "line?break") == 0);

    CHECK(ip_info_parse_location("{\"ip\":\"8.8.8.8\",\"success\":true,\"country\":\"US\","
                                "\"region\":\"California\",\"city\":\"Mountain View\","
                                "\"connection\":{\"asn\":15169,\"org\":\"Google LLC\",\"isp\":\"Google LLC\"}}", &result));
    CHECK(strcmp(result.location, "US / California / Mountain View") == 0);
    CHECK(strstr(result.network, "AS15169") != NULL && strstr(result.network, "Google LLC") != NULL);
    CHECK(!ip_info_parse_location("{\"ip\":\"1.1.1.1\",\"success\":true}", &result));
    CHECK(!ip_info_parse_location("{\"success\":false,\"message\":\"limit\"}", &result));
    CHECK(!ip_info_parse_location("[]", &result));
    CHECK(ip_info_parse_location("{\"ip\":\"8.8.8.8\",\"success\":true}", &result));
    CHECK(strstr(result.location, "Unknown") != NULL && strstr(result.network, "Unknown") != NULL);
    CHECK(ip_info_parse_location("{\"ip\":\"8.8.8.8\",\"success\":true,\"connection\":{\"asn\":4294967295}}", &result));
    CHECK(strstr(result.network, "AS4294967295") != NULL);
    ip_info_detail_t detail = {.info = {.ip = address(8, 8, 8, 8)}};
    CHECK(ip_info_parse_abuse("{\"data\":{\"ipAddress\":\"8.8.8.8\",\"abuseConfidenceScore\":0,"
        "\"totalReports\":0,\"numDistinctUsers\":0,\"lastReportedAt\":null,\"isTor\":false}}", &detail));
    CHECK(strstr(detail.abuse, "Score: 0/100") && strstr(detail.abuse, "Last report: None"));
    CHECK(strstr(detail.abuse, "Tor: No") && strstr(detail.abuse, "Usage: Unknown"));
    CHECK(ip_info_parse_abuse("{\"data\":{\"ipAddress\":\"8.8.8.8\",\"abuseConfidenceScore\":100,"
        "\"totalReports\":4294967295,\"numDistinctUsers\":2,\"isTor\":true,"
        "\"usageType\":\"Hosting\\nInjected\",\"lastReportedAt\":\"2026-11-06T00:00:00+00:00\"}}", &detail));
    CHECK(strstr(detail.abuse, "Reports: 4294967295") && strstr(detail.abuse, "Tor: Yes"));
    CHECK(strstr(detail.abuse, "Hosting?Injected"));
    CHECK(strstr(detail.abuse, "Last report: 2026-11-06\nUsage:"));
    const char *bad_abuse[] = {"{}", "[]", "{bad json", "[[[[[[[[[[[[[0]]]]]]]]]]]]]",
        "{\"errors\":[{\"detail\":\"Unauthorized\"}]}",
        "{\"data\":{\"ipAddress\":\"1.1.1.1\",\"abuseConfidenceScore\":0,\"totalReports\":0,\"numDistinctUsers\":0}}",
        "{\"data\":{\"ipAddress\":\"8.8.8.8\",\"abuseConfidenceScore\":101,\"totalReports\":0,\"numDistinctUsers\":0}}",
        "{\"data\":{\"ipAddress\":\"8.8.8.8\",\"abuseConfidenceScore\":0.5,\"totalReports\":0,\"numDistinctUsers\":0}}",
        "{\"data\":{\"ipAddress\":\"8.8.8.8\",\"abuseConfidenceScore\":0,\"totalReports\":-1,\"numDistinctUsers\":0}}",
        "{\"data\":{\"ipAddress\":\"8.8.8.8\",\"abuseConfidenceScore\":0,\"totalReports\":0}}",
        "{\"data\":{\"ipAddress\":\"8.8.8.8\",\"abuseConfidenceScore\":0,\"totalReports\":0,\"numDistinctUsers\":0,\"isTor\":\"false\"}}"};
    char before[sizeof(detail.abuse)];
    strcpy(before, detail.abuse);
    for (size_t i = 0; i < sizeof(bad_abuse) / sizeof(bad_abuse[0]); ++i) {
        CHECK(!ip_info_parse_abuse(bad_abuse[i], &detail));
        CHECK(strcmp(detail.abuse, before) == 0);
    }
    puts("PASS: IP/PTR/location parsing, AbuseIPDB identity/ranges/types/negative answers/control sanitization");
    return 0;
}
