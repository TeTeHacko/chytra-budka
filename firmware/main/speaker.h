/* speaker.h — 1-bit square-wave audio output ("buzzer" pin function).
 *
 * Drives whatever GPIO is mapped to the "buzzer" pin function (app_config
 * pin map) with an LEDC square wave. The `spkr_tone` NVS bool (read live at
 * playback time) selects how:
 *
 *   OFF = active buzzer — a self-drive piezo (e.g. Kingstate KPE-2xx): the
 *                 pin is driven full-on for the note duration; pitch comes
 *                 from the buzzer's own oscillator, so `freq_hz` only gates
 *                 on/off and adjacent notes get a short gap (melodies become
 *                 rhythm).
 *   ON  = tone   — a passive piezo / small speaker (via series R or a small
 *                 amp) OR a line input fed through a cap + divider: the LEDC
 *                 frequency follows `freq_hz`, so real chiptune melodies play.
 *
 * Playback is queued onto a dedicated task — every call is non-blocking, so
 * the MQTT/HTTP caller never stalls on a multi-second melody. Output is pure
 * square wave (no DAC); this is chiptune, not PCM audio.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One step of a melody. freq_hz: tone frequency in Hz (ignored for pitch in
 * "buzzer" sink — the element self-oscillates). freq_hz == 0 → a silent rest.
 * ms: how long to hold the step. */
typedef struct {
    uint16_t freq_hz;
    uint16_t ms;
} speaker_note_t;

/* Bind the LEDC output to the GPIO mapped to "buzzer" and start the playback
 * task. No-op (logs once) when no pin slot carries the "buzzer" function —
 * safe to call unconditionally from app_main. Idempotent. */
void speaker_init(void);

/* Queue a single tone (non-blocking). freq_hz == 0 → a silent gap. */
void speaker_beep(uint16_t freq_hz, uint16_t ms);

/* Queue a melody (non-blocking). `notes` is copied internally; the caller
 * keeps ownership and may pass a stack/const array. */
void speaker_play(const speaker_note_t *notes, size_t n);

/* Like speaker_play(), but repeats `notes` until speaker_stop() is called
 * (or the queue task is otherwise told to stop). Non-blocking; `notes` is
 * copied. A fresh loop/play request supersedes a running loop. */
void speaker_loop(const speaker_note_t *notes, size_t n);

/* Like speaker_play(), but legato — skips the staccato articulation so notes
 * glide together. For smooth sound effects (sweeps, the coin), not melodies. */
void speaker_sfx(const speaker_note_t *notes, size_t n);

/* Stop any in-progress playback or loop and silence the output. Safe to call
 * when nothing is playing or when the speaker is idle/unmapped. */
void speaker_stop(void);

/* Resolved buzzer GPIO once initialised, or -1 if no pad is mapped to "buzzer"
 * (or init failed). Read-only; for diagnostics / selftest. */
int speaker_gpio(void);

#ifdef __cplusplus
}
#endif
