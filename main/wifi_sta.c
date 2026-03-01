#include "wifi_sta.h"
#include "config.h"
#include "led.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"

static const char *TAG = "wifi_sta";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_events;
static int s_retry_count;
static bool s_connected;
static bool s_started;
static int64_t s_disconnect_time_us;

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_started = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)data;
        if (s_connected) {
            s_disconnect_time_us = esp_timer_get_time();
        }
        s_connected = false;
        led_set_state(LED_WIFI_CONNECTING);

        int reboot_min = CONFIG_BRIDGE_WIFI_REBOOT_TIMEOUT;
        if (reboot_min > 0 && s_disconnect_time_us > 0) {
            int64_t elapsed_s = (esp_timer_get_time() - s_disconnect_time_us) / 1000000;
            if (elapsed_s >= reboot_min * 60) {
                ESP_LOGE(TAG, "WiFi down for %d min, rebooting...", reboot_min);
                esp_restart();
            }
        }

        int max_retry = CONFIG_BRIDGE_WIFI_MAX_RETRY;
        if (max_retry == 0 || s_retry_count < max_retry) {
            int delay_ms = (s_retry_count < 2) ? 1000 : 3000;
            ESP_LOGW(TAG, "Disconnected (reason %d). Retry in %d ms (#%d)...",
                     dis->reason, delay_ms, s_retry_count + 1);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            esp_wifi_connect();
            s_retry_count++;
        } else {
            ESP_LOGE(TAG, "Max retries (%d) reached", max_retry);
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        const bridge_config_t *c = config_get();
        ESP_LOGI(TAG, "Connected. IP: " IPSTR " (%s)",
                 IP2STR(&event->ip_info.ip),
                 c->static_ip[0] ? "static" : "DHCP");
        s_retry_count = 0;
        s_connected = true;
        s_disconnect_time_us = 0;
        led_set_state(LED_WIFI_CONNECTED);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void do_scan(const char *ssid)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = (uint8_t *)ssid,
        .show_hidden = true,
    };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed");
        return;
    }
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGW(TAG, "No APs found with SSID \"%s\"", ssid);
        return;
    }
    wifi_ap_record_t *aps = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!aps) return;
    esp_wifi_scan_get_ap_records(&ap_count, aps);
    for (int i = 0; i < ap_count; i++) {
        ESP_LOGI(TAG, "AP: \"%s\" bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%d rssi=%d auth=%d",
                 aps[i].ssid,
                 aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                 aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5],
                 aps[i].primary, aps[i].rssi, aps[i].authmode);
    }
    free(aps);
}

esp_err_t wifi_sta_init(void)
{
    const bridge_config_t *cfg = config_get();

    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    if (cfg->static_ip[0] != '\0') {
        esp_netif_dhcpc_stop(netif);
        esp_netif_ip_info_t ip_info = {0};
        ip_info.ip.addr      = esp_ip4addr_aton(cfg->static_ip);
        ip_info.gw.addr      = cfg->static_gw[0]   ? esp_ip4addr_aton(cfg->static_gw)   : ip_info.ip.addr;
        ip_info.netmask.addr = cfg->static_mask[0]  ? esp_ip4addr_aton(cfg->static_mask) : esp_ip4addr_aton("255.255.255.0");
        ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));

        if (cfg->static_dns[0]) {
            esp_netif_dns_info_t dns = {0};
            dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(cfg->static_dns);
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
        }
        ESP_LOGI(TAG, "Static IP: %s  GW: %s  Mask: %s",
                 cfg->static_ip,
                 cfg->static_gw[0]  ? cfg->static_gw  : "(same as IP)",
                 cfg->static_mask[0] ? cfg->static_mask : "255.255.255.0");
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable  = false,
                .required = false,
            },
        },
    };
    strncpy((char *)wifi_cfg.sta.ssid, cfg->wifi_ssid,
            sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, cfg->wifi_password,
            sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* Wait for STA_START before scanning */
    while (!s_started) vTaskDelay(pdMS_TO_TICKS(10));

    int8_t tx_power = (int8_t)cfg->wifi_tx_power;
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(tx_power));
    ESP_LOGI(TAG, "TX power: %.2f dBm", tx_power * 0.25);

    do_scan(cfg->wifi_ssid);

    ESP_LOGI(TAG, "Connecting to \"%s\" (%s)...",
             cfg->wifi_ssid,
             cfg->static_ip[0] ? cfg->static_ip : "DHCP");

    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Failed to connect to WiFi");
    return ESP_FAIL;
}

bool wifi_sta_is_connected(void)
{
    return s_connected;
}
