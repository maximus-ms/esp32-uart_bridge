#include "config.h"

#include <string.h>
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "config";
static const char *NS  = "bridge";

static bridge_config_t s_cfg;

static void load_defaults(void)
{
    strncpy(s_cfg.wifi_ssid, CONFIG_BRIDGE_WIFI_SSID, sizeof(s_cfg.wifi_ssid) - 1);
    strncpy(s_cfg.wifi_password, CONFIG_BRIDGE_WIFI_PASSWORD, sizeof(s_cfg.wifi_password) - 1);
    s_cfg.wifi_tx_power = CONFIG_BRIDGE_WIFI_TX_POWER;
    strncpy(s_cfg.static_ip,   CONFIG_BRIDGE_STATIC_IP,   sizeof(s_cfg.static_ip) - 1);
    strncpy(s_cfg.static_gw,   CONFIG_BRIDGE_STATIC_GW,   sizeof(s_cfg.static_gw) - 1);
    strncpy(s_cfg.static_mask, CONFIG_BRIDGE_STATIC_MASK,  sizeof(s_cfg.static_mask) - 1);
    strncpy(s_cfg.static_dns,  CONFIG_BRIDGE_STATIC_DNS,   sizeof(s_cfg.static_dns) - 1);

    s_cfg.uart_baud_rate = CONFIG_BRIDGE_UART_BAUD_RATE;

    s_cfg.tcp_port = CONFIG_BRIDGE_TCP_PORT;
    strncpy(s_cfg.tcp_allowed_ip, CONFIG_BRIDGE_TCP_ALLOWED_IP,
            sizeof(s_cfg.tcp_allowed_ip) - 1);

    s_cfg.ota_port = CONFIG_BRIDGE_OTA_PORT;

    strncpy(s_cfg.hostname, CONFIG_BRIDGE_HOSTNAME, sizeof(s_cfg.hostname) - 1);
}

#define NVS_READ_STR(h, key, dst) do {          \
        size_t _l = sizeof(dst);                \
        nvs_get_str(h, key, dst, &_l);          \
    } while (0)

#define NVS_READ_I32(h, key, dst) do {          \
        int32_t _v;                             \
        if (nvs_get_i32(h, key, &_v) == ESP_OK)\
            dst = (int)_v;                      \
    } while (0)

esp_err_t config_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    load_defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved config, using Kconfig defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s, using defaults", esp_err_to_name(err));
        return ESP_OK;
    }

    NVS_READ_STR(h, "wifi_ssid",   s_cfg.wifi_ssid);
    NVS_READ_STR(h, "wifi_pass",   s_cfg.wifi_password);
    NVS_READ_I32(h, "wifi_txpwr",  s_cfg.wifi_tx_power);
    NVS_READ_STR(h, "static_ip",   s_cfg.static_ip);
    NVS_READ_STR(h, "static_gw",   s_cfg.static_gw);
    NVS_READ_STR(h, "static_mask", s_cfg.static_mask);
    NVS_READ_STR(h, "static_dns",  s_cfg.static_dns);

    NVS_READ_I32(h, "uart_baud",   s_cfg.uart_baud_rate);

    NVS_READ_I32(h, "tcp_port",    s_cfg.tcp_port);
    NVS_READ_STR(h, "tcp_ip",      s_cfg.tcp_allowed_ip);

    NVS_READ_I32(h, "ota_port",    s_cfg.ota_port);

    NVS_READ_STR(h, "hostname",    s_cfg.hostname);

    nvs_close(h);

    ESP_LOGI(TAG, "Loaded config from NVS");
    return ESP_OK;
}

const bridge_config_t *config_get(void)
{
    return &s_cfg;
}

esp_err_t config_save(const bridge_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, "wifi_ssid",   cfg->wifi_ssid);
    nvs_set_str(h, "wifi_pass",   cfg->wifi_password);
    nvs_set_i32(h, "wifi_txpwr",  cfg->wifi_tx_power);
    nvs_set_str(h, "static_ip",   cfg->static_ip);
    nvs_set_str(h, "static_gw",   cfg->static_gw);
    nvs_set_str(h, "static_mask", cfg->static_mask);
    nvs_set_str(h, "static_dns",  cfg->static_dns);

    nvs_set_i32(h, "uart_baud",   cfg->uart_baud_rate);

    nvs_set_i32(h, "tcp_port",    cfg->tcp_port);
    nvs_set_str(h, "tcp_ip",      cfg->tcp_allowed_ip);

    nvs_set_i32(h, "ota_port",    cfg->ota_port);

    nvs_set_str(h, "hostname",    cfg->hostname);

    err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Config saved to NVS");
    return err;
}

esp_err_t config_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config reset to defaults");
    return ESP_OK;
}
