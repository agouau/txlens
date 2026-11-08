#include "router.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "lwip/inet.h"
#include "nvs_flash.h"
#include "traffic_meter.h"
#include "wifi_settings.h"

static const char *TAG = "router";
static esp_netif_t *s_ap;
static esp_netif_t *s_sta;
static esp_timer_handle_t s_retry_timer;
static bool s_napt_enabled;
static esp_ip4_addr_t s_ap_ip;
static esp_ip4_addr_t s_dns_ip;
static bool s_sta_enabled;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static router_status_t s_status = { .state = ROUTER_CONNECTING, .sta_ip = "--" };

static void set_status(router_state_t state, const esp_ip4_addr_t *sta_ip)
{
    router_status_t status = { .state = state };
    snprintf(status.ap_ip, sizeof(status.ap_ip), IPSTR, IP2STR(&s_ap_ip));
    if (sta_ip != NULL) {
        snprintf(status.sta_ip, sizeof(status.sta_ip), IPSTR, IP2STR(sta_ip));
    } else {
        strcpy(status.sta_ip, "--");
    }
    portENTER_CRITICAL(&s_status_lock);
    s_status = status;
    portEXIT_CRITICAL(&s_status_lock);
}

router_status_t router_get_status(void)
{
    portENTER_CRITICAL(&s_status_lock);
    router_status_t status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    wifi_sta_list_t clients = {0};
    if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK) {
        status.clients = clients.num;
    }
    return status;
}

static void stop_napt(void)
{
    if (s_napt_enabled) {
        ESP_ERROR_CHECK(esp_netif_napt_disable(s_ap));
        s_napt_enabled = false;
    }
}

static void stop_dhcp_server(void)
{
    esp_err_t err = esp_netif_dhcps_stop(s_ap);
    if (err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_ERROR_CHECK(err);
    }
}

static esp_ip4_addr_t parse_ip(const char *address)
{
    esp_ip4_addr_t ip;
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(address, &ip));
    return ip;
}

static bool choose_ap_ip(const esp_netif_ip_info_t *upstream, esp_ip4_addr_t *chosen)
{
    const char *candidates[] = { ROUTER_AP_IP_CANDIDATES };
    uint32_t ap_mask = parse_ip(ROUTER_AP_NETMASK).addr;
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        esp_ip4_addr_t ip = parse_ip(candidates[i]);
        // Test both masks: either subnet may be wider than the other.
        uint32_t difference = ip.addr ^ upstream->ip.addr;
        if ((difference & upstream->netmask.addr) != 0 && (difference & ap_mask) != 0) {
            *chosen = ip;
            return true;
        }
    }
    return false;
}

static void configure_dhcp(esp_ip4_addr_t ap_ip, esp_netif_dns_info_t *dns)
{
    stop_dhcp_server();
    esp_netif_ip_info_t ap = {
        .ip = ap_ip,
        .gw = ap_ip,
        .netmask = parse_ip(ROUTER_AP_NETMASK),
    };
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap, &ap));
    uint8_t offer_dns = 0x02;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET,
                   ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, dns));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap));
    s_ap_ip = ap_ip;
    s_dns_ip = dns->ip.u_addr.ip4;
}

static void uplink_ready(const ip_event_got_ip_t *event)
{
    stop_napt();
    esp_ip4_addr_t ap_ip;
    if (!choose_ap_ip(&event->ip_info, &ap_ip)) {
        ESP_LOGE(TAG, "No non-overlapping AP subnet; change ROUTER_AP_IP");
        set_status(ROUTER_SUBNET_CONFLICT, &event->ip_info.ip);
        return;
    }

    esp_netif_dns_info_t dns = {0};
    ESP_ERROR_CHECK(esp_netif_get_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &dns));
    if (dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) {
        ESP_ERROR_CHECK(esp_netif_get_dns_info(s_sta, ESP_NETIF_DNS_BACKUP, &dns));
    }
    if (dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4 = parse_ip(ROUTER_FALLBACK_DNS);
    }

    bool lease_changed = ap_ip.addr != s_ap_ip.addr || dns.ip.u_addr.ip4.addr != s_dns_ip.addr;
    configure_dhcp(ap_ip, &dns);
    ESP_ERROR_CHECK(esp_netif_set_default_netif(s_sta));
    ESP_ERROR_CHECK(traffic_meter_attach(s_ap, s_sta));
    ESP_ERROR_CHECK(esp_netif_napt_enable(s_ap));
    s_napt_enabled = true;
    if (lease_changed) {
        // Existing leases retain old DNS/address information until renewal.
        // Reassociation makes phones/computers obtain the new DHCP settings.
        ESP_ERROR_CHECK(esp_wifi_deauth_sta(0));
    }
    set_status(ROUTER_READY, &event->ip_info.ip);
    ESP_LOGI(TAG, "NAT ready: STA=" IPSTR " AP=" IPSTR " DNS=" IPSTR,
             IP2STR(&event->ip_info.ip), IP2STR(&ap_ip), IP2STR(&dns.ip.u_addr.ip4));
}

