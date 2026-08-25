/* pir.h — AM312 PIR motion sensor on GPIO PIR_PIN.
 *
 * The sensor pulls its OUT line high for ~2 s on motion (internal
 * blocking time), then low. We poll the line every 20 ms in a
 * low-priority task and bump a monotonic counter on each confirmed
 * LOW→HIGH transition (after a short sample-confirm debounce).
 * Higher-level code consumes events via `pir_motion_consume()`,
 * which returns `true` exactly once per detected edge.
 *
 * Why polling (was ISR earlier): the strapping/floating-prone
 * PIR_PIN, combined with WiFi-TX coupling and the AM312's slow
 * warm-up, generated phantom-edge storms that saturated the GPIO
 * ISR and starved other tasks, eventually tripping `int_wdt` on
 * the bench. Polling absorbs arbitrary EMI bursts shorter than
 * the debounce window without touching the interrupt path.
 *
 * The PIR is RTC-capable on the XIAO so it can also wake the SoC
 * from deep sleep. That hook is exposed via
 * `pir_arm_deep_sleep_wakeup()` but is not used yet — firmware
 * currently stays awake. Deep-sleep wake is independent of the
 * polling task: RTC GPIO fires on the rising edge during sleep,
 * the task resumes on wake. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of PIR pins the module can drive simultaneously.
 * Same cap as reed for the same XIAO-header-arithmetic reason: only
 * 4 of the 8 user breakouts can plausibly be PIRs once you account
 * for I²C and the camera. */
#define PIR_MAX_INSTANCES 4

esp_err_t pir_init(void);
bool      pir_ready(void);

/* How many PIR instances are currently armed. */
int       pir_active_count(void);

/* Singleton API (backward-compat — operates on instance 0). */
bool      pir_motion_consume(void);
uint32_t  pir_motion_count(void);
uint32_t  pir_last_motion_ms(void);

/* Per-instance access. Out-of-range / unarmed indices return false / 0. */
bool      pir_motion_consume_nth(int idx);
uint32_t  pir_motion_count_nth(int idx);
uint32_t  pir_last_motion_ms_nth(int idx);
/* Raw line level (RTC-mux read for RTC-IO pins, else digital), -1 if unarmed.
 * Used by /debug/pir so it's accurate even in Safe+light-sleep. */
int       pir_raw_level_nth(int idx);

/* DIAGNOSTIC: cumulative poll-task iterations + HIGH reads (instance 0). Lets
 * /debug/pir reveal whether the poll runs at all in light-sleep and whether it
 * ever sees the line HIGH — separates "poll starved" / "sensor not asserting" /
 * "debounce bug". */
void      pir_debug_poll_stats(uint32_t *poll_iters, uint32_t *high_reads, uint32_t *ext1_wakes);

/* True iff the sensor appears stuck-HIGH: the polling task observed a
 * confirmed LOW→HIGH transition >5 minutes ago and the line has been
 * HIGH ever since. AM312's max HW hold time is 2 s, so anything past
 * a few minutes is the sensor itself failing — selftest surfaces this
 * so the operator can distinguish "PIR broken" from "no sensor wired"
 * without having to poll /debug/pir manually. */
bool      pir_wedged(void);

/* Legacy single-pin EXT0 deep-sleep arm — superseded by the EXT1 multi-pin mask
 * armed unconditionally in pir_init() (which serves both light AND deep sleep).
 * Kept for reference; not called. */
esp_err_t pir_arm_deep_sleep_wakeup(void);

/* True iff at least one RTC-capable PIR pin is armed for EXT1 wake (set at
 * pir_init). cb_ds logs this before deep sleep to confirm motion-wake is live. */
bool      pir_ds_wake_armed(void);

/* EXT1 wake bitmask of the RTC-capable PIR pins (0 if none). cb_ds OR's this
 * (only when pir_enabled) into the live deep-sleep wake mask so a disabled PIR
 * no longer wakes the unit. */
uint64_t  pir_rtc_pin_mask(void);

/* React to a runtime change of the `pir_enabled` NVS knob.
 *
 * Called from app_config::apply_side_effects on every cmd/cfg/pir_enabled
 * write. The polling task + GPIO arming stay live regardless (the gate
 * is consume-side in main.cpp, not arm-side here) — what this function
 * does is publish state/motion=false the moment the operator flips OFF
 * so the HA dashboard stops showing the residual ON until MOTION_HOLD_MS
 * (10 s) expires. Also re-publishes motion_count so the entity has a
 * fresh retained value across the toggle.
 *
 * Idempotent in both directions. No-op when pir_enabled flips ON
 * (next PIR fire will publish motion=true the normal way). */
void pir_apply_config(void);

#ifdef __cplusplus
}
#endif
