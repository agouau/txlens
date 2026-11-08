"""Exercise the real HTTP access guard/handlers with deterministic socket stubs."""
from pathlib import Path
import subprocess
import tempfile

PLATFORM = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_ARG 3
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_408_REQ_TIMEOUT 408
#define HTTPD_SOCK_ERR_TIMEOUT -2
#define HTTP_GET 0
#define HTTP_POST 1
#define AF_INET 2
typedef unsigned socklen_t;
struct in_addr { uint32_t s_addr; };
struct sockaddr { int unused; };
struct sockaddr_in { int sin_family; struct in_addr sin_addr; };
struct ifreq { char ifr_name[16]; };
typedef struct { size_t content_len, offset; const char *host, *password, *body; } httpd_req_t;
typedef void *httpd_handle_t;
typedef struct {
    int stack_size, max_open_sockets, recv_wait_timeout, send_wait_timeout;
    bool lru_purge_enable;
    struct ifreq *if_name;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
typedef int esp_netif_t;
static int ap_netif;
static bool ap_present = true;
static char bound_name[16], response[8192];
static int response_status, saves, key_saves, route_count, password_reads;
static unsigned char image[8192];
static size_t image_size;
static int capture_calls, capture_error, chunk_failure;
static int64_t fake_tick;
static uint32_t local_address = 0x0132a8c0, random_state;
static uint32_t inet_addr(const char *text) { return !strcmp(text,"192.168.50.1") ? 0x0132a8c0 : 0x1401a8c0; }
static int getsockname(int fd, struct sockaddr *addr, socklen_t *size) {
    (void)fd; (void)size;
    struct sockaddr_in *local = (struct sockaddr_in *)addr;
    local->sin_family = AF_INET; local->sin_addr.s_addr = local_address; return 0;
}
static int httpd_req_to_sockfd(httpd_req_t *req) { (void)req; return 1; }
static size_t httpd_req_get_hdr_value_len(httpd_req_t *req,const char *name) {
    assert(!strcmp(name,"X-Setup-Password")); return req->password ? strlen(req->password) : 0;
}
static int httpd_req_get_hdr_value_str(httpd_req_t *req,const char *name,char *out,size_t size) {
    if (strcmp(name,"Host") && strcmp(name,"X-Setup-Password")) return ESP_FAIL;
    if (!strcmp(name,"X-Setup-Password")) ++password_reads;
    const char *value = !strcmp(name,"Host") ? req->host : req->password;
    if (!value || strlen(value) >= size) return ESP_FAIL;
    strcpy(out,value); return ESP_OK;
}
static int httpd_resp_set_hdr(httpd_req_t *req,const char *name,const char *value) { (void)req; (void)name; (void)value; return 0; }
static int httpd_resp_set_type(httpd_req_t *req,const char *value) { (void)req; (void)value; return 0; }
static int httpd_resp_set_status(httpd_req_t *req,const char *value) { (void)req; response_status=atoi(value); return 0; }
static int httpd_resp_sendstr(httpd_req_t *req,const char *text) { (void)req; snprintf(response,sizeof(response),"%s",text); return 0; }
static int httpd_resp_send_err(httpd_req_t *req,int status,const char *text) { response_status=status; return httpd_resp_sendstr(req,text); }
static int httpd_resp_send(httpd_req_t *req,const char *text,ptrdiff_t size) { (void)size; return httpd_resp_sendstr(req,text); }
static int httpd_resp_send_chunk(httpd_req_t *req,const char *data,size_t size) {
    (void)req;
    if (chunk_failure) return ESP_FAIL;
    assert(image_size+size<=sizeof(image));
    if (size) memcpy(image+image_size,data,size);
    image_size+=size; return ESP_OK;
}
static int httpd_req_recv(httpd_req_t *req,char *out,size_t size) {
    size_t remaining = req->content_len - req->offset;
    if (size > remaining) size=remaining;
    memcpy(out,req->body+req->offset,size); req->offset+=size; return (int)size;
}
static int64_t esp_timer_get_time(void) { return fake_tick; }
static uint32_t esp_random(void) { ++random_state; return random_state == 1 ? UINT32_MAX : random_state == 2 ? 42 : random_state * 2654435761U; }
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { assert(!strcmp(key,"WIFI_AP_DEF")); return ap_present ? &ap_netif : NULL; }
static int esp_netif_get_netif_impl_name(esp_netif_t *netif,char *name) { assert(netif==&ap_netif); strcpy(name,"ap1"); return 0; }
static int httpd_start(httpd_handle_t *handle,const httpd_config_t *cfg) { assert(cfg->if_name); strcpy(bound_name,cfg->if_name->ifr_name); *handle=(void *)1; return 0; }
static int httpd_stop(httpd_handle_t handle) { (void)handle; return 0; }
static int httpd_register_uri_handler(httpd_handle_t handle,const httpd_uri_t *route) { (void)handle; assert(route->handler); ++route_count; return 0; }
'''

TEST = r'''
#include "platform.h"
#include "wifi_settings.h"
#include "router.h"
wifi_settings_t wifi_settings_get(void) { return (wifi_settings_t){.ap_ssid="Device"}; }
bool wifi_settings_saved(void) { return true; }
bool abuseipdb_key_get(char key[ABUSEIPDB_KEY_CAPACITY]) { key[0]=0; return false; }
router_status_t router_get_status(void) { return (router_status_t){.state=ROUTER_READY,.ap_ip="192.168.50.1"}; }
bool wifi_settings_parse(const char *json,wifi_settings_t *settings) { (void)settings; return !strcmp(json,"{}"); }
esp_err_t wifi_settings_save(const wifi_settings_t *settings) { (void)settings; ++saves; return 0; }
bool abuseipdb_key_parse(const char *json,char key[ABUSEIPDB_KEY_CAPACITY]) { (void)json; strcpy(key,"test-key"); return true; }
esp_err_t abuseipdb_key_save(const char *key) { assert(!strcmp(key,"test-key")); ++key_saves; return 0; }
esp_err_t display_capture_rgb565(uint16_t **pixels,int *width,int *height) {
    ++capture_calls;
    if (capture_error) return ESP_FAIL;
    const uint16_t colors[]={0xf800,0x07e0,0x001f,0xffff,0,0x8410};
    *width=3; *height=2; *pixels=malloc(sizeof(colors)); assert(*pixels);
    memcpy(*pixels,colors,sizeof(colors)); return ESP_OK;
}
#include "settings_http.c"
static void reset_response(void) { response_status=200; response[0]=0; image_size=0; }
int main(void) {
    assert(wifi_settings_http_start()==0 && !strcmp(bound_name,"ap1") && route_count==5);
    char password[7]; strcpy(password,wifi_settings_setup_password()); assert(strlen(password)==6 && !strcmp(password,"000042"));
    httpd_req_t req={.host="192.168.50.1",.body="{}",.content_len=2};
    reset_response(); page(&req); assert(response_status==200); // Public shell contains no secret.
    reset_response(); state(&req); assert(response_status==403 && !strstr(response,password));
    // Even the correct candidate must not be read/compared during the interval.
    req.password=password;
    int reads=password_reads;
    reset_response(); state(&req); assert(response_status==429 && password_reads==reads);
    reset_response(); save(&req); assert(response_status==429 && !saves);
    reset_response(); save_abuseipdb(&req); assert(response_status==429 && !key_saves);
    fake_tick=499999;
    reset_response(); state(&req); assert(response_status==429 && password_reads==reads);
    ++fake_tick;
    reset_response(); state(&req); assert(response_status==200 && password_reads==reads+1);
    reset_response(); save(&req); assert(response_status==429 && !saves);
    fake_tick+=500000;
    req.password="000042000000";
    reset_response(); save(&req); assert(response_status==403 && !saves);
    fake_tick+=500000;
    req.password="00004";
    reset_response(); save(&req); assert(response_status==403 && !saves);
    req.password="000000";
    // No cumulative lockout: every exact half-second permits one new attempt.
    for (unsigned i=0;i<20;++i) {
        fake_tick+=500000; reset_response(); state(&req); assert(response_status==403);
    }
    req.password=password; reset_response(); state(&req); assert(response_status==429);
    fake_tick+=500000;
    req.password=password; reset_response(); state(&req);
    assert(response_status==200 && strstr(response,"ap_ssid") && !strstr(response,password));
    assert(!strstr(response,"token") && !strstr(response,"setup_password"));
    req.host="evil.example"; reset_response(); save(&req); assert(response_status==403 && !saves);
    req.host="192.168.50.1"; local_address=0x1401a8c0;
    reset_response(); save(&req); assert(response_status==403 && !saves);
    local_address=0x0132a8c0; req.host="192.168.50.1:80";
    fake_tick+=500000;
    reset_response(); save(&req); assert(response_status==200 && saves==1);
    fake_tick+=500000;
    req.offset=0; reset_response(); save_abuseipdb(&req); assert(response_status==200 && key_saves==1);
    // Navigation shows a password prompt; no framebuffer is read before auth.
    req.password=NULL; reset_response(); screenshot(&req);
    assert(response_status==200 && strstr(response,"prompt(") && !capture_calls && !image_size);
    req.password="000000"; fake_tick+=500000; reset_response(); screenshot(&req);
    assert(response_status==403 && !capture_calls && !image_size);
    req.password=password; reset_response(); screenshot(&req);
    assert(response_status==429 && !capture_calls);
    req.host="evil.example"; fake_tick+=500000; reset_response(); screenshot(&req);
    assert(response_status==403 && !capture_calls);
    req.host="192.168.50.1"; local_address=0x1401a8c0;
    reset_response(); screenshot(&req); assert(response_status==403 && !capture_calls);
    local_address=0x0132a8c0;
    reset_response(); assert(screenshot(&req)==ESP_OK && response_status==200 && capture_calls==1);
    assert(image_size==78 && image[0]=='B' && image[1]=='M' && image[2]==78);
    assert(image[10]==54 && image[14]==40 && image[18]==3);
    assert(image[22]==254 && image[23]==255 && image[24]==255 && image[25]==255);
    assert(image[26]==1 && image[28]==24 && image[30]==0 && image[34]==24);
    const unsigned char expected[]={0,0,255, 0,255,0, 255,0,0, 0,0,0,
                                    255,255,255, 0,0,0, 132,130,132, 0,0,0};
    assert(!memcmp(image+54,expected,sizeof(expected))); // BGR, row order and padding.
    capture_error=1; fake_tick+=500000; reset_response(); screenshot(&req);
    assert(response_status==500 && !image_size); capture_error=0;
    chunk_failure=1; fake_tick+=500000; reset_response();
    assert(screenshot(&req)==ESP_FAIL); chunk_failure=0;
    assert(wifi_settings_http_start()==0 && strcmp(password,wifi_settings_setup_password()));
    reset_response(); state(&req); assert(response_status==403); // Previous boot's password is invalid.
    ap_present=false; assert(wifi_settings_http_start()==ESP_ERR_INVALID_STATE);
    puts("PASS: six-digit code, boot rotation, AP binding, no disclosure, strict 500ms verification interval without cumulative lockout");
    return 0;
}
'''

root = Path(__file__).resolve().parents[1]
cjson = root / "managed_components/espressif__cjson/cJSON"
with tempfile.TemporaryDirectory(prefix="settings-http-") as directory:
    work = Path(directory)
    (work / "platform.h").write_text(PLATFORM, encoding="utf-8")
    for name in ("esp_err.h", "esp_http_server.h", "esp_random.h", "esp_timer.h",
                 "esp_netif.h", "lwip/sockets.h"):
        path = work / name
        path.parent.mkdir(exist_ok=True)
        path.write_text('#include "platform.h"\n', encoding="utf-8")
    source = (root / "main/settings_http.c").read_text(encoding="utf-8")
    source = source.replace('extern const char settings_html_start[] asm("_binary_settings_html_start");',
                            'static const char settings_html_start[]="<html></html>";')
    source = source.replace('extern const char settings_html_end[] asm("_binary_settings_html_end");',
                            '#define settings_html_end (settings_html_start+sizeof(settings_html_start))')
    (work / "settings_http.c").write_text(source, encoding="utf-8")
    (work / "test.c").write_text(TEST, encoding="utf-8")
    subprocess.run(["cl", "/nologo", "/c", "/O2", str(cjson / "cJSON.c"),
                    f"/Fo{work / 'cjson.obj'}"], check=True)
    subprocess.run(["cl", "/nologo", "/std:c11", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
                    f"/I{work}", f"/I{root / 'main'}", f"/I{cjson}", str(work / "test.c"),
                    str(work / "cjson.obj"), f"/Fo{work}\\", f"/Fe{work / 'test.exe'}"], check=True)
    subprocess.run([str(work / "test.exe")], check=True)
