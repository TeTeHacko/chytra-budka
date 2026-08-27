// ota.h — periodic HTTPS OTA poller.
//
// Spawns a low-priority FreeRTOS task that wakes every OTA_CHECK_PERIOD_MS,
// checks OTA_URL, compares image version (esp_app_desc) with the running
// firmware, and applies the update + restart on mismatch.
//
// Cert validation uses the embedded LE ISRG bundle (esp_crt_bundle).

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ota_init(void);

/* True iff the running image is still ESP_OTA_IMG_PENDING_VERIFY (not yet
 * mark-valid'd). cb_ds reads this to refuse deep sleep on a fresh OTA image —
 * sleeping would reboot into a bootloader rollback. Lock-free; any task. */
bool ota_img_pending_verify(void);

/* Run an OTA check synchronously on the calling task (used by hibernate's
 * on-wake "check if due"). Same body as the periodic poll; coalesces with the
 * background task via a re-entrancy guard. May esp_restart() on an applied
 * update — does not return in that case. */
void ota_check_now_blocking(void);

/* Request an immediate OTA poll outside the periodic schedule. Safe to
 * call from any task; the actual HTTPS GET happens on the OTA task. If
 * a check is already running, the request coalesces (next idle slot
 * picks it up). Used by the MQTT cmd/ota handler to skip the 5-min
 * poll cadence after a new image has been pushed. */
void ota_trigger_now(void);

/* OTA download progress for the bench OLED overlay: -1 when no OTA is in
 * flight, else 0..100 (%). Lock-free read; safe from any task. */
int ota_progress_pct(void);

#ifdef __cplusplus
}
#endif
