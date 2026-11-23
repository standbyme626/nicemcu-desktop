/* NiceMCU-S3-2.8IPS 中文桌面
 * LCD ST7789 240x320 SPI: SCK=12 MOSI=11 CS=10 DC=9 BL=45
 * TP  CST816T (T6336G) I2C 0x15: SDA=17 SCL=18 INT=8
 * WiFi STA + SNTP + open-meteo 天气；左右滑动两页
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "lvgl.h"
#include "include/lvgl/draw/lv_draw_utils.h"
#include "include/lvgl/themes/lv_theme_default.h"

#define W 320
#define H 240
#define PIN_SCK 12
#define PIN_MOSI 11
#define PIN_CS 10
#define PIN_DC 9
#define PIN_BL 45
#define PIN_SDA 17
#define PIN_SCL 18

#define C_BG     0x0B1220
#define C_CARD   0x141C2B
#define C_TEXT   0xE6EDF3
#define C_SUB    0x8B98A9
#define C_ACCENT 0x4CC9F0
#define C_WARM   0xFFB703

LV_FONT_DECLARE(font_zh16);
LV_FONT_DECLARE(font_zh24);
LV_FONT_DECLARE(font_zh48);
LV_FONT_DECLARE(font_zhfull);

static const char *TAG = "desktop";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_tp;

static volatile bool s_wifi_ok;
static char s_ip_str[16] = "-";
static int s_rssi = 0;
static volatile bool s_wx_req;
static volatile bool s_sntp_started;

typedef struct {
    volatile uint32_t seq;
    bool valid;
    float temp, wind;
    int rh, code;
} wx_t;
static wx_t s_wx;

static lv_obj_t *s_clock, *s_date, *s_temp, *s_cond, *s_hum, *s_wind;
static lv_obj_t *s_wifi, *s_ip, *s_mem;
static lv_obj_t *s_set_panel, *s_set_val;
static int s_bright_pct = 60;
static volatile bool s_sleep;

typedef enum { POMO_IDLE, POMO_WORK, POMO_BREAK, POMO_PAUSE } pomo_t;
static pomo_t s_pomo = POMO_IDLE, s_pomo_resume = POMO_WORK;
static int s_pomo_left = 25 * 60;
static lv_obj_t *s_pomo_time, *s_pomo_state_lbl;

/* ---------------- LCD / touch ---------------- */
static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static esp_err_t tp_read(uint8_t reg, uint8_t *buf, size_t n);
static void ledc_set_bright(int pct);

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    size_t n = (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1);
    lv_draw_rgb565_swap(px, n);
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
}

static void indev_cb(lv_indev_t *indev, lv_indev_data_t *d)
{
    static lv_coord_t lx, ly;
    uint8_t b[6];
    bool pressed = s_tp && tp_read(0x02, b, 6) == ESP_OK && (b[0] & 0x0F) > 0;
    if (s_sleep) {
        if (pressed) {
            ledc_set_bright(s_bright_pct);
            s_sleep = false;
        }
        d->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (pressed) {
        int tx = ((b[1] & 0x0F) << 8) | b[2];
        int ty = ((b[3] & 0x0F) << 8) | b[4];
        int nx = ty;
        int ny = 239 - tx;
        lx = nx < W ? nx : W - 1;
        ly = ny < H ? ny : H - 1;
        d->point.x = lx;
        d->point.y = ly;
        d->state = LV_INDEV_STATE_PRESSED;
    } else {
        d->point.x = lx;
        d->point.y = ly;
        d->state = LV_INDEV_STATE_RELEASED;
    }
}

static void ledc_set_bright(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    s_bright_pct = pct;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void lcd_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = W * 2 * 40,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_CS, .dc_gpio_num = PIN_DC, .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000, .trans_queue_depth = 10,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &s_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t ch = {
        .gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    ledc_set_bright(60);
}

static esp_err_t tp_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_tp, &reg, 1, buf, n, 100);
}

static void touch_init(void)
{
    i2c_master_bus_config_t buscfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&buscfg, &s_bus));
    if (i2c_master_probe(s_bus, 0x15, 20) != ESP_OK) {
        ESP_LOGE(TAG, "no touch at 0x15");
        return;
    }
    i2c_device_config_t devcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x15, .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &devcfg, &s_tp));
    uint8_t id = 0;
    if (tp_read(0xA7, &id, 1) == ESP_OK) ESP_LOGI(TAG, "touch chip id 0x%02x", id);
}

/* ---------------- WiFi / SNTP ---------------- */
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ok = false;
        strcpy(s_ip_str, "-");
        s_rssi = 0;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip_str, sizeof s_ip_str, IPSTR, IP2STR(&ev->ip_info.ip));
        s_wifi_ok = true;
        ESP_LOGI(TAG, "got ip %s", s_ip_str);
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, CONFIG_DESKTOP_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_DESKTOP_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
}

static void sntp_start(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_netif_sntp_init(&cfg);
}

/* ---------------- weather ---------------- */
typedef struct { char *buf; int len; int used; } http_buf_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    http_buf_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b && b->used < b->len - 1) {
        int n = e->data_len;
        if (b->used + n > b->len - 1) n = b->len - 1 - b->used;
        memcpy(b->buf + b->used, e->data, n);
        b->used += n;
        b->buf[b->used] = 0;
    }
    return ESP_OK;
}

static int http_fetch(const char *url, char *out, int len)
{
    http_buf_t b = {out, len, 0};
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 12000, .user_data = &b, .event_handler = http_evt,
    };
    if (strncmp(url, "https", 5) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGW(TAG, "http init failed");
        return -1;
    }
    int ret = -1;
    esp_err_t err = esp_http_client_perform(c);
    int code = esp_http_client_get_status_code(c);
    if (err == ESP_OK && code == 200) ret = b.used;
    else ESP_LOGW(TAG, "http %s err=%s code=%d", strncmp(url, "https", 5) ? "ip-api" : "open-meteo", esp_err_to_name(err), code);
    esp_http_client_cleanup(c);
    return ret;
}

