#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "settings.h"

#define TAG "settings"
#define NS "desk"
#define KEY "cfg"
#define VERSION 1

static desktop_settings_t s_cfg;

static void load_defaults(void)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.version = VERSION;
    s_cfg.bright = 60;
    s_cfg.pomo_work_min = 25;
    s_cfg.pomo_break_min = 5;
    snprintf(s_cfg.lat, sizeof s_cfg.lat, "%s", CONFIG_DESKTOP_LAT);
    snprintf(s_cfg.lon, sizeof s_cfg.lon, "%s", CONFIG_DESKTOP_LON);
    snprintf(s_cfg.media_host, sizeof s_cfg.media_host, "%s", CONFIG_DESKTOP_MEDIA_HOST);
    s_cfg.ta = 0.0f;
    s_cfg.tb = 1.0f;
    s_cfg.tc = 0.0f;
    s_cfg.td = -1.0f;
    s_cfg.te = 0.0f;
    s_cfg.tf = 239.0f;
}

void settings_init(void)
{
    load_defaults();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    desktop_settings_t tmp;
    size_t len = sizeof tmp;
    if (nvs_get_blob(h, KEY, &tmp, &len) == ESP_OK && len == sizeof tmp && tmp.version == VERSION) {
        s_cfg = tmp;
    }
    nvs_close(h);
    if (s_cfg.bright < 5 || s_cfg.bright > 100) s_cfg.bright = 60;
    if (s_cfg.pomo_work_min < 1 || s_cfg.pomo_work_min > 180) s_cfg.pomo_work_min = 25;
    if (s_cfg.pomo_break_min < 1 || s_cfg.pomo_break_min > 60) s_cfg.pomo_break_min = 5;
    ESP_LOGI(TAG, "bright=%u pomo=%u/%u host=%s", s_cfg.bright,
             s_cfg.pomo_work_min, s_cfg.pomo_break_min, s_cfg.media_host);
}

desktop_settings_t *settings_get(void)
{
    return &s_cfg;
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, KEY, &s_cfg, sizeof s_cfg);
    nvs_commit(h);
    nvs_close(h);
}

void settings_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    load_defaults();
}
