/* audiofx.h — one notification, both outputs.
 *
 * The single front-end every "make a sound" site should call. Each function
 * fans the same notification out to BOTH audio backends so it plays on
 * whatever is wired up (a buzzer, a PDM "1-bit DAC", or both at once):
 *
 *   - speaker.c  — LEDC square-wave chiptune on the "buzzer" pad
 *   - pcm.c      — I2S PDM sigma-delta on the "pcm" pad (embedded samples,
 *                  or tones synthesised from the same note list)
 *
 * Each backend self-gates on its pin-function mapping, so an unmapped output
 * is simply a no-op — map one pad, the other, or both.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "speaker.h"   /* speaker_note_t */

#ifdef __cplusplus
extern "C" {
#endif

/* One tone (freq_hz == 0 → a silent gap). */
void audiofx_beep(uint16_t freq_hz, uint16_t ms);

/* Play a melody once, staccato (articulated). */
void audiofx_play(const speaker_note_t *notes, size_t n);

/* Play once, legato (smooth — for sound effects/sweeps). */
void audiofx_sfx(const speaker_note_t *notes, size_t n);

/* Loop a melody until audiofx_stop(). */
void audiofx_loop(const speaker_note_t *notes, size_t n);

/* The SMB "coin" — the embedded sample on the PDM output + its chiptune
 * transcription on the buzzer. Used for the capture beep. */
void audiofx_coin(void);

/* The power-up boot jingle on both outputs. Call once after speaker_init()
 * and pcm_init() (so both backends are up). */
void audiofx_boot(void);

/* Lifecycle cues — one-shot, both outputs. Wired to the events of the same
 * name so every system sound flows through this one router. */
void audiofx_ota_start(void);   /* OTA download starting */
void audiofx_ota_done(void);    /* OTA image applied OK (just before reboot) */
void audiofx_ota_fail(void);    /* OTA download/apply failed */
void audiofx_ap(void);          /* SoftAP config mode came up */

/* Stop any in-progress playback/loop on both outputs. */
void audiofx_stop(void);

#ifdef __cplusplus
}
#endif
