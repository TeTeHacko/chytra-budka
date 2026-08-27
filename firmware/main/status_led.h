/* status_led.h — onboard XIAO ESP32-S3 status LED debug patterns.
 *
 * XIAO ESP32-S3 exposes the user LED on GPIO21 (active-low on Seeed
 * boards). The module keeps LED handling out of hot paths: callers only
 * update coarse state flags; a tiny background task renders blink patterns.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent. Reads runtime config keys status_led_en/status_led_dbg. */
void status_led_init(void);

/* Connectivity state hooks. */
void status_led_wifi_connected(bool connected);
void status_led_mqtt_connected(bool connected);

/* Short event pulses. */
void status_led_pir_pulse(void);

/* Long-running activity hooks. Nesting is not supported/needed; capture is
 * serialized elsewhere, OTA is single-tasked. */
void status_led_capture_begin(void);
void status_led_capture_end(void);
void status_led_ota_active(bool active);

#ifdef __cplusplus
}
#endif
