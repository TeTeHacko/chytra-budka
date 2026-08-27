/* melody.c — see melody.h. */
#include "melody.h"

#include <stdint.h>
#include <stdlib.h>

size_t melody_parse(const char *s, speaker_note_t *out, size_t max) {
    if (!s || !out) return 0;
    size_t n = 0;
    while (*s && n < max) {
        while (*s == ',' || *s == ' ') s++;
        if (!*s) break;
        char *end;
        long f = strtol(s, &end, 10);
        if (end == s) break;                 /* not a number → stop */
        long ms = 120;
        if (*end == ':') ms = strtol(end + 1, &end, 10);
        if (f < 0) f = 0;
        if (f > 12000) f = 12000;
        if (ms < 10) ms = 10;
        if (ms > 5000) ms = 5000;
        out[n].freq_hz = (uint16_t)f;
        out[n].ms      = (uint16_t)ms;
        n++;
        s = end;
    }
    return n;
}
