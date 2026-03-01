#include "uart_bridge.h"
#include "tcp_server.h"
#include "config.h"
#include "led.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "bridge";

#define BRIDGE_TASK_PRIO  10
#define BRIDGE_TASK_STACK 4096
#define UART_EVENT_QUEUE_SIZE 32
#define UART_RX_TIMEOUT_CHARS 3

static uart_port_t s_uart_num;
static QueueHandle_t s_uart_event_queue;
static volatile bridge_stats_t s_stats;

static uart_parity_t get_parity(int val)
{
    switch (val) {
    case 1:  return UART_PARITY_ODD;
    case 2:  return UART_PARITY_EVEN;
    default: return UART_PARITY_DISABLE;
    }
}

static uart_stop_bits_t get_stop_bits(int val)
{
    switch (val) {
    case 2:  return UART_STOP_BITS_1_5;
    case 3:  return UART_STOP_BITS_2;
    default: return UART_STOP_BITS_1;
    }
}

static uart_word_length_t get_data_bits(int val)
{
    switch (val) {
    case 5:  return UART_DATA_5_BITS;
    case 6:  return UART_DATA_6_BITS;
    case 7:  return UART_DATA_7_BITS;
    default: return UART_DATA_8_BITS;
    }
}

static esp_err_t uart_init(void)
{
    const bridge_config_t *cfg = config_get();
    s_uart_num = CONFIG_BRIDGE_UART_NUM;

    const uart_config_t uart_cfg = {
        .baud_rate  = cfg->uart_baud_rate,
        .data_bits  = get_data_bits(CONFIG_BRIDGE_UART_DATA_BITS),
        .parity     = get_parity(CONFIG_BRIDGE_UART_PARITY),
        .stop_bits  = get_stop_bits(CONFIG_BRIDGE_UART_STOP_BITS),
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(s_uart_num,
        CONFIG_BRIDGE_UART_RX_BUF_SIZE,
        CONFIG_BRIDGE_UART_TX_BUF_SIZE,
        UART_EVENT_QUEUE_SIZE, &s_uart_event_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(s_uart_num, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(s_uart_num,
        CONFIG_BRIDGE_UART_TX_GPIO,
        CONFIG_BRIDGE_UART_RX_GPIO,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE));

    ESP_ERROR_CHECK(uart_set_rx_timeout(s_uart_num, UART_RX_TIMEOUT_CHARS));

    ESP_LOGI(TAG, "UART%d: %d baud, TX=%d, RX=%d",
             s_uart_num, cfg->uart_baud_rate,
             CONFIG_BRIDGE_UART_TX_GPIO, CONFIG_BRIDGE_UART_RX_GPIO);
    return ESP_OK;
}

static void uart_to_tcp_task(void *arg)
{
    uint8_t *buf = malloc(CONFIG_BRIDGE_UART_RX_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate UART RX buffer");
        vTaskDelete(NULL);
        return;
    }

    esp_task_wdt_add(NULL);

    uart_event_t event;
    while (1) {
        esp_task_wdt_reset();
        if (!xQueueReceive(s_uart_event_queue, &event, pdMS_TO_TICKS(5000))) {
            continue;
        }
        switch (event.type) {
        case UART_DATA: {
            int len = uart_read_bytes(s_uart_num, buf, event.size, 0);
            if (len > 0) {
                tcp_server_send(buf, len);
                s_stats.uart_to_tcp_pkt++;
                s_stats.uart_to_tcp_bytes += len;
                led_notify_packet();
            }
            break;
        }
        case UART_FIFO_OVF:
            s_stats.uart_fifo_ovf++;
            ESP_LOGW(TAG, "UART FIFO overflow (#%lu)", (unsigned long)s_stats.uart_fifo_ovf);
            uart_flush_input(s_uart_num);
            xQueueReset(s_uart_event_queue);
            break;
        case UART_BUFFER_FULL:
            s_stats.uart_buf_full++;
            ESP_LOGW(TAG, "UART ring buffer full (#%lu)", (unsigned long)s_stats.uart_buf_full);
            uart_flush_input(s_uart_num);
            xQueueReset(s_uart_event_queue);
            break;
        default:
            break;
        }
    }
}

static void tcp_to_uart_task(void *arg)
{
    uint8_t *buf = malloc(CONFIG_BRIDGE_TCP_RX_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate TCP RX buffer");
        vTaskDelete(NULL);
        return;
    }

    esp_task_wdt_add(NULL);

    while (1) {
        esp_task_wdt_reset();
        int len = tcp_server_recv(buf, CONFIG_BRIDGE_TCP_RX_BUF_SIZE, 20);
        if (len > 0) {
            uart_write_bytes(s_uart_num, buf, len);
            s_stats.tcp_to_uart_pkt++;
            s_stats.tcp_to_uart_bytes += len;
            led_notify_packet();
        }
    }
}

esp_err_t uart_bridge_start(void)
{
    esp_err_t err = uart_init();
    if (err != ESP_OK) {
        return err;
    }

    xTaskCreate(uart_to_tcp_task, "uart2tcp", BRIDGE_TASK_STACK, NULL,
                BRIDGE_TASK_PRIO, NULL);
    xTaskCreate(tcp_to_uart_task, "tcp2uart", BRIDGE_TASK_STACK, NULL,
                BRIDGE_TASK_PRIO, NULL);

    ESP_LOGI(TAG, "Bridge started");
    return ESP_OK;
}

bridge_stats_t bridge_get_stats(void)
{
    return *(const bridge_stats_t *)&s_stats;
}
