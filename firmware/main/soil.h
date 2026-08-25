/* soil.h — Grove Soil Moisture (analog resistive probe) on the "soil"
 * pin function.
 *
 * Optional — runtime-enabled via the `soil_enabled` NVS bool (default
 * OFF) + a pad mapped to the "soil" pin function. The pad must be
 * ADC1-capable: D0..D5 = GPIO1..6 qualify, D6/D7 = GPIO43/44 do NOT
 * (the pin-map setter refuses those up front). Wiring (Grove cable):
 * SIG → the mapped D-pad, VCC → 3V3, GND → GND.
 *
 * A poll task takes a one-shot ADC average every `soil_poll_s` seconds
 * (live — drop to 1–2 s while calibrating) and publishes soil_mv +
 * soil_moist. Percentage comes from the live-tunable soil_dry_mv /
 * soil_wet_mv calibration knobs: probe in dry air → copy the published
 * soil_mv into soil_dry_mv; probe shorted / in wet soil → copy into
 * soil_wet_mv.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boot init. No-op (ESP_OK) when soil_enabled=OFF or no ADC1 pad
 * carries the "soil" function. */
esp_err_t soil_init(void);

/* Live apply for the soil_enabled toggle — arms/disarms the poll task
 * without a reboot (the ADC unit itself is created once and kept). */
void soil_apply_config(void);

/* True while the poll task is armed. */
bool soil_ready(void);

/* Fresh averaged read (poll-task internal, but safe to call from the
 * telemetry owner too). out_mv = calibrated millivolts on SIG; out_pct
 * = calibrated moisture percent, NAN when the dry/wet knobs are
 * degenerate (wet ≤ dry). False when disabled/unarmed or the ADC read
 * fails. Also refreshes the cache behind soil_last(). */
bool soil_read(float *out_mv, float *out_pct);

/* Cached copy of the last successful soil_read() — for the HTML status
 * page, which must never do its own conversions. */
bool soil_last(float *out_mv, float *out_pct);

#ifdef __cplusplus
}
#endif
