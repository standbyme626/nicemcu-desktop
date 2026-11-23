#pragma once

#include <stdbool.h>
#include <stddef.h>

void wifi_mgr_start(void);
void wifi_start_portal(void);
bool wifi_is_connected(void);
bool wifi_is_portal(void);
const char *wifi_portal_ssid(void);
void wifi_get_ip(char *buf, size_t cap);
void wifi_get_ssid(char *buf, size_t cap);
int wifi_get_rssi(void);
void wifi_save_creds(const char *ssid, const char *pass);
