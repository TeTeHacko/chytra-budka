/* audiofx.c — see audiofx.h. Fan each notification out to both backends. */
#include "audiofx.h"

#include "pcm.h"
#include "speaker.h"

void audiofx_beep(uint16_t freq_hz, uint16_t ms) {
    speaker_beep(freq_hz, ms);
    speaker_note_t n = { freq_hz, ms };
    pcm_play_tones(&n, 1, /*legato*/ true, /*loop*/ false);
}

void audiofx_play(const speaker_note_t *notes, size_t n) {
    speaker_play(notes, n);
    pcm_play_tones(notes, n, /*legato*/ false, /*loop*/ false);
}

void audiofx_sfx(const speaker_note_t *notes, size_t n) {
    speaker_sfx(notes, n);
    pcm_play_tones(notes, n, /*legato*/ true, /*loop*/ false);
}

void audiofx_loop(const speaker_note_t *notes, size_t n) {
    speaker_loop(notes, n);
    pcm_play_tones(notes, n, /*legato*/ false, /*loop*/ true);
}

void audiofx_coin(void) {
    /* SMB "coin" (B5→E6), legato. On PCM we play the real recorded sample;
     * on the buzzer its chiptune transcription. Both, so it sounds on whichever
     * output is connected. */
    static const speaker_note_t coin[] = { { 988, 80 }, { 1319, 430 } };
    speaker_sfx(coin, sizeof(coin) / sizeof(coin[0]));
    pcm_play_named("coin");
}

void audiofx_boot(void) {
    /* Play once. Fired from the OLED splash render (oled.c) so the jingle lands
     * together with the boot screen; the latch stops the display self-heal from
     * re-splashing → re-playing it. Single caller (the OLED task), so a plain
     * flag is race-free here. */
    static bool played = false;
    if (played) return;
    played = true;

    /* Power-up boot jingle: three fast ascending runs (C → A♭ → B♭) climbing
     * ~3 octaves. Legato so it glides. */
    static const speaker_note_t powerup[] = {
        {  523, 36 }, {  392, 32 }, {  523, 32 }, {  659, 32 }, {  784, 32 }, { 1047, 32 }, {  784, 28 },
        {  415, 28 }, {  523, 32 }, {  622, 32 }, {  831, 32 }, {  622, 32 }, {  831, 36 }, { 1047, 32 }, { 1245, 32 }, { 1245, 28 },
        {  466, 32 }, {  587, 28 }, {  698, 36 }, {  932, 32 }, {  698, 32 }, {  932, 32 }, { 1175, 32 }, { 1397, 32 }, { 1865, 32 }, { 1397, 36 },
    };
    audiofx_sfx(powerup, sizeof(powerup) / sizeof(powerup[0]));
}

void audiofx_ota_start(void) {
    /* short rising "incoming" alert */
    static const speaker_note_t s[] = { { 784, 90 }, { 1047, 90 }, { 1319, 170 } };
    audiofx_sfx(s, sizeof(s) / sizeof(s[0]));
}

void audiofx_ota_done(void) {
    /* ascending success fanfare */
    static const speaker_note_t s[] = {
        { 1318, 90 }, { 1568, 90 }, { 2637, 90 }, { 2093, 90 }, { 2349, 90 }, { 3136, 220 },
    };
    audiofx_sfx(s, sizeof(s) / sizeof(s[0]));
}

void audiofx_ota_fail(void) {
    /* descending "failed" motif */
    static const speaker_note_t s[] = { { 622, 130 }, { 466, 130 }, { 311, 280 } };
    audiofx_sfx(s, sizeof(s) / sizeof(s[0]));
}

void audiofx_ap(void) {
    /* two-note "config mode up" chirp */
    static const speaker_note_t s[] = { { 880, 120 }, { 0, 60 }, { 1175, 220 } };
    audiofx_play(s, sizeof(s) / sizeof(s[0]));
}

void audiofx_stop(void) {
    speaker_stop();
    pcm_stop();
}
