#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "portal.h"
#include "wifi_mgr.h"

#define TAG "portal"
#define AP_IP "192.168.4.1"

static bool s_started;

static const char PORTAL_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>NiceMCU 配网</title>"
"<style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 12px}"
"input,button{width:100%;padding:10px;margin:6px 0;font-size:16px;box-sizing:border-box}"
"button{background:#07c160;color:#fff;border:0;border-radius:8px}</style></head><body>"
"<h3>NiceMCU WiFi 配置</h3>"
"<form method=POST action=/save>"
"<label>WiFi</label><input list=aps name=ssid required placeholder='选择或输入 SSID'>"
"<datalist id=aps></datalist>"
"<label>密码</label><input name=pass type=password>"
"<button type=submit>保存并连接</button>"
"</form><p id=msg></p>"
"<script>fetch('/scan').then(r=>r.json()).then(l=>{"
"const d=document.getElementById('aps');"
"l.forEach(a=>{const o=document.createElement('option');o.value=a.ssid;"
"o.label=a.ssid+' ('+a.rssi+'dBm)';d.appendChild(o)})}).catch(()=>{});"
"</script></body></html>";

static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static int json_escape(char *dst, int cap, const char *src)
{
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o < cap - 2; p++) {
        if (*p == '"' || *p == '\\') dst[o++] = '\\';
        if (*p >= 0x20) dst[o++] = (char)*p;
    }
    dst[o] = 0;
    return o;
}

static esp_err_t handler_scan(httpd_req_t *req)
{
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return httpd_resp_send(req, "[]", 2);
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof *recs);
    if (!recs) return httpd_resp_send(req, "[]", 2);
    esp_wifi_scan_get_ap_records(&n, recs);

    char *js = malloc(4096);
    if (!js) {
        free(recs);
        return httpd_resp_send(req, "[]", 2);
    }
    int off = 0;
    js[off++] = '[';
    for (int i = 0; i < n && off < 3500; i++) {
        if (!recs[i].ssid[0]) continue;
        if (off > 1) js[off++] = ',';
        off += snprintf(js + off, 4096 - off, "{\"ssid\":\"");
        off += json_escape(js + off, 4096 - off, (char *)recs[i].ssid);
        off += snprintf(js + off, 4096 - off, "\",\"rssi\":%d}", recs[i].rssi);
    }
    js[off++] = ']';
    js[off] = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, js, off);
    free(js);
    free(recs);
    return ESP_OK;
}

static void url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char h[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(h, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static void parse_form(char *body, char *ssid, size_t scap, char *pass, size_t pcap)
{
    char *p = body;
    while (p && *p) {
        char *amp = strchr(p, '&');
        if (amp) *amp = 0;
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            char *v = eq + 1;
            url_decode(v);
            if (!strcmp(p, "ssid")) strlcpy(ssid, v, scap);
            else if (!strcmp(p, "pass")) strlcpy(pass, v, pcap);
        }
        p = amp ? amp + 1 : NULL;
    }
}

static esp_err_t handler_save(httpd_req_t *req)
{
    char body[256];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof body) len = sizeof body - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) break;
        got += r;
    }
    body[got] = 0;

    char ssid[33] = {0}, pass[65] = {0};
    parse_form(body, ssid, sizeof ssid, pass, sizeof pass);
    if (!ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    wifi_save_creds(ssid, pass);
    ESP_LOGI(TAG, "wifi saved: %s", ssid);
    httpd_resp_send(req, "已保存，设备即将重启连接新网络", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handler_redirect(httpd_req_t *req)
{
    char loc[64];
    snprintf(loc, sizeof loc, "http://%s/", AP_IP);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void start_portal_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 4;
    cfg.stack_size = 8192;
    cfg.ctrl_port = 32769;
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "portal httpd start failed");
        return;
    }
    httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = handler_index },
        { .uri = "/scan", .method = HTTP_GET, .handler = handler_scan },
        { .uri = "/save", .method = HTTP_POST, .handler = handler_save },
        { .uri = "/*", .method = HTTP_GET, .handler = handler_redirect },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr = { .s_addr = INADDR_ANY },
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t rx[128], tx[160];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = recvfrom(sock, rx, sizeof rx, 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;
        memcpy(tx, rx, n);
        tx[2] = 0x81;
        tx[3] = 0x80;
        tx[4] = 0;
        tx[5] = 1;
        tx[6] = 0;
        tx[7] = 1;
        tx[n++] = 0xC0;
        tx[n++] = 0x0C;
        tx[n++] = 0;
        tx[n++] = 1;
        tx[n++] = 0;
        tx[n++] = 1;
        memset(tx + n, 0, 4);
        n += 4;
        tx[n++] = 0;
        tx[n++] = 4;
        uint32_t ip = ipaddr_addr(AP_IP);
        memcpy(tx + n, &ip, 4);
        n += 4;
        sendto(sock, tx, n, 0, (struct sockaddr *)&from, flen);
    }
}

static void portal_timeout_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(10 * 60 * 1000));
    ESP_LOGW(TAG, "portal timeout -> reboot");
    esp_restart();
}

void portal_start(const char *ap_ssid)
{
    if (s_started) return;
    s_started = true;
    esp_netif_create_default_wifi_ap();
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(ap_ssid);
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
    }

    xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL);
    start_portal_http();
    xTaskCreate(portal_timeout_task, "portal_to", 2048, NULL, 3, NULL);
    ESP_LOGI(TAG, "portal AP %s -> http://%s/", ap_ssid, AP_IP);
}
