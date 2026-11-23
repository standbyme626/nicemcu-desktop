#include "sdkconfig.h"

#include <string.h>

#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "screen.h"
#include "settings.h"

#define TAG "screen"

static SemaphoreHandle_t s_scr_mtx;
static SemaphoreHandle_t s_fb_mtx;
static SemaphoreHandle_t s_snap_mtx;
static uint8_t *s_shadow;
static uint8_t *s_snap;
static volatile int s_bright = 60;
static volatile bool s_asleep;

void screen_init(int bl_gpio)
{
    s_scr_mtx = xSemaphoreCreateMutex();
    s_fb_mtx = xSemaphoreCreateMutex();
    s_snap_mtx = xSemaphoreCreateMutex();

    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t ch = {
        .gpio_num = bl_gpio, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));

    size_t bytes = (size_t)SCREEN_W * SCREEN_H * 2;
#ifdef CONFIG_SPIRAM
    s_shadow = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    s_snap = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#endif
    if (!s_shadow) s_shadow = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!s_snap) s_snap = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (s_shadow) memset(s_shadow, 0, bytes);
    if (s_snap) memset(s_snap, 0, bytes);
    ESP_LOGI(TAG, "shadow=%p snap=%p heap=%u psram=%u internal=%u largest=%u", s_shadow, s_snap,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    screen_set_bright(settings_get()->bright);
}

void screen_set_bright(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    if (s_scr_mtx) xSemaphoreTake(s_scr_mtx, portMAX_DELAY);
    s_bright = pct;
    if (!s_asleep) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    if (s_scr_mtx) xSemaphoreGive(s_scr_mtx);
}

int screen_get_bright(void)
{
    return s_bright;
}

void screen_sleep(void)
{
    if (s_scr_mtx) xSemaphoreTake(s_scr_mtx, portMAX_DELAY);
    s_asleep = true;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (s_scr_mtx) xSemaphoreGive(s_scr_mtx);
}

void screen_wake(void)
{
    if (s_scr_mtx) xSemaphoreTake(s_scr_mtx, portMAX_DELAY);
    s_asleep = false;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, s_bright * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (s_scr_mtx) xSemaphoreGive(s_scr_mtx);
}

bool screen_is_asleep(void)
{
    return s_asleep;
}

void screen_flush_hook(const lv_area_t *a, const uint8_t *px)
{
    if (!s_shadow || !s_fb_mtx || !a || !px) return;
    int w = a->x2 - a->x1 + 1;
    int h = a->y2 - a->y1 + 1;
    if (w <= 0 || h <= 0) return;
    xSemaphoreTake(s_fb_mtx, portMAX_DELAY);
    for (int y = 0; y < h; y++) {
        int sy = a->y1 + y;
        if (sy < 0 || sy >= SCREEN_H) continue;
        int sx = a->x1;
        int n = w;
        if (sx < 0) { n += sx; sx = 0; }
        if (sx + n > SCREEN_W) n = SCREEN_W - sx;
        if (n <= 0) continue;
        memcpy(s_shadow + ((size_t)sy * SCREEN_W + sx) * 2,
               px + ((size_t)y * w + (sx - a->x1)) * 2, (size_t)n * 2);
    }
    xSemaphoreGive(s_fb_mtx);
}

bool screen_snapshot_begin(void)
{
    if (!s_shadow || !s_snap || !s_fb_mtx || !s_snap_mtx) return false;
    if (xSemaphoreTake(s_snap_mtx, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    xSemaphoreTake(s_fb_mtx, portMAX_DELAY);
    memcpy(s_snap, s_shadow, (size_t)SCREEN_W * SCREEN_H * 2);
    xSemaphoreGive(s_fb_mtx);
    return true;
}

const uint16_t *screen_snapshot_data(void)
{
    return (const uint16_t *)s_snap;
}

void screen_snapshot_end(void)
{
    if (s_snap_mtx) xSemaphoreGive(s_snap_mtx);
}
