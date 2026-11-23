#pragma once

#include <stddef.h>

void diag_log_install(void);
const char *diag_log_tail(int want, size_t *len);
int diag_build_status(char *buf, size_t cap);
