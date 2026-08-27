/* reed.h — magnetic reed-switch driver (door/lid contact).
 *
 * Optional sensor wired between REED_PIN and GND. Firmware drives the
 * line with an internal pull-up so the contact reads HIGH when the
 * magnet is away (switch open) and LOW when the magnet is present
 * (switch closed, line shorted to GND).
 *
 * Debounce: sample-and-confirm in a polling task (see reed.c for
 * parameters). The earlier ISR + per-edge lockout filtered the
 * mechanical bounce but not EMI/coupling on the long lid harness,
 * which produced tens of fake transitions per hour. Polling at 20 ms
 * with a 5-sample confirmation window gives ~100 ms response and
 * tolerates arbitrary EMI patterns short of sustained line
 * toggling.
 *
 * Currently the module just exposes presence + count; no behaviour
 * (capture, mode switch, etc.) is tied to it yet. Useful as a "door
 * was opened" telemetry feed; downstream consumers can pick it up
 * from MQTT or query the API directly.
 *
 * Gated by the reed_enabled NVS bool (default OFF). When disabled the
 * GPIO is left untouched and no task is spawned. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of reed pins the module can drive simultaneously.
 * Capped low because the XIAO header only has 8 user-accessible
 * breakouts and 4 of those are I²C — a board with 4 reeds is already
 * dedicating half its pins to door/lid contacts. */
#define REED_MAX_INSTANCES 4

/* Configure GPIO + ISR if reed_enabled is true; no-op otherwise.
 * Idempotent. Returns ESP_OK even when disabled (a missing sensor is
 * not a boot-blocking failure). */
esp_err_t reed_init(void);

/* True when the module armed at least one reed (implies the
 * reed_enabled gate was ON and at least one slot in the pin function
 * map is set to "reed"). */
bool reed_ready(void);

/* How many reed instances are currently armed (0..REED_MAX_INSTANCES).
 * 0 means reed_init found no GPIO mapped to "reed" or the master gate
 * is OFF. */
int reed_active_count(void);

/* Singleton API (backward-compat for callers that only care about the
 * first / only reed). These are thin wrappers around the _nth variants
 * with idx=0 so the historic single-reed code paths keep working
 * without change. Safe to call when no reed is armed — they return
 * false / 0 in that case. */
bool reed_is_closed(void);
uint32_t reed_event_count(void);
bool reed_event_consume(void);

/* Per-instance access (idx in 0..reed_active_count()-1). Callers
 * iterate via `for (int i = 0; i < reed_active_count(); i++)`. Out-
 * of-range or unarmed indices return false / 0. */
bool reed_is_closed_nth(int idx);
uint32_t reed_event_count_nth(int idx);
bool reed_event_consume_nth(int idx);

/* React to a runtime change of the `reed_enabled` NVS knob.
 *
 * Called from app_config.c::apply_side_effects when the operator
 * toggles the switch via MQTT/HA. Idempotent in both directions —
 * if the desired state already matches the running state, no-op.
 *
 * ON  → arm GPIO + spawn poll task + publish initial state/reed so
 *       HA's binary_sensor.door_lid stops showing "unknown".
 * OFF → signal the poll task to exit, mark not-ready. The GPIO is
 *       left in input + pull-up (cheap idle, ~10 µA).
 *
 * No reboot required after toggling. */
void reed_apply_config(void);

#ifdef __cplusplus
}
#endif
