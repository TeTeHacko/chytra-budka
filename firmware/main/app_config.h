/* app_config.h — runtime-mutable configuration backed by NVS.
 *
 * Schema-driven. Each key is declared once in app_config.c as a
 * cfg_entry_t and is then accessible at runtime via the typed getters.
 *
 * Workflow:
 *   1) app_config_init() opens NVS and loads each entry's cached value
 *      (or default).
 *   2) Any module reads the live value via app_config_get_<type>(key).
 *   3) MQTT receives chytra-budka/cmd/cfg/<key> with a value payload,
 *      app_config_set_from_string() validates + persists + republishes
 *      retained state to chytra-budka/state/cfg/<key>.
 *   4) On every MQTT (re)connect, app_config_publish_state_all() and
 *      app_config_publish_discovery() are called to keep HA in sync.
 *
 * Live application: float/int/bool getters always return the latest
 * cached value, so most consumers (telemetry periods, camera_enabled,
 * pir_enabled, force_mode, etc.) need no callback. The exception is
 * cb::Vad, whose Config is captured at object construction; audio.cpp
 * exposes audio_apply_config() to re-init the VAD when its keys change. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_config_init(void);

/* Erase the cb_cfg NVS namespace and restore every cached value to its
 * schema default (config-only reset). Does NOT touch WiFi creds or TLS
 * certs. Re-publishes retained state. Callers usually reboot afterward so
 * pin-map / apply-on-change side effects re-run from a clean state. */
esp_err_t app_config_reset_defaults(void);

/* Introspection for the local web /config page (mirrors what HA discovery
 * exposes over MQTT). Lets http_server render + apply every setting without
 * duplicating the schema. */
size_t      app_config_count(void);
const char *app_config_key_at(size_t i);
/* Render entry i as an HTML <p><label>…<input/select></label></p> form row
 * (number / ON-OFF select / mode + pin selects), pre-filled with the current
 * value. Returns false if i is out of range. */
bool        app_config_form_row(size_t i, char *buf, size_t cap);

/* Typed getters — never fail; on unknown key the schema default is returned. */
float    app_config_get_float(const char *key);
int32_t  app_config_get_int(const char *key);
bool     app_config_get_bool(const char *key);

/* Parse `value` according to the entry's type and persist. Re-publishes
 * the retained state topic. Returns ESP_ERR_NOT_FOUND for unknown keys
 * or ESP_ERR_INVALID_ARG when out of range / unparseable. */
esp_err_t app_config_set_from_string(const char *key, const char *value);

/* Idempotent: call from MQTT_EVENT_CONNECTED. */
void app_config_publish_discovery(void);
void app_config_publish_state_all(void);

/* Pin function map — see SCHEMA[] / pin_fn_t in app_config.c.
 *
 * `app_config_pins_for("reed", out, 4)` writes up to 4 GPIO numbers
 * assigned to function "reed" into `out` and returns the actual count
 * (capped at out_max). Pass NULL/0 to just probe the count. The order
 * is stable: pin slot D0 < D1 < … < D7.
 *
 * `app_config_pin_for_first("reed")` is the convenience for singleton
 * functions — returns the first matching GPIO or -1 if unassigned.
 *
 * Callers usually invoke these once at module init; the pin map only
 * changes across a reboot so caching the result for the lifetime of
 * the boot is fine. */
size_t   app_config_pins_for(const char *fn_label, int *out, size_t out_max);
int      app_config_pin_for_first(const char *fn_label);

/* Slot introspection — for status pages enumerating the full D0..D7 row.
 * slot ∈ [0..7]. *gpio_out gets the ESP32-S3 GPIO for that slot.
 * *fn_label_out gets a stable pointer to the function label (e.g. "reed",
 * "uart_tx", "none"). Returns false if slot is out of range. */
bool     app_config_pin_slot_info(int slot, int *gpio_out, const char **fn_label_out);
int      app_config_pin_slot_count(void);

#ifdef __cplusplus
}
#endif
