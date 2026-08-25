/* melody.h — parse a "f:ms,f:ms,…" melody string into speaker_note_t[].
 *
 * Pulled out of mqtt.c so it's pure (libc only, no ESP deps) and therefore
 * host-unit-testable — it parses untrusted MQTT payloads, so the clamping and
 * stop-on-garbage behaviour are worth pinning down in a test. */
#pragma once

#include <stddef.h>

#include "speaker.h"   /* speaker_note_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Parse "f:ms,f:ms,…" — f in Hz (0 = rest), ms = duration (default 120 when
 * ":ms" is omitted) — into out[]. Tolerant: skips leading commas/spaces, stops
 * at the first unparseable token. Each value is clamped (f 0..12000,
 * ms 10..5000). Returns the note count written (<= max). Safe on NULL.  */
size_t melody_parse(const char *s, speaker_note_t *out, size_t max);

#ifdef __cplusplus
}
#endif
