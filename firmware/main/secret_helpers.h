#pragma once

#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns true if `s` looks like an unreplaced placeholder from
 * secrets.h.example rather than a real credential.
 *
 * The example file uses two naming conventions for placeholders:
 *   - "placeholder-<purpose>"  (current convention: placeholder-bearer,
 *                               placeholder-mqtt, placeholder-ota)
 *   - "your-<...>"             (legacy:           your-shared-secret,
 *                               your-mqtt-password, your-ota-password)
 *
 * Catching both prefixes means a developer who copies secrets.h.example
 * and forgets to fill in any of the real values gets the same "module
 * disabled" warning regardless of which placeholder shape they left in.
 *
 * Adding a new placeholder is just a matter of using one of these
 * prefixes; this helper covers it automatically.
 */
static inline bool secret_is_placeholder(const char *s) {
    if (!s) return true;
    return strncmp(s, "placeholder-", 12) == 0 ||
           strncmp(s, "your-", 5) == 0;
}

#ifdef __cplusplus
}
#endif
