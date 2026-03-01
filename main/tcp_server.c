#include "tcp_server.h"
#include "config.h"
#include "led.h"

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "tcp_srv";

static int s_listen_fd = -1;
static int s_client_fd = -1;
static SemaphoreHandle_t s_mutex;

static bool ip_is_allowed(const struct sockaddr_in *addr)
{
    const char *allowed = config_get()->tcp_allowed_ip;
    if (allowed[0] == '\0') {
        return true;
    }
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
    return strcmp(client_ip, allowed) == 0;
}

static void close_client(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_client_fd >= 0) {
        close(s_client_fd);
        s_client_fd = -1;
        led_set_state(LED_WIFI_CONNECTED);
        ESP_LOGI(TAG, "Client disconnected");
    }
    xSemaphoreGive(s_mutex);
}

static void accept_task(void *arg)
{
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int new_fd = accept(s_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (new_fd < 0) {
            ESP_LOGE(TAG, "accept() failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));

        if (!ip_is_allowed(&client_addr)) {
            ESP_LOGW(TAG, "Rejected connection from %s (not in allowed list)", addr_str);
            close(new_fd);
            continue;
        }

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_client_fd >= 0) {
            ESP_LOGW(TAG, "Replacing existing client with %s", addr_str);
            close(s_client_fd);
        }
        s_client_fd = new_fd;
        xSemaphoreGive(s_mutex);

        int flag = 1;
        setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        int keepalive = 1;
        int keepidle = 5;
        int keepintvl = 5;
        int keepcnt = 3;
        setsockopt(new_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        setsockopt(new_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
        setsockopt(new_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
        setsockopt(new_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

        led_set_state(LED_TCP_CONNECTED);
        ESP_LOGI(TAG, "Client connected: %s", addr_str);
    }
}

esp_err_t tcp_server_start(void)
{
    const bridge_config_t *cfg = config_get();

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(cfg->tcp_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }

    if (listen(s_listen_fd, 1) < 0) {
        ESP_LOGE(TAG, "listen() failed: errno %d", errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Listening on port %d", cfg->tcp_port);

    xTaskCreate(accept_task, "tcp_accept", 3072, NULL, 5, NULL);
    return ESP_OK;
}

int tcp_server_send(const uint8_t *data, size_t len)
{
    int ret = -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_client_fd >= 0) {
        ret = send(s_client_fd, data, len, MSG_NOSIGNAL);
        if (ret < 0) {
            ESP_LOGW(TAG, "send() failed: errno %d", errno);
            close(s_client_fd);
            s_client_fd = -1;
        }
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

int tcp_server_recv(uint8_t *buf, size_t buf_size, int timeout_ms)
{
    int fd;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    fd = s_client_fd;
    xSemaphoreGive(s_mutex);

    if (fd < 0) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return 0;
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    int sel = select(fd + 1, &readfds, NULL, NULL, &tv);
    if (sel < 0) {
        if (errno == EBADF) {
            close_client();
        }
        return -1;
    }
    if (sel == 0) {
        return 0;
    }

    int n = recv(fd, buf, buf_size, 0);
    if (n <= 0) {
        close_client();
        return -1;
    }
    return n;
}

bool tcp_server_has_client(void)
{
    bool has;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    has = (s_client_fd >= 0);
    xSemaphoreGive(s_mutex);
    return has;
}
