#pragma once

enum {
    APP_UI_NONE = 0,
    APP_UI_DASHBOARD = 1,
    APP_UI_MEDIA = 2,
    APP_UI_SETTINGS = 3,
    APP_UI_QUEUE = 4,
    APP_UI_CALIB = 5,
    APP_UI_ABOUT = 6,
    APP_UI_OTA = 7,
};

void app_ui_request(int page);
void app_get_queue_info(int *n, int *start, int *total);
