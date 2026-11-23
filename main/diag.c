#include "sdkconfig.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#ifdef CONFIG_SPIRAM
#include "esp_psram.h"
#endif

#include "app_ui.h"
#include "diag.h"
#include "ota.h"
#include "wifi_mgr.h"
#include "screen.h"
#include "settings.h"

#define LOGBUF_SIZE 8192

static char s_logbuf[LOGBUF_SIZE];
static volatile size_t s_loglen;
static portMUX_TYPE s_logmux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf;

static int log_ring_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    char tmp[256];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
        taskENTER_CRITICAL(&s_logmux);
        if (s_loglen + (size_t)n + 1 > LOGBUF_SIZE) {
            size_t drop = s_loglen / 2;
            memmove(s_logbuf, s_logbuf + drop, s_loglen - drop);
            s_loglen -= drop;
        }
        memcpy(s_logbuf + s_loglen, tmp, n);
        s_loglen += n;
        s_logbuf[s_loglen] = 0;
        taskEXIT_CRITICAL(&s_logmux);
    }
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : 0;
}

void diag_log_install(void)
{
    if (s_prev_vprintf) return;
    s_prev_vprintf = esp_log_set_vprintf(log_ring_vprintf);
    s_loglen = 0;
    s_logbuf[0] = 0;
}

static size_t log_tail_start(int want)
{
    size_t len = s_loglen;
    int lines = 0;
    for (size_t i = len; i > 0; i--) {
        if (s_logbuf[i - 1] == '\n') {
            if (++lines > want) return i;
        }
    }
    return 0;
}

const char *diag_log_tail(int want, size_t *len)
{
    if (want <= 0) want = 200;
    size_t start = log_tail_start(want);
    *len = s_loglen - start;
    return s_logbuf + start;
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
    }
}

int diag_build_status(char *buf, size_t cap)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    desktop_settings_t *cfg = settings_get();

    char ip[16], ssid[33];
    wifi_get_ip(ip, sizeof ip);
    wifi_get_ssid(ssid, sizeof ssid);
    int rssi = wifi_get_rssi();

    unsigned psram_size = 0, psram_free = 0;
#ifdef CONFIG_SPIRAM
    psram_size = (unsigned)esp_psram_get_size();
    psram_free = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif

    int qn = 0, qstart = 0, qtotal = 0;
    app_get_queue_info(&qn, &qstart, &qtotal);
    return snprintf(buf, cap,
        "{\"proj\":\"%s\",\"ver\":\"%s\",\"idf\":\"%s\",\"slot\":\"%s\","
        "\"reset\":\"%s\",\"heap\":%u,\"psram\":%u,\"psram_free\":%u,"
        "\"internal\":%u,\"internal_largest\":%u,"
        "\"uptime_s\":%lu,\"ip\":\"%s\",\"rssi\":%d,\"ssid\":\"%s\","
        "\"portal\":%d,\"ap\":\"%s\",\"bright\":%u,\"asleep\":%d,\"pomo\":[%u,%u],\"media_host\":\"%s\","
        "\"queue\":{\"n\":%d,\"start\":%d,\"total\":%d},"
        "\"ota\":{\"state\":\"%s\",\"pct\":%d,\"err\":\"%s\"}}\n",
        app->project_name, app->version, esp_get_idf_version(),
        run ? run->label : "?", reset_reason_str(esp_reset_reason()),
        (unsigned)esp_get_free_heap_size(), psram_size, psram_free,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned long)(esp_timer_get_time() / 1000000),
        ip, rssi, ssid, wifi_is_portal() ? 1 : 0, wifi_portal_ssid(), (unsigned)cfg->bright, screen_is_asleep() ? 1 : 0,
        cfg->pomo_work_min, cfg->pomo_break_min, cfg->media_host,
        qn, qstart, qtotal,
        ota_state_text(), ota_get_progress(), ota_last_error());
}
