#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Safer alternative to the common `p += snprintf(p, left, ...); left -= w;`
 * chain. That chain underflows `size_t left` to ~4 GB on truncation and the
 * next append scribbles past the buffer end.
 *
 * Writes formatted text into *p with *left bytes available (the count
 * vsnprintf was given, including space for its NUL). On success advances
 * *p and decrements *left. On truncation (or vsnprintf error) sets
 * *left = 0 so subsequent calls become no-ops; the buffer stays
 * NUL-terminated by the just-completed vsnprintf write.
 *
 * Returns true if the format string fit fully, false on truncation/error.
 */
static inline bool text_append(char **p, size_t *left, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static inline bool text_append(char **p, size_t *left, const char *fmt, ...) {
    if (*left == 0) return false;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(*p, *left, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= *left) {
        *left = 0;
        return false;
    }
    *p += (size_t)w;
    *left -= (size_t)w;
    return true;
}

#ifdef __cplusplus
}
#endif
