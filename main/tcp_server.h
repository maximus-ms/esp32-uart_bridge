#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Start the TCP server on the configured port.
 * Accepts one client at a time. Provides functions for the bridge
 * to send/receive data through the active connection.
 */
esp_err_t tcp_server_start(void);

/**
 * Send data to the connected TCP client.
 * Returns number of bytes sent, or -1 on error / no client connected.
 */
int tcp_server_send(const uint8_t *data, size_t len);

/**
 * Receive data from the connected TCP client (non-blocking with timeout).
 * Returns number of bytes received, 0 on timeout, or -1 on error/disconnect.
 */
int tcp_server_recv(uint8_t *buf, size_t buf_size, int timeout_ms);

/**
 * Returns true if a TCP client is currently connected.
 */
bool tcp_server_has_client(void);
