#include "ota_server.h"
#include "config.h"
#include "uart_bridge.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"

static const char *TAG = "web";

/* ── URL-decode helpers ─────────────────────────────────────────── */

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0;
    while (*src && i < dst_size - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static const char *find_param(const char *body, const char *key, size_t key_len)
{
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == body || p[-1] == '&') && p[key_len] == '=')
            return p + key_len + 1;
        p += key_len;
    }
    return NULL;
}

static bool form_str(const char *body, const char *key, char *dst, size_t dst_size)
{
    size_t klen = strlen(key);
    const char *v = find_param(body, key, klen);
    if (!v) return false;
    const char *end = strchr(v, '&');
    size_t vlen = end ? (size_t)(end - v) : strlen(v);
    char *tmp = malloc(vlen + 1);
    if (!tmp) return false;
    memcpy(tmp, v, vlen);
    tmp[vlen] = '\0';
    url_decode(dst, tmp, dst_size);
    free(tmp);
    return true;
}

static bool form_int(const char *body, const char *key, int *dst)
{
    char buf[16];
    if (!form_str(body, key, buf, sizeof(buf))) return false;
    *dst = atoi(buf);
    return true;
}

/* ── HTML: combined settings + OTA page ─────────────────────────── */

static const char HTML_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' "
    "content='width=device-width,initial-scale=1'><title>UART Bridge</title>"
    "<link rel='icon' href=\"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"
    "<rect rx='20' width='100' height='100' fill='%231e1e2e'/>"
    "<text x='50' y='68' font-size='50' text-anchor='middle' fill='%2394e2d5'>UB</text></svg>\">"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font:14px/1.6 system-ui,sans-serif;background:#1e1e2e;color:#cdd6f4;padding:16px}"
    ".c{max-width:480px;margin:0 auto}"
    "h1{font-size:1.3em;margin-bottom:12px;text-align:center;color:#cdd6f4}"
    ".g{background:#24243a;border:1px solid #313244;border-radius:6px;padding:0 12px 12px;margin-top:12px}"
    "h2{font-size:1em;background:#313244;padding:8px 12px;margin:0 -12px 8px;"
    "border-radius:6px 6px 0 0;color:#a6adc8}"
    "label{display:block;margin:4px 0 1px;font-size:.85em;color:#7f849c}"
    "input,select{width:100%;padding:6px 8px;border:1px solid #45475a;border-radius:4px;"
    "font-size:14px;margin-bottom:4px;background:#181825;color:#cdd6f4}"
    "input:focus,select:focus{outline:none;border-color:#94e2d5}"
    ".r{display:flex;gap:8px;flex-wrap:wrap}"
    ".r>div{flex:1;min-width:120px}"
    ".btns{display:flex;gap:8px;margin-top:12px}"
    ".btns button{flex:1;padding:8px 0;border:none;border-radius:4px;cursor:pointer;"
    "font-size:14px;color:#1e1e2e}"
    ".sv{background:#a6e3a1}.sv:hover{background:#94e2d5}"
    ".rs{background:#fab387}.rs:hover{background:#f9e2af}"
    "table{color:#bac2de;width:100%;font-size:13px;border-collapse:collapse}"
    "table td:last-child{text-align:right}"
    ".foot{margin-top:16px;font-size:.8em;color:#585b70;text-align:center}"
    "#prog{display:none;margin:8px 0;color:#f9e2af}"
    "hr{border:none;border-top:1px solid #313244;margin:16px 0}"
    "</style></head><body><div class='c'>";

static const char HTML_TAIL[] =
    "</div></body></html>";

static void chunk(httpd_req_t *req, const char *s)
{
    httpd_resp_sendstr_chunk(req, s);
}

static void chunk_fmt(httpd_req_t *req, char *buf, size_t bsz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, bsz, fmt, ap);
    va_end(ap);
    httpd_resp_sendstr_chunk(req, buf);
}

/* ── Dropdown option tables ─────────────────────────────────────── */

typedef struct { int val; const char *label; } opt_t;

static const opt_t txpwr_opts[] = {
    {  8,  "2 dBm"},   {20,  "5 dBm"},   {28,  "7 dBm"},    {34,  "8.5 dBm"},
    {44, "11 dBm"},    {52, "13 dBm"},    {60, "15 dBm"},
};
#define TXPWR_OPTS_N (sizeof(txpwr_opts)/sizeof(txpwr_opts[0]))

static const int baud_opts[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 250000, 460800, 500000, 921600, 1000000
};
#define BAUD_OPTS_N (sizeof(baud_opts)/sizeof(baud_opts[0]))

