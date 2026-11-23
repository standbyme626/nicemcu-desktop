#pragma once

#include <stdbool.h>

typedef enum {
    OTA_IDLE = 0,
    OTA_CHECKING,
    OTA_DOWNLOADING,
    OTA_UP_TO_DATE,
    OTA_FAILED,
    OTA_REBOOTING,
} ota_state_t;

void ota_start_boot_check(void);
bool ota_request_check(void);
bool ota_configured(void);
ota_state_t ota_get_state(void);
int ota_get_progress(void);
const char *ota_state_text(void);
const char *ota_last_error(void);
void ota_mark_valid_if_pending(void);
