#include "config.h"
#include "led.h"
#include "wifi_sta.h"
#include "tcp_server.h"
#include "uart_bridge.h"
#include "ota_server.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(led_init());

    const bridge_config_t *cfg = config_get();

    ESP_ERROR_CHECK(wifi_sta_init());

    if (cfg->hostname[0]) {
        ESP_ERROR_CHECK(mdns_init());
        ESP_ERROR_CHECK(mdns_hostname_set(cfg->hostname));
        mdns_instance_name_set("UART Bridge");
        mdns_service_add(NULL, "_tcp", "_tcp", cfg->tcp_port, NULL, 0);
        ESP_LOGI(TAG, "mDNS: %s.local", cfg->hostname);
    }

    ESP_ERROR_CHECK(tcp_server_start());
    ESP_ERROR_CHECK(uart_bridge_start());
    ESP_ERROR_CHECK(ota_server_start());

    ESP_LOGI(TAG, "Bridge ready: TCP %d, UART %d baud, HTTP %d",
             cfg->tcp_port, cfg->uart_baud_rate, cfg->ota_port);
}