/* ── GET / ──────────────────────────────────────────────────────── */

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    const bridge_config_t *c = config_get();
    const esp_app_desc_t *app = esp_app_get_description();
    char b[256];

    httpd_resp_set_type(req, "text/html");
    chunk(req, HTML_HEAD);

    chunk(req, "<h1>UART Bridge</h1>");

    /* Statistics */
    bridge_stats_t st = bridge_get_stats();

    wifi_ap_record_t ap_info;
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    int64_t uptime_s = esp_timer_get_time() / 1000000;
    int up_d = uptime_s / 86400;
    int up_h = (uptime_s % 86400) / 3600;
    int up_m = (uptime_s % 3600) / 60;

    chunk(req, "<div class='g'><h2>Statistics</h2><table>");
    chunk_fmt(req, b, sizeof(b),
        "<tr><td>TCP &rarr; UART</td><td>%lu pkt / %lu bytes</td></tr>",
        (unsigned long)st.tcp_to_uart_pkt, (unsigned long)st.tcp_to_uart_bytes);
    chunk_fmt(req, b, sizeof(b),
        "<tr><td>UART &rarr; TCP</td><td>%lu pkt / %lu bytes</td></tr>",
        (unsigned long)st.uart_to_tcp_pkt, (unsigned long)st.uart_to_tcp_bytes);
    if (st.uart_fifo_ovf || st.uart_buf_full) {
        chunk_fmt(req, b, sizeof(b),
            "<tr><td>UART errors</td><td>%lu ovf / %lu full</td></tr>",
            (unsigned long)st.uart_fifo_ovf, (unsigned long)st.uart_buf_full);
    }
    chunk_fmt(req, b, sizeof(b),
        "<tr><td>WiFi RSSI</td><td>%d dBm</td></tr>", rssi);
    chunk_fmt(req, b, sizeof(b),
        "<tr><td>Uptime</td><td>%dd %dh %dm</td></tr></table>",
        up_d, up_h, up_m);
    chunk(req, "</div>");

    chunk(req, "<form id='sf' method='POST' action='/settings'>");

    /* WiFi */
    chunk_fmt(req, b, sizeof(b),
        "<div class='g'><h2>WiFi</h2>"
        "<label>SSID</label><input name='wifi_ssid' value='%s' maxlength='32'>"
        "<label>Password</label><input name='wifi_pass' type='password' value='%s' maxlength='64'>",
        c->wifi_ssid, c->wifi_password);

    chunk(req, "<label>TX Power</label><select name='wifi_txpwr'>");
    for (int i = 0; i < (int)TXPWR_OPTS_N; i++) {
        chunk_fmt(req, b, sizeof(b),
            "<option value='%d'%s>%s</option>",
            txpwr_opts[i].val,
            c->wifi_tx_power == txpwr_opts[i].val ? " selected" : "",
            txpwr_opts[i].label);
    }
    chunk(req, "</select>");

    chunk_fmt(req, b, sizeof(b),
        "<label>Static IP (empty=DHCP)</label>"
        "<input name='static_ip' value='%s' maxlength='15' placeholder='e.g. 192.168.1.50'>",
        c->static_ip);
    chunk(req, "<div class='r'>");
    chunk_fmt(req, b, sizeof(b),
        "<div><label>Gateway</label>"
        "<input name='static_gw' value='%s' maxlength='15' placeholder='192.168.1.1'></div>",
        c->static_gw);
    chunk_fmt(req, b, sizeof(b),
        "<div><label>Netmask</label>"
        "<input name='static_mask' value='%s' maxlength='15' placeholder='255.255.255.0'></div>",
        c->static_mask);
    chunk(req, "</div>");
    chunk_fmt(req, b, sizeof(b),
        "<label>DNS</label><input name='static_dns' value='%s' maxlength='15' placeholder='8.8.8.8'>",
        c->static_dns);

    /* UART */
    chunk(req, "</div><div class='g'><h2>UART</h2><label>Baud Rate</label><select name='uart_baud'>");
    for (int i = 0; i < (int)BAUD_OPTS_N; i++) {
        chunk_fmt(req, b, sizeof(b),
            "<option value='%d'%s>%d</option>",
            baud_opts[i],
            c->uart_baud_rate == baud_opts[i] ? " selected" : "",
            baud_opts[i]);
    }
    chunk(req, "</select>");

    /* Network */
    chunk(req, "</div><div class='g'><h2>Network</h2><div class='r'>");
    chunk_fmt(req, b, sizeof(b),
        "<div><label>TCP Port</label>"
        "<input name='tcp_port' type='number' value='%d' min='1' max='65535'></div>",
        c->tcp_port);
    chunk_fmt(req, b, sizeof(b),
        "<div><label>HTTP Port</label>"
        "<input name='ota_port' type='number' value='%d' min='1' max='65535'></div>",
        c->ota_port);
    chunk(req, "</div>");
    chunk_fmt(req, b, sizeof(b),
        "<label>Allowed IP (empty=any, bridge only)</label>"
        "<input name='tcp_ip' value='%s' maxlength='15' placeholder='e.g. 192.168.1.10'>",
        c->tcp_allowed_ip);
    chunk_fmt(req, b, sizeof(b),
        "<label>Hostname (.local)</label>"
        "<input name='hostname' value='%s' maxlength='32' placeholder='uart-bridge'>",
        c->hostname);

    /* Buttons */
    chunk(req,
        "</div></form>"
        "<form id='rf' method='POST' action='/reset'></form>"
        "<div class='btns'>"
        "<button type='submit' form='sf' class='sv'>Save &amp; Reboot</button>"
        "<button type='submit' form='rf' class='rs' "
        "onclick=\"return confirm('Reset all settings?')\">"
        "Reset to Defaults</button>"
        "</div>");

    /* Footer */
    chunk_fmt(req, b, sizeof(b),
        "<div class='foot'>v%s &middot; built %s</div>",
        app->version, app->date);

    chunk(req, HTML_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── POST /settings ─────────────────────────────────────────────── */

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    int body_len = req->content_len;
    if (body_len <= 0 || body_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad form data");
        return ESP_FAIL;
    }

    char *body = malloc(body_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int off = 0;
    while (off < body_len) {
        int n = httpd_req_recv(req, body + off, body_len - off);
        if (n <= 0) { free(body); return ESP_FAIL; }
        off += n;
    }
    body[body_len] = '\0';

    bridge_config_t cfg = *config_get();

    form_str(body, "wifi_ssid",  cfg.wifi_ssid,      sizeof(cfg.wifi_ssid));
    form_str(body, "wifi_pass",  cfg.wifi_password,   sizeof(cfg.wifi_password));
    form_int(body, "wifi_txpwr", &cfg.wifi_tx_power);
    form_str(body, "static_ip",   cfg.static_ip,   sizeof(cfg.static_ip));
    form_str(body, "static_gw",   cfg.static_gw,   sizeof(cfg.static_gw));
    form_str(body, "static_mask", cfg.static_mask,  sizeof(cfg.static_mask));
    form_str(body, "static_dns",  cfg.static_dns,   sizeof(cfg.static_dns));

    form_int(body, "uart_baud",  &cfg.uart_baud_rate);

    form_int(body, "tcp_port",   &cfg.tcp_port);
    form_str(body, "tcp_ip",     cfg.tcp_allowed_ip, sizeof(cfg.tcp_allowed_ip));

    form_int(body, "ota_port",   &cfg.ota_port);

    form_str(body, "hostname",   cfg.hostname,     sizeof(cfg.hostname));

    free(body);

    esp_err_t err = config_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }

    char resp[256];
    snprintf(resp, sizeof(resp),
        "<html><body><h2>Saved</h2><p>Rebooting...</p>"
        "<script>setTimeout(function(){location='http://'+location.hostname+':%d/'},8000)</script>"
        "</body></html>", cfg.ota_port);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, resp);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ── POST /reset ────────────────────────────────────────────────── */

