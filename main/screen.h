#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#define SCREEN_W 320
#define SCREEN_H 240

void screen_init(int bl_gpio);
void screen_set_bright(int pct);
int screen_get_bright(void);
void screen_sleep(void);
void screen_wake(void);
bool screen_is_asleep(void);

void screen_flush_hook(const lv_area_t *a, const uint8_t *px);

bool screen_snapshot_begin(void);
const uint16_t *screen_snapshot_data(void);
void screen_snapshot_end(void);