static const char *json_val(const char *buf, const char *key)
{
    static char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(buf, pat);
    return p ? p + strlen(pat) : NULL;
}

static double json_num(const char *buf, const char *key)
{
    const char *p = json_val(buf, key);
    return p ? strtod(p, NULL) : NAN;
}

static const char *wx_text(int code)
{
    if (code == 0) return "晴";
    if (code <= 2) return "多云";
    if (code == 3) return "阴";
    if (code == 45 || code == 48) return "雾";
    if (code >= 51 && code <= 57) return "小雨";
    if (code == 61) return "小雨";
    if (code == 63) return "中雨";
    if (code == 65) return "大雨";
    if (code >= 66 && code <= 67) return "雨";
    if (code >= 71 && code <= 73) return "小雪";
    if (code == 75) return "中雪";
    if (code == 77) return "大雪";
    if (code >= 80 && code <= 82) return "阵雨";
    if (code == 85 || code == 86) return "阵雪";
    if (code >= 95) return "雷阵雨";
    return "未知";
}

static bool weather_fetch(void)
{
    ESP_LOGI(TAG, "weather fetch, heap=%u", (unsigned)esp_get_free_heap_size());
    const double lat = atof(CONFIG_DESKTOP_LAT);
    const double lon = atof(CONFIG_DESKTOP_LON);
    int n;
    static char buf[4096], url[320];
    ESP_LOGI(TAG, "weather coords lat=%.4f lon=%.4f", lat, lon);
    snprintf(url, sizeof url,
             "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
             "&timezone=Asia%%2FShanghai", lat, lon);
    n = http_fetch(url, buf, sizeof buf);
    if (n <= 0) {
        ESP_LOGW(TAG, "open-meteo failed");
        return false;
    }
    const char *cur = strstr(buf, "\"current\":");
    if (!cur) return false;

    double t = json_num(cur, "temperature_2m");
    double rh = json_num(cur, "relative_humidity_2m");
    double code = json_num(cur, "weather_code");
    double wind = json_num(cur, "wind_speed_10m");
    if (isnan(t) || isnan(code)) return false;

    s_wx.seq++;
    s_wx.temp = t;
    s_wx.rh = (int)rh;
    s_wx.code = (int)code;
    s_wx.wind = isnan(wind) ? 0 : wind;
    s_wx.valid = true;
    s_wx.seq++;
    ESP_LOGI(TAG, "weather %.1fC rh=%d code=%d wind=%.1f", t, s_wx.rh, s_wx.code, s_wx.wind);
    return true;
}

static void weather_task(void *arg)
{
    for (;;) {
        if (s_wifi_ok && s_sntp_started) {
            if (weather_fetch()) {
                for (int i = 0; i < 900 && !s_wx_req; i++) vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                for (int i = 0; i < 30 && !s_wx_req; i++) vTaskDelay(pdMS_TO_TICKS(1000));
            }
            s_wx_req = false;
        } else {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}


/* ---------------- media-web thin client ---------------- */
#define MEDIA_HOST CONFIG_DESKTOP_MEDIA_HOST
#define MEDIA_PORT 8097
#define COVER_W 80
#define COVER_H 80
#define MEDIA_QUEUE_MAX 60

typedef struct {
    bool ok, pc_online, muted, playing;
    char title[192], app[48], bv[16];
    int pos, dur, vol, qn, qidx;
    uint32_t ver;
} media_state_t;

static media_state_t s_media;
static SemaphoreHandle_t s_media_mtx, s_cover_mtx;
static uint8_t s_cover_front[COVER_W * COVER_H * 2];
static uint8_t s_cover_back[COVER_W * COVER_H * 2];
static volatile uint32_t s_cover_ver;
static volatile bool s_cover_dirty;

typedef struct { char bv[16]; char title[128]; } media_qitem_t;
static media_qitem_t s_queue[MEDIA_QUEUE_MAX];
static volatile int s_queue_n;
static volatile uint32_t s_queue_ver;
static volatile bool s_queue_req;

typedef struct { int type, arg; } media_cmd_t;
enum { MC_PLAY, MC_PAUSE, MC_NEXT, MC_PREV, MC_VOL, MC_MUTE, MC_SEEK, MC_SLEEP, MC_JUMP };
static QueueHandle_t s_media_q;
static char s_media_auth[192];
static volatile bool s_media_active;
static volatile bool s_poll_now;

static void media_build_auth(void)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char in[128];
    int n = snprintf(in, sizeof in, "%s:%s", CONFIG_DESKTOP_MEDIA_USER, CONFIG_DESKTOP_MEDIA_PASSWORD);
    int o = 0, i = 0;
    memcpy(s_media_auth, "Basic ", 6);
    o = 6;
    for (; i + 2 < n; i += 3) {
        s_media_auth[o++] = t[(in[i] >> 2) & 0x3F];
        s_media_auth[o++] = t[((in[i] & 3) << 4) | ((in[i + 1] >> 4) & 0xF)];
        s_media_auth[o++] = t[((in[i + 1] & 0xF) << 2) | ((in[i + 2] >> 6) & 3)];
        s_media_auth[o++] = t[in[i + 2] & 0x3F];
    }
    if (i < n) {
        s_media_auth[o++] = t[(in[i] >> 2) & 0x3F];
        if (i + 1 < n) {
            s_media_auth[o++] = t[((in[i] & 3) << 4) | ((in[i + 1] >> 4) & 0xF)];
            s_media_auth[o++] = t[(in[i + 1] & 0xF) << 2];
            s_media_auth[o++] = '=';
        } else {
            s_media_auth[o++] = t[(in[i] & 3) << 4];
            s_media_auth[o++] = '=';
            s_media_auth[o++] = '=';
        }
    }
    s_media_auth[o] = 0;
}

typedef struct { char *out; int outlen; uint8_t *bin; int binlen; int used; } media_http_buf_t;

static esp_err_t media_http_evt(esp_http_client_event_t *e)
{
    media_http_buf_t *b = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA || !b || e->data_len <= 0) return ESP_OK;
    int n = e->data_len;
    if (b->bin) {
        if (b->used + n > b->binlen) n = b->binlen - b->used;
        if (n > 0) { memcpy(b->bin + b->used, e->data, n); b->used += n; }
    } else if (b->out) {
        if (b->used + n > b->outlen - 1) n = b->outlen - 1 - b->used;
        if (n > 0) { memcpy(b->out + b->used, e->data, n); b->used += n; b->out[b->used] = 0; }
    }
    return ESP_OK;
}

