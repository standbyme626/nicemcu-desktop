#pragma once

#include <stdbool.h>

static inline bool ota_version_is_newer(const char *cur, const char *new_ver)
{
    if (!cur || !new_ver) return false;
    const char *p = cur, *q = new_ver;
    for (;;) {
        unsigned a = 0, b = 0;
        while (*p >= '0' && *p <= '9') a = a * 10 + (unsigned)(*p++ - '0');
        while (*q >= '0' && *q <= '9') b = b * 10 + (unsigned)(*q++ - '0');
        if (a != b) return b > a;
        if ((*p && *p != '.') || (*q && *q != '.')) return false;
        if (*p == '.') p++;
        if (*q == '.') q++;
        if (!*p && !*q) return false;
    }
}