static esp_err_t reset_post_handler(httpd_req_t *req)
{
    config_reset();

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body><h2>Reset</h2><p>Defaults restored. Rebooting...</p>"
        "<script>setTimeout(function(){location='/'},8000)</script>"
        "</body></html>");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ── POST /update (OTA) ────────────────────────────────────────── */

static esp_err_t update_handler(httpd_req_t *req)
{
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA target: %s (0x%"PRIx32", 0x%"PRIx32")",
             update_part->label, update_part->address, update_part->size);

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(CONFIG_BRIDGE_OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int total = 0;
    bool header_ok = false;

    ESP_LOGI(TAG, "Receiving %d bytes...", remaining);

    while (remaining > 0) {
        int n = httpd_req_recv(req, buf,
            remaining < CONFIG_BRIDGE_OTA_BUF_SIZE ? remaining : CONFIG_BRIDGE_OTA_BUF_SIZE);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Receive error");
            break;
        }

        if (!header_ok) {
            if (n < (int)sizeof(esp_image_header_t)) break;
            if (((esp_image_header_t *)buf)->magic != ESP_IMAGE_HEADER_MAGIC) {
                ESP_LOGE(TAG, "Bad image magic");
                break;
            }
            header_ok = true;
        }

        err = esp_ota_write(ota, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            break;
        }
        remaining -= n;
        total += n;
    }
    free(buf);

    if (remaining != 0) {
        esp_ota_abort(ota);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Validation failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_ota_set_boot_partition(update_part));
    ESP_LOGI(TAG, "OTA OK (%d bytes). Rebooting...", total);

    int port = config_get()->ota_port;
    char resp[256];
    snprintf(resp, sizeof(resp),
        "<html><body><h2>OK</h2><p>Flashed %d bytes. Rebooting...</p>"
        "<script>setTimeout(function(){location='http://'+location.hostname+':%d/'},12000)</script>"
        "</body></html>", total, port);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, resp);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ── Start HTTP server ──────────────────────────────────────────── */

esp_err_t ota_server_start(void)
{
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port    = config_get()->ota_port;
    hcfg.stack_size     = 8192;
    hcfg.task_priority  = 3;
    hcfg.max_uri_handlers = 8;

    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &hcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",        .method = HTTP_GET,  .handler = settings_get_handler },
        { .uri = "/settings",.method = HTTP_POST, .handler = settings_post_handler },
        { .uri = "/reset",   .method = HTTP_POST, .handler = reset_post_handler },
        { .uri = "/update",  .method = HTTP_POST, .handler = update_handler },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP server on port %d", config_get()->ota_port);
    return ESP_OK;
}
