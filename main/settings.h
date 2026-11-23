#pragma once

#include <stdint.h>

typedef struct {
    uint32_t version;
    uint8_t bright;
    uint16_t pomo_work_min;
    uint16_t pomo_break_min;
    char lat[16];
    char lon[16];
    char media_host[64];
    float ta, tb, tc, td, te, tf;
} desktop_settings_t;

void settings_init(void);
desktop_settings_t *settings_get(void);
void settings_save(void);
void settings_reset(void);
