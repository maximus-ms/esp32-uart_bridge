#include "led.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_GPIO      CONFIG_BRIDGE_LED_GPIO
#define LED_ACTIVE_LO CONFIG_BRIDGE_LED_ACTIVE_LOW
#define TICK_MS       25

static volatile led_state_t s_state = LED_WIFI_CONNECTING;
static volatile int s_pkt_hold;

static inline void led_on(void)  { gpio_set_level(LED_GPIO, LED_ACTIVE_LO ? 0 : 1); }
static inline void led_off(void) { gpio_set_level(LED_GPIO, LED_ACTIVE_LO ? 1 : 0); }

static void led_task(void *arg)
{
    uint32_t ms = 0;
    bool is_on = false;

    while (1) {
        led_state_t st = s_state;
        bool want = false;

        switch (st) {
        case LED_WIFI_CONNECTING:
            want = (ms % 500) < 250;
            break;
        case LED_WIFI_CONNECTED:
            want = (ms % 5000) < 50;
            break;
        case LED_TCP_CONNECTED:
            want = (ms % 1000) < 50;
            break;
        }

        if (s_pkt_hold > 0) {
            want = true;
            s_pkt_hold--;
        }

        if (want != is_on) {
            if (want) led_on(); else led_off();
            is_on = want;
        }

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        ms += TICK_MS;
    }
}

esp_err_t led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    led_off();

    xTaskCreate(led_task, "led", 1024, NULL, 1, NULL);
    return ESP_OK;
}

void led_set_state(led_state_t state)
{
    s_state = state;
}

void led_notify_packet(void)
{
    s_pkt_hold = 2;
}
