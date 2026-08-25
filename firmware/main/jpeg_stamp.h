/* jpeg_stamp.h — add EXIF metadata ("which board, when, why") to captured
 * frames so the relay / SD images are self-describing without a sidecar.
 *
 * NO BURNED-IN VISUAL OVERLAY (deliberate). A visible caption was built and
 * bench-verified, then removed: the OV3660 emits JPEG on-chip with no OSD,
 * and the ESP32-S3 has no hardware JPEG codec, so burning even a few pixels
 * needs a full software decode→re-encode of the whole frame — measured
 * ~3.6 s at UXGA, on a solar field unit, for a caption. The same data is in
 * the EXIF here + the MQTT event JSON; tools/timelapse.py burns date/board
 * onto frames for the "video" case and HA can overlay the live view at zero
 * device cost. See NOTES.md for the full dead-end record.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *mac;          /* e.g. "aa:bb:cc:dd:ee:01" */
    const char *trigger;      /* "pir" / "vad" / "mqtt" / "http" */
    uint32_t    sequence;     /* monotonic capture counter */
    uint32_t    uptime_s;     /* seconds since boot */
    int64_t     epoch_ms;     /* wall-clock ms, 0 if SNTP not synced */
    int         agc_gain;     /* sensor AGC at decision time, -1 if unknown */
    bool        ir_active;    /* whether the IR illuminator fired for this shot */
    uint32_t    capture_ms;   /* wall-clock duration: mutex acquired → fb ready */
} jpeg_stamp_info_t;

/* Take a JPEG buffer and add an EXIF APP1 segment carrying the capture
 * metadata (DateTimeOriginal, Make/Model=hostname, Software, trigger,
 * UserComment JSON). Metadata only — no pixels are touched (see the file
 * header for why there is no visual overlay). width/height/jpeg_quality are
 * accepted for API stability but unused. Output buffer is allocated here
 * (PSRAM preferred); caller frees with free(). Returns 0 on failure (caller
 * should fall back to the unstamped frame). */
size_t jpeg_stamp_apply(const uint8_t *src, size_t src_len,
                        uint16_t width, uint16_t height,
                        uint8_t jpeg_quality,
                        const jpeg_stamp_info_t *info,
                        uint8_t **out_buf);

#ifdef __cplusplus
}
#endif
