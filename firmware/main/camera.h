/* camera.h — OV2640/OV3660 capture on XIAO ESP32-S3 Sense.
 *
 * Driver wrapper around espressif/esp32-camera. PSRAM-backed JPEG
 * framebuffer. Initializes once; capture returns a borrowed pointer
 * that must be released back via camera_release_fb().
 *
 * Trigger sources call camera_capture_event() which performs:
 *   1. Optional IR LED warm-up
 *   2. JPEG capture (UXGA 1600x1200, quality 12)
 *   3. Save to /sdcard/<timestamp>.jpg if SD mounted
 *   4. Publish MQTT thumbnail event (metadata only)
 *   5. Update last-JPEG cache for HTTP /last.jpg
 *   6. IR LED off
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_camera.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the sensor (OV2640 or OV3660, auto-detected by the
 * driver) + LEDC channel for IR LED. Idempotent.
 * Returns ESP_OK on success, ESP_ERR_NOT_FOUND if no camera detected. */
esp_err_t camera_init(void);

bool camera_ready(void);

/* Borrow a freshly captured JPEG frame. Caller MUST call
 * camera_release_fb() when done. NULL on error. */
camera_fb_t *camera_capture_fb(void);
void         camera_release_fb(camera_fb_t *fb);

/* Grab one JPEG frame into a caller-owned heap copy, serialized with the
 * capture path (safe to call concurrently with camera_capture_event()).
 * Returns byte length (0 on failure); caller frees *out. */
size_t camera_grab_jpeg_copy(uint8_t **out);

/* High-level: capture one photo, save to SD (if mounted), publish
 * MQTT event, refresh last-JPEG cache. The trigger reason is
 * included in the MQTT event payload.
 *
 * Synchronous (blocks ~100–500 ms; IR shot adds ~480 ms warmup).
 * Callers from the main supervisor loop should use
 * camera_request_event() instead so they don't block the 1 Hz tick. */
esp_err_t camera_capture_event(const char *trigger_reason);

/* Enqueue a capture request onto the worker queue (capacity 4, drop-
 * oldest semantics so the freshest trigger always lands). Non-blocking;
 * the worker on CPU1 prio 5 picks it up asynchronously and calls
 * camera_capture_event() internally. Returns ESP_OK if enqueued (even
 * when an older entry was discarded to make room), ESP_FAIL if the
 * worker isn't running yet. The trigger string is copied (≤15 chars,
 * truncated longer ones with a debug log). */
esp_err_t camera_request_event(const char *trigger_reason);

/* Spawn the camera worker task pinned to CPU1 prio 5. Creates the
 * internal request queue if needed. Idempotent. */
void camera_worker_start(void);

/* Total capture-request enqueue drops since boot (queue full while a
 * new trigger arrived; drop-oldest discarded an earlier slot). HA/MQTT
 * surfaces this so a flood of triggers under unattended operation is
 * visible, not buried behind the 1/min UART log throttle. */
uint32_t camera_request_drops_total(void);

/* True iff camera_worker_start() succeeded and both the request queue
 * and worker task are live. Selftest surfaces this so a silent
 * xTaskCreate / xQueueCreate OOM doesn't masquerade as "all captures
 * fail" with no visible cause. */
bool camera_worker_running(void);

/* IR LED control (manual override; capture_event handles it
 * automatically). Duty 0..255. */
void camera_ir_led_set(uint8_t duty);

/* Read the sensor's current AGC gain (refreshes the driver's status
 * cache from live registers — a few SCCB transactions). Returns the
 * gain (0..30+ for OV3660 — higher = darker scene) on success, -1 if
 * no sensor or refresh failed. Safe to call concurrently with a
 * capture; serializes via the same mutex used by camera_capture_event. */
int camera_get_agc_gain(void);

/* Read both live AE controls in one SCCB refresh: AGC gain (0..30+, higher =
 * darker) and the AEC exposure value (line-units, rises as the scene darkens —
 * the responsive ambient-light proxy). Returns true on success (outputs filled),
 * false on no sensor / SCCB contention. Used by the bench OLED light bar. */
bool camera_get_ae(int *gain, int *exposure);

#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
/* BENCH-ONLY measurement spike: toggle the OV3660 software power-down
 * (SYSTEM_CTROL0 0x3008 bit6) to measure the sensor's residual analog draw
 * on the bench power meter. Holds s_capture_mtx + a NO_LIGHT_SLEEP lock;
 * drains a few frames on wake. NOT wired into the mode FSM and compiled out
 * of field/production. Exposed via GET /debug/cam_standby?on=1|0. */
esp_err_t camera_debug_sensor_standby(bool on);
#endif

/* Re-read app_config orientation (cam_rotate_180) and apply via
 * vflip/hmirror sensor registers. Called by camera_init at startup
 * and by app_config when the user toggles the switch over MQTT —
 * no reinit needed, the next captured frame picks up the new flip. */
void camera_apply_orientation(void);

/* Apply the "capture" profile (cam_framesize + cam_quality from NVS) to
 * the sensor. Default state after boot. Called by app_config when the
 * user changes cam_*, and by the MJPEG handler on every stream-exit
 * path. Holds s_capture_mtx briefly; idempotent. */
esp_err_t camera_apply_capture_profile(void);

/* Apply the "stream" profile (mjpg_framesize + mjpg_quality from NVS)
 * to the sensor. Called by the MJPEG handler on stream entry.
 * Holds s_capture_mtx briefly; idempotent. */
esp_err_t camera_apply_stream_profile(void);

/* True iff the sensor is currently configured for the MJPEG stream
 * profile. Used by app_config to decide whether a cam_/mjpg_ key
 * change should apply immediately or be deferred to the next profile
 * switch. */
bool camera_stream_profile_active(void);

/* Statistics (since boot). */
uint32_t camera_capture_count(void);
uint32_t camera_capture_failures(void);

/* Copy the cached JPEG (populated by camera_capture_event()) out under
 * the lock, returning a malloc()ed buffer the caller MUST free().
 * HTTP /last.jpg uses this to avoid holding the cache mutex across the
 * slow socket send, which would otherwise let a slow client wedge the
 * next capture's cache update. Returns NULL if no capture is available
 * or allocation fails. *out_len is set on success. */
uint8_t *camera_last_jpeg_dup(size_t *out_len);

/* Copy up to `cap` bytes from the FRONT of the cached JPEG into `out`, under
 * the lock, returning the number copied (0 if no capture yet). Cheap way to
 * read the EXIF header (which lives right after SOI) without duplicating the
 * whole multi-hundred-KB frame the way camera_last_jpeg_dup() does. */
size_t camera_last_jpeg_peek_header(uint8_t *out, size_t cap);

/* Pixel dimensions of the CAPTURE-profile framesize (cam_framesize), looked up
 * in the esp32-camera resolution table. Lets the web UI reserve the correct
 * aspect-ratio for the photo preview without hardcoding 4:3. Returns false (and
 * zeroes both outputs) if the framesize is out of range. */
bool camera_capture_dimensions(uint16_t *w, uint16_t *h);

#ifdef __cplusplus
}
#endif
