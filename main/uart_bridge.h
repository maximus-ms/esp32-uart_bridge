#pragma once

#include "esp_err.h"
#include <stdint.h>

typedef struct {
    uint32_t uart_to_tcp_pkt;
    uint32_t uart_to_tcp_bytes;
    uint32_t tcp_to_uart_pkt;
    uint32_t tcp_to_uart_bytes;
    uint32_t uart_fifo_ovf;
    uint32_t uart_buf_full;
} bridge_stats_t;

esp_err_t uart_bridge_start(void);
bridge_stats_t bridge_get_stats(void);