static int media_req(const char *path, const char *body, char *out, int outlen, uint8_t *bin, int binlen)
{
    char url[192];
    snprintf(url, sizeof url, "http://" MEDIA_HOST ":%d%s", MEDIA_PORT, path);
    media_http_buf_t b = {.out = out, .outlen = outlen, .bin = bin, .binlen = binlen, .used = 0};
    if (out) out[0] = 0;
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 5000, .user_data = &b, .event_handler = media_http_evt,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "Authorization", s_media_auth);
    if (body) {
        esp_http_client_set_method(c, HTTP_METHOD_POST);
        esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(c, body, strlen(body));
    }
    esp_err_t err = esp_http_client_perform(c);
    int code = esp_http_client_get_status_code(c);
    int used = b.used;
    esp_http_client_cleanup(c);
    return (err == ESP_OK && code == 200) ? used : -1;
}

static const char *jskip(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static const char *jfield(const char *js, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return NULL;
    p = jskip(p + strlen(pat));
    if (*p == ':') p++;
    return jskip(p);
}

static unsigned jhex4(const char *p, int *adv)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    }
    *adv = 4;
    return v;
}

static bool jstr(const char *js, const char *key, char *out, int outlen)
{
    const char *p = jfield(js, key);
    if (!p || *p != '"') return false;
    p++;
    int i = 0;
    while (*p && *p != '"') {
        if (*p != '\\') {
            if (i >= outlen - 1) break;
            out[i++] = *p++;
            continue;
        }
        unsigned cp;
        char c = p[1];
        if (c == 'u') {
            int adv = 0;
            cp = jhex4(p + 2, &adv);
            p += 6;
            if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                unsigned lo = jhex4(p + 2, &adv);
                p += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
        } else {
            cp = (c == 'n') ? '\n' : (c == 't') ? '\t' : (c == 'r') ? '\r' : (unsigned char)c;
            p += 2;
        }
        int need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (i + need > outlen - 1) break;
        if (cp < 0x80) out[i++] = (char)cp;
        else if (cp < 0x800) {
            out[i++] = (char)(0xC0 | (cp >> 6));
            out[i++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[i++] = (char)(0xE0 | (cp >> 12));
            out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[i++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[i++] = (char)(0xF0 | (cp >> 18));
            out[i++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[i++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[i] = 0;
    return true;
}

static int jint(const char *js, const char *key)
{
    const char *p = jfield(js, key);
    return p ? atoi(p) : 0;
}

static bool jbool(const char *js, const char *key)
{
    const char *p = jfield(js, key);
    return p && !strncmp(p, "true", 4);
}

static void media_parse_status(const char *js)
{
    media_state_t st;
    memset(&st, 0, sizeof st);
    st.ok = jbool(js, "ok");
    jstr(js, "title", st.title, sizeof st.title);
    jstr(js, "app", st.app, sizeof st.app);
    jstr(js, "bv", st.bv, sizeof st.bv);
    char stt[16] = "";
    jstr(js, "status", stt, sizeof stt);
    st.playing = !strcmp(stt, "playing");
    st.pos = jint(js, "pos");
    st.dur = jint(js, "dur");
    st.vol = jint(js, "volume");
    st.muted = jbool(js, "muted");
    st.pc_online = jbool(js, "pc_online");
    const char *q = strstr(js, "\"queue\"");
    if (q) { st.qn = jint(q, "n"); st.qidx = jint(q, "idx"); }
    if (xSemaphoreTake(s_media_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        char old[16];
        strncpy(old, s_media.bv, sizeof old - 1);
        old[15] = 0;
        st.ver = s_media.ver + 1;
        s_media = st;
        xSemaphoreGive(s_media_mtx);
        if (strcmp(old, st.bv) != 0) s_cover_dirty = true;
    }
}

static int media_parse_queue(const char *js)
{
    const char *p = strstr(js, "\"queue\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    int n = 0;
    while (n < MEDIA_QUEUE_MAX) {
        const char *bp = strstr(p, "\"bv\"");
        if (!bp) break;
        char bv[16] = "", title[128] = "";
        if (!jstr(bp, "bv", bv, sizeof bv)) break;
        const char *tp = strstr(bp, "\"title\"");
        if (tp) jstr(tp, "title", title, sizeof title);
        snprintf(s_queue[n].bv, sizeof s_queue[n].bv, "%s", bv);
        snprintf(s_queue[n].title, sizeof s_queue[n].title, "%s", title);
        n++;
        p = tp ? tp + 7 : bp + 4;
    }
    return n;
}

static void media_fetch_cover(const char *bv)
{
    char path[64];
    snprintf(path, sizeof path, "/api/cover?bv=%s", bv);
    int n = media_req(path, NULL, NULL, 0, s_cover_back, sizeof s_cover_back);
    if (n == (int)sizeof s_cover_back) {
        if (xSemaphoreTake(s_cover_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
            s_cover_ver++;
            xSemaphoreGive(s_cover_mtx);
        }
    }
}

static void media_send(int type, int arg)
{
    media_cmd_t c = {.type = type, .arg = arg};
    if (s_media_q) xQueueSend(s_media_q, &c, pdMS_TO_TICKS(50));
}

static void media_task(void *arg)
{
    static char js[1536];
    static char qjs[16384];
    int64_t last_poll = 0;
    for (;;) {
        media_cmd_t c;
        while (xQueueReceive(s_media_q, &c, 0) == pdTRUE) {
            char body[64], path[64];
            switch (c.type) {
            case MC_PLAY: media_req("/api/play", "", NULL, 0, NULL, 0); break;
            case MC_PAUSE: media_req("/api/pause", "", NULL, 0, NULL, 0); break;
            case MC_NEXT: media_req("/api/next", "", NULL, 0, NULL, 0); break;
            case MC_PREV: media_req("/api/prev", "", NULL, 0, NULL, 0); break;
            case MC_VOL: snprintf(body, sizeof body, "level=%d", c.arg); media_req("/api/volume", body, NULL, 0, NULL, 0); break;
            case MC_MUTE: media_req("/api/mute", "", NULL, 0, NULL, 0); break;
            case MC_SEEK: snprintf(body, sizeof body, "pos=%d", c.arg); media_req("/api/seek", body, NULL, 0, NULL, 0); break;
            case MC_SLEEP: snprintf(body, sizeof body, "min=%d", c.arg); media_req("/api/sleep", body, NULL, 0, NULL, 0); break;
            case MC_JUMP: snprintf(path, sizeof path, "/api/queue/jump?i=%d", c.arg); media_req(path, NULL, NULL, 0, NULL, 0); break;
            }
        }
        int64_t now = esp_timer_get_time() / 1000;
        if (s_poll_now) {
            s_poll_now = false;
            last_poll = 0;
        }
        if (s_media_active && now - last_poll >= 2000) {
            last_poll = now;
            if (media_req("/api/status", NULL, js, sizeof js, NULL, 0) > 0) {
                media_parse_status(js);
                char bv[16] = "";
                if (xSemaphoreTake(s_media_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
                    strncpy(bv, s_media.bv, sizeof bv - 1);
                    xSemaphoreGive(s_media_mtx);
                }
                if (bv[0] && s_cover_dirty) {
                    s_cover_dirty = false;
                    media_fetch_cover(bv);
                }
            }
        }
        if (s_queue_req) {
            s_queue_req = false;
            if (media_req("/api/queue/status", NULL, qjs, sizeof qjs, NULL, 0) > 0) {
                s_queue_n = media_parse_queue(qjs);
                s_queue_ver++;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ---------------- UI ---------------- */
static void set_text(lv_obj_t *l, const char *s)
{
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, s)) lv_label_set_text(l, s);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *mk_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, int x, int y, int w, int h, const char *text,
                        lv_event_cb_t cb, uint32_t bg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mk_label(b, &font_zh16, bg == C_CARD ? C_TEXT : 0x06121E, text));
    return b;
}


/* ---- media page ---- */
static lv_obj_t *s_media_page, *s_queue_page, *s_queue_list, *s_queue_title;
static lv_obj_t *s_med_title, *s_med_sub, *s_med_state, *s_med_pos, *s_med_dur;
static lv_obj_t *s_med_prog, *s_med_vol, *s_med_vol_lbl, *s_med_play, *s_med_mute, *s_med_timer;
static lv_obj_t *s_med_cover;
static bool s_seek_drag, s_vol_drag;
static int s_sleep_min;
static uint32_t s_cover_seen, s_queue_seen;

static void label_bg(lv_obj_t *l)
{
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(l, lv_color_hex(C_BG), 0);
}

static void media_state_copy(media_state_t *out)
{
    memset(out, 0, sizeof *out);
    if (xSemaphoreTake(s_media_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_media;
        xSemaphoreGive(s_media_mtx);
    }
}

static void fmt_mmss(int sec, char *b, int n)
{
    if (sec < 0) sec = 0;
    if (sec >= 3600) snprintf(b, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    else snprintf(b, n, "%02d:%02d", sec / 60, sec % 60);
}

static void med_close_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_media_page, LV_OBJ_FLAG_HIDDEN);
    if (s_queue_page) lv_obj_add_flag(s_queue_page, LV_OBJ_FLAG_HIDDEN);
    s_media_active = false;
}
static void med_open_cb(lv_event_t *e)
{
    lv_obj_remove_flag(s_media_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_media_page);
    s_media_active = true;
    s_poll_now = true;
}
static void med_queue_close_cb(lv_event_t *e) { lv_obj_add_flag(s_queue_page, LV_OBJ_FLAG_HIDDEN); }

static void med_play_cb(lv_event_t *e)
{
    media_state_t st;
    media_state_copy(&st);
    media_send(st.playing ? MC_PAUSE : MC_PLAY, 0);
}
static void med_prev_cb(lv_event_t *e) { media_send(MC_PREV, 0); }
static void med_next_cb(lv_event_t *e) { media_send(MC_NEXT, 0); }
static void med_mute_cb(lv_event_t *e) { media_send(MC_MUTE, 0); }

static void med_seek_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_seek_drag = true;
    } else if (code == LV_EVENT_RELEASED) {
        s_seek_drag = false;
        media_send(MC_SEEK, lv_slider_get_value(lv_event_get_target(e)));
    }
}

static void med_vol_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int v = lv_slider_get_value(lv_event_get_target(e));
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    set_text(s_med_vol_lbl, b);
    if (code == LV_EVENT_PRESSED) s_vol_drag = true;
    else if (code == LV_EVENT_RELEASED) {
        s_vol_drag = false;
        media_send(MC_VOL, v);
    }
}

static void med_timer_cb(lv_event_t *e)
{
    static const int mins[] = {0, 30, 60, 90};
    static int idx;
    idx = (idx + 1) % 4;
    s_sleep_min = mins[idx];
    media_send(MC_SLEEP, s_sleep_min);
    char b[24];
    if (s_sleep_min) snprintf(b, sizeof b, "定时%d", s_sleep_min);
    else snprintf(b, sizeof b, "定时");
    set_text(s_med_timer, b);
}

static void med_queue_cb(lv_event_t *e)
{
    lv_obj_remove_flag(s_queue_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_queue_page);
    s_media_active = true;
    s_poll_now = true;
    s_queue_seen = 0;
    s_queue_req = true;
    if (s_queue_list) lv_obj_clean(s_queue_list);
}

static void queue_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    media_send(MC_JUMP, i);
    lv_obj_add_flag(s_queue_page, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *mk_icon_btn(lv_obj_t *p, int x, int y, int w, int h, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mk_label(b, &lv_font_montserrat_32, C_TEXT, sym));
    return b;
}

static void sanitize_for_font(char *s, const lv_font_t *font)
{
    char *r = s, *w = s;
    while (*r) {
        unsigned char c = (unsigned char)*r;
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        unsigned cp = 0;
        if (len == 1) cp = c;
        else if (len == 2) cp = ((c & 0x1F) << 6) | (r[1] & 0x3F);
        else if (len == 3) cp = ((c & 0x0F) << 12) | ((r[1] & 0x3F) << 6) | (r[2] & 0x3F);
        else cp = ((c & 0x07) << 18) | ((r[1] & 0x3F) << 12) | ((r[2] & 0x3F) << 6) | (r[3] & 0x3F);
        lv_font_glyph_dsc_t gd;
        if (lv_font_get_glyph_dsc(font, &gd, cp, 0)) {
            memmove(w, r, len);
            w += len;
        } else {
            *w++ = '?';
        }
        r += len;
    }
    *w = 0;
}

static void wrap_title(const char *src, char *out, int outlen, int max_px, int max_lines)
{
    int line_px = 0, lines = 1, oi = 0;
    const char *p = src;
    while (*p && oi < outlen - 8) {
        unsigned char c = (unsigned char)*p;
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        unsigned cp = 0;
        if (len == 1) cp = c;
        else if (len == 2) cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        else if (len == 3) cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        else cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        int w = cp >= 0x2E80 ? 16 : 8;
        if (line_px + w > max_px) {
            if (lines >= max_lines) {
                out[oi++] = '.'; out[oi++] = '.'; out[oi++] = '.';
                break;
            }
            out[oi++] = '\n';
            lines++;
            line_px = 0;
        }
        memcpy(out + oi, p, len);
        oi += len;
        line_px += w;
        p += len;
    }
    out[oi] = 0;
}

static void media_queue_build(void)
{
    if (!s_queue_list) return;
    lv_obj_clean(s_queue_list);
    media_state_t st;
    media_state_copy(&st);
    for (int i = 0; i < s_queue_n; i++) {
        lv_obj_t *row = lv_button_create(s_queue_list);
        lv_obj_set_size(row, 296, 42);
        lv_obj_set_pos(row, 0, i * 46);
        lv_obj_set_style_bg_color(row, lv_color_hex(i == st.qidx ? 0x1E2A3D : C_CARD), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, queue_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char b[240], t[128], w[400];
        snprintf(t, sizeof t, "%s", s_queue[i].title);
        sanitize_for_font(t, &font_zhfull);
        snprintf(b, sizeof b, "%d. %s", i + 1, t);
        wrap_title(b, w, sizeof w, 276, 2);
        lv_obj_t *l = mk_label(row, &font_zhfull, i == st.qidx ? C_ACCENT : C_TEXT, w);
        lv_obj_set_size(l, 280, 40);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
    }
    if (s_queue_title) {
        char tb[32];
        snprintf(tb, sizeof tb, "队列 %d/%d", st.qidx + 1, s_queue_n);
        set_text(s_queue_title, tb);
    }
    lv_obj_scroll_to_y(s_queue_list, 0, LV_ANIM_OFF);
    lv_obj_invalidate(s_queue_list);
}

static void media_ui_build(lv_obj_t *scr)
{
    for (int i = 0; i < COVER_W * COVER_H; i++) {
        uint16_t v = 0x0861;
        s_cover_front[i * 2] = v & 0xFF;
        s_cover_front[i * 2 + 1] = v >> 8;
    }

    s_media_page = lv_obj_create(scr);
    lv_obj_set_size(s_media_page, W, H);
    lv_obj_set_pos(s_media_page, 0, 0);
    lv_obj_set_style_bg_color(s_media_page, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_media_page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_media_page, 0, 0);
    lv_obj_set_style_border_width(s_media_page, 0, 0);
    lv_obj_remove_flag(s_media_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cl = mk_label(s_media_page, &font_zh16, C_SUB, LV_SYMBOL_CLOSE);
    lv_obj_align(cl, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_add_flag(cl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(cl, 8);
    lv_obj_add_event_cb(cl, med_close_cb, LV_EVENT_CLICKED, NULL);

    s_med_cover = lv_canvas_create(s_media_page);
    lv_canvas_set_buffer(s_med_cover, s_cover_front, COVER_W, COVER_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_med_cover, 8, 10);

    s_med_title = mk_label(s_media_page, &font_zhfull, C_TEXT, "加载中...");
    lv_obj_set_pos(s_med_title, 96, 4);
    lv_obj_set_size(s_med_title, 196, 84);
    lv_label_set_long_mode(s_med_title, LV_LABEL_LONG_CLIP);
    label_bg(s_med_title);
    s_med_sub = NULL;
    s_med_state = mk_label(s_media_page, &font_zh16, C_ACCENT, "-");
    lv_obj_set_pos(s_med_state, 8, 92);
    lv_obj_set_size(s_med_state, 140, 20);
    lv_label_set_long_mode(s_med_state, LV_LABEL_LONG_DOT);
    label_bg(s_med_state);

    s_med_prog = lv_slider_create(s_media_page);
    lv_obj_set_pos(s_med_prog, 8, 118);
    lv_obj_set_size(s_med_prog, 304, 10);
    lv_slider_set_range(s_med_prog, 0, 1);
    lv_obj_set_style_bg_color(s_med_prog, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_med_prog, lv_color_hex(C_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(s_med_prog, med_seek_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_med_prog, med_seek_cb, LV_EVENT_RELEASED, NULL);

    s_med_pos = mk_label(s_media_page, &font_zh16, C_SUB, "00:00");
    lv_obj_set_pos(s_med_pos, 8, 130);
    label_bg(s_med_pos);
    s_med_dur = mk_label(s_media_page, &font_zh16, C_SUB, "00:00");
    lv_obj_align(s_med_dur, LV_ALIGN_TOP_RIGHT, -8, 130);
    label_bg(s_med_dur);

    mk_icon_btn(s_media_page, 8, 152, 52, 44, LV_SYMBOL_PREV, med_prev_cb);
    lv_obj_t *bp = mk_icon_btn(s_media_page, 64, 152, 72, 44, LV_SYMBOL_PLAY, med_play_cb);
    s_med_play = lv_obj_get_child(bp, 0);
    mk_icon_btn(s_media_page, 140, 152, 52, 44, LV_SYMBOL_NEXT, med_next_cb);
    lv_obj_t *bm = mk_icon_btn(s_media_page, 196, 152, 52, 44, LV_SYMBOL_VOLUME_MAX, med_mute_cb);
    s_med_mute = lv_obj_get_child(bm, 0);

    lv_obj_t *bt = lv_button_create(s_media_page);
    lv_obj_set_pos(bt, 252, 152);
    lv_obj_set_size(bt, 60, 44);
    lv_obj_set_style_bg_color(bt, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(bt, 10, 0);
    lv_obj_set_style_shadow_width(bt, 0, 0);
    lv_obj_add_event_cb(bt, med_timer_cb, LV_EVENT_CLICKED, NULL);
    s_med_timer = mk_label(bt, &font_zh16, C_TEXT, "定时");
    lv_obj_center(s_med_timer);

    lv_obj_t *vl = mk_label(s_media_page, &font_zh16, C_SUB, "音量");
    lv_obj_set_pos(vl, 8, 198);
    s_med_vol = lv_slider_create(s_media_page);
    lv_obj_set_pos(s_med_vol, 56, 200);
    lv_obj_set_size(s_med_vol, 200, 12);
    lv_slider_set_range(s_med_vol, 0, 100);
    lv_obj_set_style_bg_color(s_med_vol, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_med_vol, lv_color_hex(C_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(s_med_vol, med_vol_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_med_vol, med_vol_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_med_vol, med_vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_med_vol_lbl = mk_label(s_media_page, &font_zh16, C_SUB, "0");
    lv_obj_set_pos(s_med_vol_lbl, 264, 198);
    label_bg(s_med_vol_lbl);

    lv_obj_t *bq = lv_button_create(s_media_page);
    lv_obj_set_pos(bq, 196, 90);
    lv_obj_set_size(bq, 116, 24);
    lv_obj_set_style_bg_color(bq, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(bq, 8, 0);
    lv_obj_set_style_shadow_width(bq, 0, 0);
    lv_obj_add_event_cb(bq, med_queue_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mk_label(bq, &font_zh16, C_TEXT, "队列"));

    lv_obj_add_flag(s_media_page, LV_OBJ_FLAG_HIDDEN);

    s_queue_page = lv_obj_create(scr);
    lv_obj_set_size(s_queue_page, W, H);
    lv_obj_set_pos(s_queue_page, 0, 0);
    lv_obj_set_style_bg_color(s_queue_page, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_queue_page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_queue_page, 0, 0);
    lv_obj_set_style_border_width(s_queue_page, 0, 0);
    lv_obj_remove_flag(s_queue_page, LV_OBJ_FLAG_SCROLLABLE);
    s_queue_title = mk_label(s_queue_page, &font_zh16, C_ACCENT, "队列");
    lv_obj_t *qc = mk_label(s_queue_page, &font_zh16, C_SUB, LV_SYMBOL_CLOSE);
    lv_obj_align(qc, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_add_flag(qc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(qc, 8);
    lv_obj_add_event_cb(qc, med_queue_close_cb, LV_EVENT_CLICKED, NULL);

    s_queue_list = lv_obj_create(s_queue_page);
    lv_obj_set_pos(s_queue_list, 8, 28);
    lv_obj_set_size(s_queue_list, 304, 204);
    lv_obj_set_style_bg_opa(s_queue_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_queue_list, 0, 0);
    lv_obj_set_style_pad_all(s_queue_list, 0, 0);
    lv_obj_set_scroll_dir(s_queue_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_queue_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_queue_page, LV_OBJ_FLAG_HIDDEN);
}

static void media_ui_refresh(void)
{
    if (s_cover_ver != s_cover_seen) {
        if (xSemaphoreTake(s_cover_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
            memcpy(s_cover_front, s_cover_back, sizeof s_cover_front);
            s_cover_seen = s_cover_ver;
            xSemaphoreGive(s_cover_mtx);
        }
        if (s_med_cover) lv_obj_invalidate(s_med_cover);
    }
    if (!s_media_page || lv_obj_has_flag(s_media_page, LV_OBJ_FLAG_HIDDEN)) return;

    media_state_t st;
    media_state_copy(&st);
    sanitize_for_font(st.title, &font_zhfull);
    char b[96], wrapped[420];
    if (st.title[0]) {
        wrap_title(st.title, wrapped, sizeof wrapped, 190, 4);
        set_text(s_med_title, wrapped);
    } else {
        set_text(s_med_title, "无播放");
    }
    set_text(s_med_state, st.pc_online ? (st.playing ? "播放中" : "已暂停") : "电脑离线");
    fmt_mmss(st.pos, b, sizeof b);
    set_text(s_med_pos, b);
    fmt_mmss(st.dur, b, sizeof b);
    set_text(s_med_dur, b);
    if (!s_seek_drag && st.dur > 0) {
        if (lv_slider_get_max_value(s_med_prog) != st.dur) lv_slider_set_range(s_med_prog, 0, st.dur);
        if (lv_slider_get_value(s_med_prog) != st.pos) lv_slider_set_value(s_med_prog, st.pos, LV_ANIM_OFF);
    }
    if (!s_vol_drag && lv_slider_get_value(s_med_vol) != st.vol) {
        lv_slider_set_value(s_med_vol, st.vol, LV_ANIM_OFF);
        snprintf(b, sizeof b, "%d", st.vol);
        set_text(s_med_vol_lbl, b);
    }
    set_text(s_med_play, st.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    set_text(s_med_mute, st.muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);

    if (s_queue_page && !lv_obj_has_flag(s_queue_page, LV_OBJ_FLAG_HIDDEN) && s_queue_ver != s_queue_seen) {
        s_queue_seen = s_queue_ver;
        media_queue_build();
    }
}

/* ---- pomodoro ---- */
static const char *pomo_text(pomo_t st)
{
    switch (st) {
    case POMO_WORK: return "工作";
    case POMO_BREAK: return "休息";
    case POMO_PAUSE: return "暂停";
    default: return "待命";
    }
}

static void pomo_refresh(void)
{
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d", s_pomo_left / 60, s_pomo_left % 60);
    set_text(s_pomo_time, b);
    set_text(s_pomo_state_lbl, pomo_text(s_pomo));
}

static void pomo_toggle(void)
{
    if (s_pomo == POMO_WORK || s_pomo == POMO_BREAK) {
        s_pomo_resume = s_pomo;
        s_pomo = POMO_PAUSE;
    } else if (s_pomo == POMO_PAUSE) {
        s_pomo = s_pomo_resume;
    } else {
        s_pomo = POMO_WORK;
        s_pomo_left = 25 * 60;
    }
    pomo_refresh();
}

static void pomo_reset(void)
{
    s_pomo = POMO_IDLE;
    s_pomo_left = 25 * 60;
    pomo_refresh();
}

static void pomo_card_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) pomo_reset();
    else pomo_toggle();
}

/* ---- sleep / dialog ---- */
static void go_sleep(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    s_sleep = true;
}

static void show_msg(const char *title, const char *text)
{
    lv_obj_t *mb = lv_msgbox_create(NULL);
    lv_obj_set_width(mb, 220);
    lv_obj_set_style_text_font(mb, &font_zh16, 0);
    lv_obj_t *t = lv_msgbox_add_title(mb, title);
    lv_obj_set_style_text_font(t, &font_zh24, 0);
    lv_obj_t *b = lv_msgbox_add_text(mb, text);
    lv_obj_set_style_text_font(b, &font_zh16, 0);
    lv_msgbox_add_close_button(mb);
    lv_obj_center(mb);
}

/* ---- settings overlay ---- */
static void set_close_cb(lv_event_t *e) { lv_obj_add_flag(s_set_panel, LV_OBJ_FLAG_HIDDEN); }
static void set_refresh_cb(lv_event_t *e) { s_wx_req = true; lv_obj_add_flag(s_set_panel, LV_OBJ_FLAG_HIDDEN); }
static void set_sleep_cb(lv_event_t *e) { go_sleep(); lv_obj_add_flag(s_set_panel, LV_OBJ_FLAG_HIDDEN); }
static void set_about_cb(lv_event_t *e)
{
    show_msg("关于", "芯片 ESP32-S3 N16R8\n触摸 CST816T\n固件 v0.2");
}

static void set_slider_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    ledc_set_bright(v);
    char b[16];
    snprintf(b, sizeof b, "%d%%", v);
    set_text(s_set_val, b);
}

static void settings_build(void)
{
    s_set_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_set_panel, 280, 196);
    lv_obj_center(s_set_panel);
    lv_obj_set_style_bg_color(s_set_panel, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(s_set_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_set_panel, 1, 0);
    lv_obj_set_style_border_color(s_set_panel, lv_color_hex(0x24344B), 0);
    lv_obj_set_style_radius(s_set_panel, 16, 0);
    lv_obj_set_style_pad_all(s_set_panel, 12, 0);
    lv_obj_remove_flag(s_set_panel, LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s_set_panel, &font_zh24, C_TEXT, "设置");
    lv_obj_t *close = mk_label(s_set_panel, &font_zh16, C_SUB, LV_SYMBOL_CLOSE);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 2);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(close, 8);
    lv_obj_add_event_cb(close, set_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = mk_label(s_set_panel, &font_zh16, C_SUB, "亮度");
    lv_obj_set_pos(bl, 0, 36);
    s_set_val = mk_label(s_set_panel, &font_zh16, C_TEXT, "60%");
    lv_obj_align(s_set_val, LV_ALIGN_TOP_RIGHT, 0, 36);

    lv_obj_t *sl = lv_slider_create(s_set_panel);
    lv_obj_set_pos(sl, 0, 58);
    lv_obj_set_size(sl, 256, 14);
    lv_slider_set_range(sl, 5, 100);
    lv_slider_set_value(sl, s_bright_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, set_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    mk_btn(s_set_panel, 0, 84, 124, 36, "刷新天气", set_refresh_cb, C_ACCENT);
    mk_btn(s_set_panel, 132, 84, 124, 36, "息屏", set_sleep_cb, C_CARD);
    mk_btn(s_set_panel, 0, 128, 124, 36, "关于", set_about_cb, C_CARD);
    mk_btn(s_set_panel, 132, 128, 124, 36, "关闭", set_close_cb, C_ACCENT);

    lv_obj_add_flag(s_set_panel, LV_OBJ_FLAG_HIDDEN);
}

static void settings_open_cb(lv_event_t *e)
{
    lv_obj_remove_flag(s_set_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_set_panel);
}

static void ui_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* A: clock */
    lv_obj_t *a = mk_card(scr, 4, 4, 196, 108);
    s_clock = mk_label(a, &font_zh48, C_TEXT, "--:--");
    lv_obj_set_pos(s_clock, 4, 0);
    s_date = mk_label(a, &font_zh16, C_SUB, "正在获取时间");
    lv_obj_set_pos(s_date, 6, 58);
    lv_obj_t *gear = mk_label(a, &font_zh16, C_SUB, "设置");
    lv_obj_set_pos(gear, 144, 76);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(gear, 10);
    lv_obj_add_event_cb(gear, settings_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *med = mk_label(a, &font_zh16, C_ACCENT, "媒体");
    lv_obj_set_pos(med, 96, 76);
    lv_obj_add_flag(med, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(med, 10);
    lv_obj_add_event_cb(med, med_open_cb, LV_EVENT_CLICKED, NULL);

    /* B: weather */
    lv_obj_t *b = mk_card(scr, 204, 4, 112, 108);
    mk_label(b, &font_zh16, C_ACCENT, "天气");
    s_temp = mk_label(b, &font_zh24, C_TEXT, "--");
    lv_obj_set_pos(s_temp, 0, 22);
    s_cond = mk_label(b, &font_zh16, C_TEXT, "获取中");
    lv_obj_set_pos(s_cond, 0, 50);
    s_hum = mk_label(b, &font_zh16, C_SUB, "湿度 --");
    lv_obj_set_pos(s_hum, 0, 70);

    /* C: device status */
    lv_obj_t *c = mk_card(scr, 4, 116, 196, 120);
    mk_label(c, &font_zh16, C_ACCENT, "设备状态");
    s_wifi = mk_label(c, &font_zh16, C_TEXT, "WiFi 连接中");
    lv_obj_set_pos(s_wifi, 0, 24);
    s_ip = mk_label(c, &font_zh16, C_TEXT, "IP -");
    lv_obj_set_pos(s_ip, 0, 46);
    s_mem = mk_label(c, &font_zh16, C_SUB, "内存 --KB 运行 --:--");
    lv_obj_set_pos(s_mem, 0, 68);
    s_wind = mk_label(c, &font_zh16, C_SUB, "风 --km/h");
    lv_obj_set_pos(s_wind, 0, 90);

    /* D: pomodoro */
    lv_obj_t *d = mk_card(scr, 204, 116, 112, 120);
    lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d, pomo_card_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(d, pomo_card_cb, LV_EVENT_LONG_PRESSED, NULL);
    mk_label(d, &font_zh16, C_ACCENT, "番茄钟");
    s_pomo_time = mk_label(d, &font_zh24, C_WARM, "25:00");
    lv_obj_align(s_pomo_time, LV_ALIGN_TOP_MID, 0, 24);
    s_pomo_state_lbl = mk_label(d, &font_zh16, C_ACCENT, "待命");
    lv_obj_align(s_pomo_state_lbl, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_t *hint = mk_label(d, &font_zh16, C_SUB, "点击切换");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 78);

    media_ui_build(scr);
    settings_build();
}

static void ui_timer_cb(lv_timer_t *t)
{
    static uint32_t last_wx_seq;
    media_ui_refresh();
    static int64_t last_retry;
    char buf[64];

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (now > 1700000000) {
        snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
        set_text(s_clock, buf);
        static const char *wd[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        snprintf(buf, sizeof buf, "%d月%d日 %s", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
        set_text(s_date, buf);
    } else {
        set_text(s_clock, "--:--");
        set_text(s_date, "正在获取时间");
    }

    if (s_wifi_ok) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
        snprintf(buf, sizeof buf, "WiFi %s %ddBm", CONFIG_DESKTOP_WIFI_SSID, s_rssi);
        set_text(s_wifi, buf);
        snprintf(buf, sizeof buf, "IP %s", s_ip_str);
        set_text(s_ip, buf);
        if (!s_sntp_started) {
            sntp_start();
            s_sntp_started = true;
            ESP_LOGI(TAG, "sntp started");
        }
    } else {
        set_text(s_wifi, "WiFi 未连接");
        set_text(s_ip, "IP -");
        int64_t ms = esp_timer_get_time() / 1000;
        if (ms - last_retry > 5000) {
            last_retry = ms;
            esp_wifi_connect();
        }
    }

    if (s_pomo == POMO_WORK || s_pomo == POMO_BREAK) {
        if (--s_pomo_left <= 0) {
            if (s_pomo == POMO_WORK) {
                s_pomo = POMO_BREAK;
                s_pomo_left = 5 * 60;
            } else {
                s_pomo = POMO_WORK;
                s_pomo_left = 25 * 60;
            }
        }
        pomo_refresh();
    }

    uint32_t up = esp_timer_get_time() / 1000000;
    snprintf(buf, sizeof buf, "内存 %uKB 运行 %02u:%02u",
             (unsigned)(esp_get_free_heap_size() / 1024), (unsigned)(up / 3600), (unsigned)(up / 60 % 60));
    set_text(s_mem, buf);

    uint32_t seq = s_wx.seq;
    if (seq != last_wx_seq && (seq & 1) == 0) {
        last_wx_seq = seq;
        if (s_wx.valid) {
            snprintf(buf, sizeof buf, "%.0f°C", s_wx.temp);
            set_text(s_temp, buf);
            set_text(s_cond, wx_text(s_wx.code));
            snprintf(buf, sizeof buf, "湿度 %d%%", s_wx.rh);
            set_text(s_hum, buf);
            snprintf(buf, sizeof buf, "风 %.0fkm/h", s_wx.wind);
            set_text(s_wind, buf);
        }
    }
}

void app_main(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    lcd_init();
    touch_init();

    s_media_mtx = xSemaphoreCreateMutex();
    s_cover_mtx = xSemaphoreCreateMutex();
    s_media_q = xQueueCreate(8, sizeof(media_cmd_t));
    media_build_auth();

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    static uint16_t buf1[W * 40], buf2[W * 40];
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_color_done};
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, disp));
    lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
                          true, &font_zh16);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_cb);

    ui_build();
    lv_timer_create(ui_timer_cb, 1000, NULL);
    xTaskCreate(weather_task, "weather", 8192, NULL, 4, NULL);
    xTaskCreate(media_task, "media", 6144, NULL, 1, NULL);

    wifi_init();
    ESP_LOGI(TAG, "desktop started");

    while (1) {
        uint32_t next = lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(next < 1 ? 1 : (next > 10 ? 10 : next)));
    }
}
