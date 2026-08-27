/* exif_read.h — minimal, defensive reader for the EXIF APP1 segment that
 * jpeg_stamp.c writes into every captured JPEG.
 *
 * This is the read side of jpeg_stamp.c's build_exif_app1(): it locates the
 * APP1 "Exif" segment, walks IFD0 + the ExifSubIFD, and pulls out the handful
 * of tags the device web UI wants to show as an HTML/CSS overlay — capture
 * time, trigger, device id, firmware version, sensor, and the UserComment
 * telemetry JSON (AGC/IR/RSSI/temp/vbatt/…).
 *
 * Reading EXIF is NOT a JPEG decode — it's a bounded scan of the APP1 header
 * (the first ~1 KB), so it is cheap enough to run on the ESP32-S3 (unlike the
 * pixel burn-in this replaces; see jpeg_stamp.h).
 *
 * Pure libc: no ESP-IDF, no allocation. Treats its input as UNTRUSTED — every
 * offset/length is bounds-checked against the buffer, so a truncated or
 * corrupt file on the SD card yields empty fields, never an out-of-bounds read.
 * Host-tested in firmware/tests/native/test_exif.c. */

#ifndef CHYTRA_BUDKA_EXIF_READ_H
#define CHYTRA_BUDKA_EXIF_READ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char datetime[20];   /* "YYYY:MM:DD HH:MM:SS" — DateTimeOriginal (0x9003),
                          * falling back to IFD0 DateTime (0x0132). */
    char offset[8];      /* "+HH:MM" — OffsetTime (0x9010). */
    char trigger[24];    /* ImageDescription (0x010E) — pir/vad/mqtt/… */
    char model[40];      /* Model (0x0110) — device id / MQTT topic base. */
    char software[40];   /* Software (0x0131) — firmware version. */
    char lens[24];       /* LensModel (0xA434) — sensor name (OV3660/…). */
    char user_json[320]; /* UserComment (0x9286) JSON payload (charset prefix
                          * stripped) — seq/uptime_s/agc/ir/rssi/heap/… */
    bool have_dt;        /* true iff datetime is present AND a real clock
                          * (not the "1970:…" pre-SNTP sentinel). */
} exif_meta_t;

/* Parse the EXIF metadata out of a JPEG buffer. Returns true if an APP1 Exif
 * segment with a valid TIFF header was found (individual fields may still be
 * empty if their tags were absent); false if there is no parseable EXIF.
 * `out` is always zeroed first, so it is safe to use even on a false return. */
bool exif_read(const uint8_t *jpeg, size_t len, exif_meta_t *out);

/* Extract one numeric value for `key` out of a compact JSON object string such
 * as exif_meta_t.user_json. Matches the exact quoted key ("agc", "rssi", …),
 * handles integers and floats (incl. negatives). Returns true + sets *out when
 * found. Pure string scan — no allocation, NUL-terminated input assumed. */
bool exif_json_num(const char *json, const char *key, double *out);

#ifdef __cplusplus
}
#endif

#endif /* CHYTRA_BUDKA_EXIF_READ_H */
