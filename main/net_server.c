#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include "app_ui.h"
#include "diag.h"
#include "net_server.h"
#include "ota.h"
#include "screen.h"
#include "settings.h"
#include "wifi_mgr.h"

#ifndef CONFIG_DESKTOP_HTTP_TOKEN
#define CONFIG_DESKTOP_HTTP_TOKEN ""
#endif

#define TAG "http"
#define HTTP_PORT 8080
#define TOKEN_MAX 64

static httpd_handle_t s_server;
static bool s_started;
static char s_token[TOKEN_MAX];

static bool auth_ok(httpd_req_t *req)
{
    if (!s_token[0]) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "token not configured\n", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    char v[TOKEN_MAX];
    if (httpd_req_get_hdr_value_str(req, "X-Token", v, sizeof v) == ESP_OK &&
        strcmp(v, s_token) == 0) {
        return true;
    }
    char q[256];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "token", v, sizeof v) == ESP_OK &&
        strcmp(v, s_token) == 0) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "auth required\n", HTTPD_RESP_USE_STRLEN);
    return false;
}

static esp_err_t send_text(httpd_req_t *req, const char *text)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_root(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    char page[1200];
    snprintf(page, sizeof page,
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>nicemcu-desktop</title>"
        "<style>body{font-family:sans-serif;background:#111;color:#ddd;padding:16px}"
        "a{color:#6cf}img{width:320px;max-width:100%%;image-rendering:pixelated;"
        "border:1px solid #333;border-radius:8px}</style></head><body>"
        "<h2>nicemcu-desktop v%s</h2><p>slot %s</p>"
        "<p><a href='/status?token=%s'>status</a> | "
        "<a href='/logs?token=%s'>logs</a> | "
        "<a href='/sleep?token=%s'>sleep</a> | "
        "<a href='/wake?token=%s'>wake</a> | "
        "<a href='/ui?page=settings&token=%s'>settings</a></p>"
        "<p><img src='/screenshot.bmp?token=%s'></p></body></html>",
        app->version, run ? run->label : "?",
        s_token, s_token, s_token, s_token, s_token, s_token);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_status(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    char js[1024];
    diag_build_status(js, sizeof js);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, js, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_logs(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    int want = 200;
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(q, "n", v, sizeof v) == ESP_OK) {
            want = atoi(v);
            if (want < 1) want = 200;
            if (want > 1000) want = 1000;
        }
    }
    size_t len = 0;
    const char *p = diag_log_tail(want, &len);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, p, len);
}

static esp_err_t handler_sleep(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    screen_sleep();
    return send_text(req, "ok\n");
}

static esp_err_t handler_wake(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    screen_wake();
    return send_text(req, "ok\n");
}

static esp_err_t handler_bright(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    int pct = 60;
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(q, "p", v, sizeof v) == ESP_OK) pct = atoi(v);
    }
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    screen_wake();
    screen_set_bright(pct);
    settings_get()->bright = (uint8_t)pct;
    settings_save();
    char msg[16];
    snprintf(msg, sizeof msg, "%d\n", pct);
    return send_text(req, msg);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t handler_reboot(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    esp_err_t r = send_text(req, "rebooting\n");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return r;
}

static esp_err_t handler_portal(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    wifi_start_portal();
    char msg[64];
    snprintf(msg, sizeof msg, "portal %s\n", wifi_portal_ssid());
    return send_text(req, msg);
}

