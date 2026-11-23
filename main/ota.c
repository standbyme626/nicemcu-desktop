#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include "ota.h"
#include "ota_version.h"

#ifndef CONFIG_DESKTOP_OTA_URL
#define CONFIG_DESKTOP_OTA_URL ""
#endif
#ifndef CONFIG_DESKTOP_OTA_CHECK_DELAY_S
#define CONFIG_DESKTOP_OTA_CHECK_DELAY_S 15
#endif

#define TAG "ota"
#define OTA_TASK_STACK 16384

static volatile ota_state_t s_state = OTA_IDLE;
static volatile int s_pct;
static volatile bool s_busy;
static bool s_boot_started;
static char s_err[96];

static void set_state(ota_state_t st, const char *err)
{
    s_state = st;
    if (st != OTA_DOWNLOADING && st != OTA_REBOOTING) s_pct = 0;
    if (err && err[0]) {
        snprintf(s_err, sizeof s_err, "%s", err);
    } else {
        s_err[0] = 0;
    }
}

bool ota_configured(void)
{
    return CONFIG_DESKTOP_OTA_URL[0] != 0;
}

ota_state_t ota_get_state(void)
{
    return s_state;
}

int ota_get_progress(void)
{
    return s_pct;
}

const char *ota_state_text(void)
{
    switch (s_state) {
    case OTA_CHECKING:
        return "正在检查更新";
    case OTA_DOWNLOADING:
        return "正在下载更新";
    case OTA_UP_TO_DATE:
        return "已是最新版本";
    case OTA_FAILED:
        return "更新失败";
    case OTA_REBOOTING:
        return "更新完成，即将重启";
    default:
        return "待命";
    }
}

const char *ota_last_error(void)
{
    return s_err;
}

static bool json_str(const char *js, const char *key, char *out, size_t cap)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return false;
    size_t n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static int http_fetch(const char *url, char *buf, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .keep_alive_enable = true,
    };
    if (strncmp(url, "https://", 8) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c);
        return -1;
    }

    int hlen = esp_http_client_fetch_headers(c);
    int code = esp_http_client_get_status_code(c);
    if (code != 200 || (hlen > 0 && (size_t)hlen >= cap)) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return -1;
    }

    int total = 0;
    int n;
    while ((n = esp_http_client_read(c, buf + total, cap - (size_t)total - 1)) > 0) {
        total += n;
        if ((size_t)total >= cap - 1) break;
    }
    buf[total] = 0;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return total;
}

void ota_mark_valid_if_pending(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;
    ESP_LOGI(TAG, "marking %s valid", run->label);
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) ESP_LOGW(TAG, "mark valid failed: %s", esp_err_to_name(err));
}

static void ota_check_and_update(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    if (!ota_configured()) {
        ESP_LOGW(TAG, "OTA URL not configured");
        set_state(OTA_FAILED, "未配置 OTA 地址");
        return;
    }

    set_state(OTA_CHECKING, NULL);
    char buf[1024];
    int n = http_fetch(CONFIG_DESKTOP_OTA_URL, buf, sizeof buf);
    if (n <= 0) {
        ESP_LOGW(TAG, "manifest fetch failed");
        set_state(OTA_FAILED, "manifest 获取失败");
        return;
    }

    char project[32] = "", version[32] = "", url[256] = "";
    if (!json_str(buf, "version", version, sizeof version) ||
        !json_str(buf, "url", url, sizeof url) || !version[0] || !url[0]) {
        ESP_LOGW(TAG, "bad manifest");
        set_state(OTA_FAILED, "manifest 格式错误");
        return;
    }
    json_str(buf, "project", project, sizeof project);
    if (project[0] && strcmp(project, app->project_name) != 0) {
        ESP_LOGE(TAG, "project mismatch: %s vs %s", project, app->project_name);
        set_state(OTA_FAILED, "manifest 项目不匹配");
        return;
    }

    ESP_LOGI(TAG, "running %s v%s, manifest v%s", app->project_name, app->version, version);
    if (!ota_version_is_newer(app->version, version)) {
        ESP_LOGI(TAG, "up to date");
        set_state(OTA_UP_TO_DATE, NULL);
        return;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "no OTA slot");
        set_state(OTA_FAILED, "无可用 OTA 槽");
        return;
    }

    ESP_LOGI(TAG, "upgrade %s -> %s from %s", app->version, version, url);
    set_state(OTA_DOWNLOADING, NULL);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    if (strncmp(url, "https://", 8) == 0) http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        s_pct = 0;
        set_state(OTA_DOWNLOADING, attempt > 1 ? "重试中" : NULL);
        esp_https_ota_handle_t h = NULL;
        err = esp_https_ota_begin(&ota_cfg, &h);
        if (err == ESP_OK) {
            int total = esp_https_ota_get_image_size(h);
            while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                int read = esp_https_ota_get_image_len_read(h);
                if (total > 0) s_pct = (int)((int64_t)read * 100 / total);
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            if (err == ESP_OK) err = esp_https_ota_finish(h);
            else esp_https_ota_abort(h);
        }
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "ota attempt %d failed: %s", attempt, esp_err_to_name(err));
        if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(5000));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota failed: %s", esp_err_to_name(err));
        set_state(OTA_FAILED, esp_err_to_name(err));
        return;
    }
    s_pct = 100;

    esp_app_desc_t nd;
    memset(&nd, 0, sizeof nd);
    if (esp_ota_get_partition_description(target, &nd) == ESP_OK) {
        if (strcmp(nd.version, version) != 0 || strcmp(nd.project_name, app->project_name) != 0) {
            ESP_LOGE(TAG, "image mismatch: %s v%s, want v%s", nd.project_name, nd.version, version);
            esp_ota_set_boot_partition(esp_ota_get_running_partition());
            set_state(OTA_FAILED, "镜像校验失败");
            return;
        }
        ESP_LOGI(TAG, "image verified: %s v%s", nd.project_name, nd.version);
    }

    set_state(OTA_REBOOTING, NULL);
    ESP_LOGI(TAG, "OTA done, rebooting");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static bool wifi_ready(void)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    return nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0;
}

static void ota_run(bool boot)
{
    if (boot || wifi_ready()) ota_mark_valid_if_pending();
    ota_check_and_update();
    ota_mark_valid_if_pending();
    s_busy = false;
}

static void ota_task(void *arg)
{
    (void)arg;
    ota_run(false);
    vTaskDelete(NULL);
}

static void ota_boot_task(void *arg)
{
    (void)arg;
    int delay = CONFIG_DESKTOP_OTA_CHECK_DELAY_S;
    if (delay > 0) vTaskDelay(pdMS_TO_TICKS((uint32_t)delay * 1000));
    ota_run(true);
    vTaskDelete(NULL);
}

void ota_start_boot_check(void)
{
    if (s_boot_started || s_busy) return;
    s_boot_started = true;
    s_busy = true;
    if (xTaskCreate(ota_boot_task, "ota_boot", OTA_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        s_boot_started = false;
        ESP_LOGE(TAG, "no mem for ota task");
    }
}

bool ota_request_check(void)
{
    if (s_busy) return false;
    s_busy = true;
    if (xTaskCreate(ota_task, "ota_manual", OTA_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        ESP_LOGE(TAG, "no mem for ota task");
        return false;
    }
    return true;
}
