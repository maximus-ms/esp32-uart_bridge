#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialize WiFi in STA mode and connect to the configured AP.
 * Blocks until connection is established or maximum retries exceeded.
 * Returns ESP_OK on successful connection, ESP_FAIL otherwise.
 */
esp_err_t wifi_sta_init(void);

/**
 * Returns true if WiFi is currently connected and has an IP address.
 */
bool wifi_sta_is_connected(void);
