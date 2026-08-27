/* photo_queue.h — PSRAM-backed FIFO of captures awaiting MQTT delivery.
 *
 * Use case: the camera captures a frame and tries to publish it via
 * mqtt_publish_photo_event() + mqtt_publish_photo_image(). Those are
 * fire-and-forget — when MQTT is offline (WiFi flap, broker restart,
 * IoT VLAN hiccup), the publish is silently dropped. With the bench's
 * SD slot also dead and the field deployment relying on remote
 * durability, every dropped capture is gone forever.
 *
 * This queue holds those captures in PSRAM and re-publishes them when
 * MQTT comes back. Ring of 4 entries (header + JPEG body capped at
 * 400 KB) → ~2 MB PSRAM budget out of 8 MB available. FIFO on
 * overflow: oldest gets dropped (with `dropped_total` counter for
 * telemetry), since a brand-new shot is more useful than a 30-minute-
 * old one nobody saw.
 *
 * Not persistent across reboot — if the device crashes mid-outage,
 * the queue is lost. Persisting would need NVS or SPIFFS; deferred
 * until field data shows reboot-during-outage is common.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate the PSRAM ring + start the drain task. Idempotent. Returns
 * ESP_OK on success or ESP_ERR_NO_MEM if PSRAM allocation failed (in
 * which case enqueue calls will return ESP_ERR_INVALID_STATE and the
 * feature gracefully degrades to "drop captures during outages"). */
esp_err_t photo_queue_init(void);

/* Copy the JPEG + metadata into the next free ring entry. If the
 * ring is full, drops the OLDEST entry (incrementing the counter via
 * photo_queue_dropped_total) and uses its slot.
 *
 * `jpg` bytes are copied into our PSRAM buffer — caller can free its
 * own buffer after this returns. Returns ESP_OK on success,
 * ESP_ERR_INVALID_SIZE if jpg_len exceeds the per-entry cap,
 * ESP_ERR_INVALID_STATE if photo_queue_init() failed at boot.
 *
 * The drain task is NOT woken automatically — caller can choose to
 * call photo_queue_kick() if they expect MQTT might be back (most
 * callers just enqueue and let the next MQTT_EVENT_CONNECTED tick
 * trigger drain). */
esp_err_t photo_queue_enqueue(const uint8_t *jpg, size_t jpg_len,
                              const char *trigger, const char *path,
                              int64_t ts_us, uint32_t seq,
                              int agc_gain, bool ir_active,
                              int framesize, int quality);

/* Wake the drain task so it tries to flush the queue. Safe to call
 * from any context (including the MQTT event loop). No-op if the
 * task isn't running yet. */
void photo_queue_kick(void);

/* Telemetry — exposed for diag/boot, selftest, and HA dashboard. */
size_t   photo_queue_depth(void);          /* entries currently waiting */
size_t   photo_queue_bytes(void);          /* total JPEG bytes in queue */
uint32_t photo_queue_dropped_total(void);  /* FIFO-overflow drops since boot */
uint32_t photo_queue_drained_total(void);  /* successful redeliveries since boot */

#ifdef __cplusplus
}
#endif
