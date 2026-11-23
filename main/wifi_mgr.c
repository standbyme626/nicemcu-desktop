#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "net_server.h"
#include "ota.h"
#include "portal.h"
#include "wifi_mgr.h"

#define TAG "wifi"
#define NS "wifi"
#define CONNECTED_BIT BIT0

static EventGroupHandle_t s_evt;
static esp_netif_t *s_sta_nif;
static bool s_inited;
static volatile bool s_portal;
static char s_ap_ssid[32];

static void load_creds(char *ssid, size_t scap, char *pass, size_t pcap)
{
    ssid[0] = 0;
    pass[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t l = scap;
    nvs_get_str(h, "ssid", ssid, &l);
    l = pcap;
    nvs_get_str(h, "pass", pass, &l);
    nvs_close(h);
}

static void save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

void wifi_save_creds(const char *ssid, const char *pass)
{
    save_creds(ssid, pass);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_portal) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "disconnected reason=%d", ev ? ev->reason : -1);
        xEventGroupClearBits(s_evt, CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_evt, CONNECTED_BIT);
        ota_start_boot_check();
        net_server_start();
    }
}

static void wifi_common_init(void)
{
    if (s_inited) return;
    s_inited = true;
    s_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    s_sta_nif = esp_netif_create_default_wifi_sta();
    if (s_sta_nif) esp_netif_set_hostname(s_sta_nif, "nicemcu-desktop");
}

static bool sta_connect(const char *ssid, const char *pass, int timeout_ms)
{
    esp_wifi_stop();
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return false;
    xEventGroupClearBits(s_evt, CONNECTED_BIT);
    if (esp_wifi_start() != ESP_OK) return false;
    esp_wifi_set_ps(WIFI_PS_NONE);
    EventBits_t bits = xEventGroupWaitBits(s_evt, CONNECTED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & CONNECTED_BIT) != 0;
}

static bool boot_btn_held(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    if (gpio_get_level(0) != 0) return false;
    vTaskDelay(pdMS_TO_TICKS(1200));
    return gpio_get_level(0) == 0;
}

static void watchdog_task(void *arg)
{
    int down = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (s_portal) continue;
        if (wifi_is_connected()) {
            down = 0;
            continue;
        }
        down += 5;
        if (down == 30) {
            ESP_LOGW(TAG, "wifi down 30s -> reconnect");
            esp_wifi_connect();
        }
        if (down >= 600) {
            ESP_LOGE(TAG, "wifi down %ds -> reboot", down);
            esp_restart();
        }
    }
}

static void wifi_task(void *arg)
{
    wifi_common_init();

    bool force_portal = boot_btn_held();
    if (force_portal) ESP_LOGI(TAG, "BOOT held -> portal");
    if (!force_portal) {
        char ssid[33] = {0}, pass[65] = {0};
        load_creds(ssid, sizeof ssid, pass, sizeof pass);
        bool seed = false;
        if (!ssid[0]) {
            snprintf(ssid, sizeof ssid, "%s", CONFIG_DESKTOP_WIFI_SSID);
            snprintf(pass, sizeof pass, "%s", CONFIG_DESKTOP_WIFI_PASSWORD);
            seed = true;
        }
        if (ssid[0] && sta_connect(ssid, pass, 15000)) {
            if (seed) save_creds(ssid, pass);
            xTaskCreate(watchdog_task, "wifi_wd", 3072, NULL, 3, NULL);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "STA failed -> portal");
        esp_wifi_stop();
    }

    s_portal = true;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "NiceMCU-%02x%02x%02x", mac[3], mac[4], mac[5]);
    portal_start(s_ap_ssid);
    vTaskDelete(NULL);
}

void wifi_mgr_start(void)
{
    xTaskCreate(wifi_task, "wifi", 8192, NULL, 5, NULL);
}

void wifi_start_portal(void)
{
    if (s_portal) return;
    s_portal = true;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "NiceMCU-%02x%02x%02x", mac[3], mac[4], mac[5]);
    portal_start(s_ap_ssid);
}

bool wifi_is_connected(void)
{
    return s_evt && (xEventGroupGetBits(s_evt) & CONNECTED_BIT) != 0;
}

bool wifi_is_portal(void)
{
    return s_portal;
}

const char *wifi_portal_ssid(void)
{
    return s_ap_ssid;
}

void wifi_get_ip(char *buf, size_t cap)
{
    snprintf(buf, cap, "0.0.0.0");
    if (!s_sta_nif) return;
    esp_netif_ip_info_t ipi;
    if (esp_netif_get_ip_info(s_sta_nif, &ipi) == ESP_OK && ipi.ip.addr) {
        snprintf(buf, cap, IPSTR, IP2STR(&ipi.ip));
    }
}

void wifi_get_ssid(char *buf, size_t cap)
{
    snprintf(buf, cap, "-");
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(buf, cap, "%s", (char *)ap.ssid);
    }
}

int wifi_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}
