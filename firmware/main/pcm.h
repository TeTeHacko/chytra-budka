/* pcm.h — real PCM-sample playback via I2S PDM TX ("1-bit DAC").
 *
 * Streams 16-bit / 16 kHz mono audio out one GPIO as a hardware sigma-delta
 * (PDM) bitstream. With an external RC low-pass on that pin (the DIY "1-bit
 * DAC") + a small amp it reproduces real sound samples — unlike the LEDC
 * square-wave chiptune in speaker.c.
 *
 * Gated by the pin map: claims its GPIO only when a pad is mapped to the "pcm"
 * pin function (app_config), so unmapped / field boards stay silent.
 *
 * Two sources: flash-embedded samples (pcm_play / pcm_play_named) and tones
 * synthesised on the fly from a note list (pcm_play_tones) — the latter lets
 * the same melodies the buzzer plays come out of the PDM output too. See the
 * audiofx.* fan-out layer, which drives both outputs together.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "speaker.h"   /* speaker_note_t — shared melody element */

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate the I2S PDM TX channel + playback task. No-op unless a pad is
 * mapped to "pcm". Idempotent, soft-fail. */
void pcm_init(void);

/* Queue a 16-bit mono 16 kHz clip (typically a flash-embedded sample) for
 * playback. Non-blocking; the caller keeps ownership of `samples`. */
void pcm_play(const int16_t *samples, size_t n);

/* Play an embedded sound by name (e.g. "coin"). Returns false if unknown. */
bool pcm_play_named(const char *name);

/* Synthesise and play a note list as square (chiptune) tones through the PDM —
 * the sample-domain equivalent of speaker_play/sfx/loop. `legato` skips the
 * staccato carve; `loop` repeats until pcm_stop(). `notes` is copied
 * internally. No-op unless a pad is mapped to "pcm". */
void pcm_play_tones(const speaker_note_t *notes, size_t n, bool legato, bool loop);

/* Stop any in-progress sample/tone playback or loop. Safe when idle/unmapped. */
void pcm_stop(void);

/* Resolved PDM-out GPIO once initialised, or -1 if no pad is mapped to "pcm"
 * (or init failed). Read-only; for diagnostics / selftest. */
int pcm_gpio(void);

#ifdef __cplusplus
}
#endif