static void reconnect(void *arg)
{
    if (!s_sta_enabled) return;
    wifi_ap_record_t upstream;
    if (esp_wifi_sta_get_ap_info(&upstream) == ESP_OK) {
        return; // Already associated; DHCP may still be obtaining an address.
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Connect request: %s; retrying", esp_err_to_name(err));
        ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, ROUTER_RETRY_MS * 1000ULL));
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            reconnect(NULL);
            break;
        case WIFI_EVENT_STA_CONNECTED:
            set_status(ROUTER_WAITING_IP, NULL);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *event = data;
            stop_napt();
            set_status(ROUTER_CONNECTING, NULL);
            ESP_LOGW(TAG, "Upstream disconnected, reason=%u; retry in %d ms",
                     event->reason, ROUTER_RETRY_MS);
            (void)esp_timer_stop(s_retry_timer);
            ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, ROUTER_RETRY_MS * 1000ULL));
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        (void)esp_timer_stop(s_retry_timer);
        uplink_ready(data);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        if (!s_napt_enabled) {
            return; // Ignore a delayed lost-IP event while already reconnecting.
        }
        stop_napt();
        set_status(ROUTER_WAITING_IP, NULL);
        esp_err_t err = esp_wifi_disconnect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Lost-IP disconnect: %s", esp_err_to_name(err));
            (void)esp_timer_stop(s_retry_timer);
            ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, ROUTER_RETRY_MS * 1000ULL));
        }
    }
}

void router_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(wifi_settings_init());
    wifi_settings_t settings = wifi_settings_get();
    s_sta_enabled = settings.sta_ssid[0] != 0;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap = esp_netif_create_default_wifi_ap();
    s_sta = esp_netif_create_default_wifi_sta();
    assert(s_ap != NULL && s_sta != NULL);

    s_ap_ip = parse_ip(ROUTER_AP_IP);
    esp_netif_dns_info_t dns = { .ip.type = ESP_IPADDR_TYPE_V4 };
    dns.ip.u_addr.ip4 = parse_ip(ROUTER_FALLBACK_DNS);
    configure_dhcp(s_ap_ip, &dns);
    set_status(s_sta_enabled ? ROUTER_CONNECTING : ROUTER_NOT_CONFIGURED, NULL);

    const esp_timer_create_args_t retry = { .callback = reconnect, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&retry, &s_retry_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, event_handler, NULL));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_sta_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    wifi_config_t sta = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .threshold.authmode = settings.sta_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    wifi_config_t ap = {
        .ap = {
            .ssid_len = strlen(settings.ap_ssid),
            .channel = ROUTER_AP_CHANNEL,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = ROUTER_AP_MAX_CONNECTIONS,
        },
    };
    memcpy(sta.sta.ssid, settings.sta_ssid, strlen(settings.sta_ssid));
    memcpy(sta.sta.password, settings.sta_password, strlen(settings.sta_password));
    memcpy(ap.ap.ssid, settings.ap_ssid, strlen(settings.ap_ssid));
    memcpy(ap.ap.password, settings.ap_password, strlen(settings.ap_password));
    if (s_sta_enabled) ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(wifi_settings_http_start());
    ESP_LOGI(TAG, "Wi-Fi settings available at http://" IPSTR "/", IP2STR(&s_ap_ip));
}
