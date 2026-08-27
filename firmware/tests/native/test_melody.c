/* test_melody.c — host unit test for melody_parse() (melody.c).
 *
 * melody_parse turns an untrusted MQTT payload ("f:ms,f:ms,…") into a bounded
 * speaker_note_t[]. The things worth pinning: it clamps f/ms into the LEDC-safe
 * range, defaults ms, treats f==0 as a rest, tolerates leading separators,
 * stops at the first garbage token, never writes past `max`, and is NULL-safe.
 */
#include <assert.h>
#include <stdio.h>

#include "melody.h"

int main(void) {
    speaker_note_t n[8];

    /* basic two-note parse */
    assert(melody_parse("523:150,659:150", n, 8) == 2);
    assert(n[0].freq_hz == 523 && n[0].ms == 150);
    assert(n[1].freq_hz == 659 && n[1].ms == 150);

    /* ms defaults to 120 when ":ms" is omitted; mixed with explicit */
    assert(melody_parse("440,880:200", n, 8) == 2);
    assert(n[0].freq_hz == 440 && n[0].ms == 120);
    assert(n[1].freq_hz == 880 && n[1].ms == 200);

    /* f == 0 is a rest, kept verbatim */
    assert(melody_parse("0:300", n, 8) == 1);
    assert(n[0].freq_hz == 0 && n[0].ms == 300);

    /* clamping: negative f → 0, huge f → 12000; ms below 10 → 10, above 5000 → 5000 */
    assert(melody_parse("-5:5", n, 8) == 1);
    assert(n[0].freq_hz == 0 && n[0].ms == 10);
    assert(melody_parse("20000:9000", n, 8) == 1);
    assert(n[0].freq_hz == 12000 && n[0].ms == 5000);

    /* tolerant of leading/duplicate separators */
    assert(melody_parse(",  440:100 , 880", n, 8) == 2);
    assert(n[0].freq_hz == 440 && n[1].freq_hz == 880);

    /* stops at the first unparseable token (keeps what came before) */
    assert(melody_parse("440:100,xyz,880", n, 8) == 1);
    assert(n[0].freq_hz == 440 && n[0].ms == 100);

    /* never writes past `max` */
    assert(melody_parse("1,2,3,4,5", n, 3) == 3);

    /* empty / NULL are safe and yield zero notes */
    assert(melody_parse("", n, 8) == 0);
    assert(melody_parse(NULL, n, 8) == 0);
    assert(melody_parse("1", NULL, 8) == 0);
    assert(melody_parse("1", n, 0) == 0);

    printf("test_melody: all assertions passed\n");
    return 0;
}
