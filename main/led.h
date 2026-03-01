#pragma once

#include "esp_err.h"

typedef enum {
    LED_WIFI_CONNECTING,
    LED_WIFI_CONNECTED,
    LED_TCP_CONNECTED,
} led_state_t;

esp_err_t led_init(void);
void led_set_state(led_state_t state);
void led_notify_packet(void);
