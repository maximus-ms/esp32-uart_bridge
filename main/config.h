#pragma once

#include "esp_err.h"

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    int  wifi_tx_power;
    char static_ip[16];
    char static_gw[16];
    char static_mask[16];
    char static_dns[16];

    int  uart_baud_rate;

    int  tcp_port;
    char tcp_allowed_ip[16];

    int  ota_port;

    char hostname[33];
} bridge_config_t;

/**
 * Load config from NVS. Falls back to Kconfig defaults for missing keys.
 * Must be called after nvs_flash_init().
 */
esp_err_t config_init(void);

/** Return pointer to the current runtime config. */
const bridge_config_t *config_get(void);

/** Save config to NVS. Does NOT reboot. */
esp_err_t config_save(const bridge_config_t *cfg);

/** Erase all saved config from NVS (reverts to Kconfig defaults on reboot). */
esp_err_t config_reset(void);