static esp_err_t handler_ota_check(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    bool ok = ota_request_check();
    return send_text(req, ok ? "started\n" : "busy\n");
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static esp_err_t handler_screenshot(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    if (!screen_snapshot_begin()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "screenshot unavailable\n", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint32_t image = (uint32_t)SCREEN_W * SCREEN_H * 3;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(hdr + 2, 54 + image);
    put32(hdr + 10, 54);
    put32(hdr + 14, 40);
    put32(hdr + 18, SCREEN_W);
    put32(hdr + 22, SCREEN_H);
    put16(hdr + 26, 1);
    put16(hdr + 28, 24);
    put32(hdr + 34, image);

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)hdr, sizeof hdr);

    const uint16_t *snap = screen_snapshot_data();
    uint8_t row[SCREEN_W * 3];
    for (int y = SCREEN_H - 1; y >= 0 && err == ESP_OK; y--) {
        for (int x = 0; x < SCREEN_W; x++) {
            uint16_t v = snap[y * SCREEN_W + x];
            uint8_t r = (v >> 11) & 0x1F;
            uint8_t g = (v >> 5) & 0x3F;
            uint8_t b = v & 0x1F;
            row[x * 3 + 0] = (uint8_t)((b * 255 + 15) / 31);
            row[x * 3 + 1] = (uint8_t)((g * 255 + 31) / 63);
            row[x * 3 + 2] = (uint8_t)((r * 255 + 15) / 31);
        }
        err = httpd_resp_send_chunk(req, (const char *)row, sizeof row);
    }
    screen_snapshot_end();
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_ui(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    int page = APP_UI_NONE;
    char q[96];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        char v[24];
        if (httpd_query_key_value(q, "page", v, sizeof v) == ESP_OK) {
            if (!strcmp(v, "dashboard")) page = APP_UI_DASHBOARD;
            else if (!strcmp(v, "media")) page = APP_UI_MEDIA;
            else if (!strcmp(v, "settings")) page = APP_UI_SETTINGS;
            else if (!strcmp(v, "queue")) page = APP_UI_QUEUE;
            else if (!strcmp(v, "calib")) page = APP_UI_CALIB;
            else if (!strcmp(v, "about")) page = APP_UI_ABOUT;
            else if (!strcmp(v, "ota")) page = APP_UI_OTA;
        }
    }
    if (page) app_ui_request(page);
    return send_text(req, page ? "ok\n" : "unknown page\n");
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

static esp_err_t handler_cfg(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    desktop_settings_t *cfg = settings_get();
    char q[512];
    bool changed = false;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        char v[128];
        if (httpd_query_key_value(q, "bright", v, sizeof v) == ESP_OK) {
            int n = atoi(v);
            if (n >= 5 && n <= 100) { cfg->bright = (uint8_t)n; screen_set_bright(n); changed = true; }
        }
        if (httpd_query_key_value(q, "pomo_work", v, sizeof v) == ESP_OK) {
            int n = atoi(v);
            if (n >= 1 && n <= 180) { cfg->pomo_work_min = (uint16_t)n; changed = true; }
        }
        if (httpd_query_key_value(q, "pomo_break", v, sizeof v) == ESP_OK) {
            int n = atoi(v);
            if (n >= 1 && n <= 60) { cfg->pomo_break_min = (uint16_t)n; changed = true; }
        }
        if (httpd_query_key_value(q, "lat", v, sizeof v) == ESP_OK) {
            url_decode(v); strlcpy(cfg->lat, v, sizeof cfg->lat); changed = true;
        }
        if (httpd_query_key_value(q, "lon", v, sizeof v) == ESP_OK) {
            url_decode(v); strlcpy(cfg->lon, v, sizeof cfg->lon); changed = true;
        }
        if (httpd_query_key_value(q, "media_host", v, sizeof v) == ESP_OK) {
            url_decode(v); strlcpy(cfg->media_host, v, sizeof cfg->media_host); changed = true;
        }
    }
    if (changed) settings_save();
    char js[384];
    snprintf(js, sizeof js,
             "{\"ok\":true,\"bright\":%u,\"pomo_work\":%u,\"pomo_break\":%u,"
             "\"lat\":\"%s\",\"lon\":\"%s\",\"media_host\":\"%s\"}\n",
             cfg->bright, cfg->pomo_work_min, cfg->pomo_break_min,
             cfg->lat, cfg->lon, cfg->media_host);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, js, HTTPD_RESP_USE_STRLEN);
}

void net_server_start(void)
{
    if (s_started) return;
    s_started = true;
    snprintf(s_token, sizeof s_token, "%s", CONFIG_DESKTOP_HTTP_TOKEN);
    if (!s_token[0]) ESP_LOGW(TAG, "HTTP token empty; API returns 503");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        s_started = false;
        return;
    }

    httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = handler_root },
        { .uri = "/status", .method = HTTP_GET, .handler = handler_status },
        { .uri = "/logs", .method = HTTP_GET, .handler = handler_logs },
        { .uri = "/sleep", .method = HTTP_GET, .handler = handler_sleep },
        { .uri = "/wake", .method = HTTP_GET, .handler = handler_wake },
        { .uri = "/bright", .method = HTTP_GET, .handler = handler_bright },
        { .uri = "/reboot", .method = HTTP_GET, .handler = handler_reboot },
        { .uri = "/ota/check", .method = HTTP_GET, .handler = handler_ota_check },
        { .uri = "/screenshot.bmp", .method = HTTP_GET, .handler = handler_screenshot },
        { .uri = "/ui", .method = HTTP_GET, .handler = handler_ui },
        { .uri = "/cfg", .method = HTTP_GET, .handler = handler_cfg },
        { .uri = "/portal", .method = HTTP_GET, .handler = handler_portal },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "HTTP API on :%d", HTTP_PORT);
}
