idf_component_register(SRCS "main.c" "ota.c" "settings.c" "screen.c" "diag.c" "net_server.c" "wifi_mgr.c" "portal.c"
                            "fonts/font_zh16.c" "fonts/font_zh24.c" "fonts/font_zh48.c" "fonts/font_zhfull.c"
                       INCLUDE_DIRS "." "fonts"
                       REQUIRES esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio
                                esp_driver_ledc esp_timer esp_wifi esp_event esp_netif nvs_flash
                                esp_http_client esp_http_server esp_https_ota app_update esp_psram lwip
                                esp-tls mbedtls lvgl)
