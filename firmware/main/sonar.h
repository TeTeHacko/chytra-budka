/* sonar.h — Grove Ultrasonic Ranger (single-pin SIG) distance sensor.
 *
 * Optional — runtime-enabled via the `sonar_enabled` NVS bool (default
 * OFF) + a pad mapped to the "sonar" pin function. Wiring (Grove cable):
 * SIG → the mapped D-pad, VCC → 3V3 (NOT 5V — the S3 GPIOs are not
 * 5V-tolerant and the module drives SIG at VCC), GND → GND. The white
 * NC wire stays unconnected.
 *
 * A polling task pings every `sonar_poll_s` seconds (median of 3) and
 * publishes <id>/state/distance_cm. While the OLED ENV page is
 * on-screen the task self-boosts to ~2 Hz for a live panel readout
 * (MQTT stays at the knob cadence). No echo (unplugged / out of range)
 * ⇒ no publish + one WARN on the transition, so a dead sensor is
 * visible in the log instead of freezing the last value.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boot init. No-op (ESP_OK) when sonar_enabled=OFF or no pad carries
 * the "sonar" function. */
esp_err_t sonar_init(void);

/* Live apply for the sonar_enabled toggle — arms/disarms the polling
 * task without a reboot (same contract as reed_apply_config). */
void sonar_apply_config(void);

/* True while the polling task is armed. */
bool sonar_ready(void);

/* Latest successful measurement in cm. False until the first good echo,
 * while the sensor is silent (absent), or while the reading is "clear"
 * (≥ sonar_clear_cm — see sonar_is_clear). */
bool sonar_last_cm(float *out_cm);

/* True while the last poll read "nothing in range": a valid echo whose
 * distance is ≥ the sonar_clear_cm knob (>0). These Grove modules emit
 * a fixed-width artifact pulse when there is no real target — it reads
 * as a constant fake distance (~60–80 cm on the bench at 3V3), which
 * this knob separates from genuine measurements. MQTT publishes the
 * 999 sentinel for it; the OLED shows "inf". */
bool sonar_is_clear(void);

/* Proximity photo trigger (sonar_trig_cm > 0): the poll task latches a
 * far→near edge (with +10 % / min 10 cm hysteresis on the far side);
 * the main loop consumes it into camera_request_event("sonar"). While
 * the trigger is armed the task samples continuously at ~2 Hz — the
 * sonar_poll_s knob then only caps the MQTT cadence. */
bool sonar_trigger_consume(void);
uint32_t sonar_trigger_count(void);

#ifdef __cplusplus
}
#endif
