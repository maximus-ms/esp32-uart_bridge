#pragma once

#include "esp_err.h"

/**
 * Start an HTTP server for OTA firmware updates.
 *   GET  /        - status page with upload form
 *   POST /update  - accepts firmware binary, flashes, reboots
 */
esp_err_t ota_server_start(void);
