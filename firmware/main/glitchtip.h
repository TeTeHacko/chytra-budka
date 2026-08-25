/* glitchtip.h — minimal Sentry/GlitchTip error reporter for ESP-IDF.
 *
 * Implements just enough of the Sentry envelope protocol to ship
 * crash boot events and ESP_LOGE/W lines to a self-hosted GlitchTip
 * instance. DSN is read from secrets.h at boot.
 *
 * Architecture: a small FreeRTOS queue feeds a dedicated task that
 * formats events into a Sentry envelope and POSTs to <host>/api/<pid>/
 * envelope/. The log hook is non-blocking — if the queue overflows
 * (more than 16 pending events) the oldest is dropped silently. The
 * task uses esp_http_client with the embedded LE cert bundle so it
 * works against any server with a Let's Encrypt cert.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize. Must be called once WiFi/SNTP are up — we need TLS and
 * a wall-clock for the event timestamps. Idempotent. Returns false
 * if the DSN is unparseable. */
bool glitchtip_init(void);

bool glitchtip_ready(void);

/* Submit an event. level ∈ {"fatal","error","warning","info","debug"}.
 * `extra_fields_json` is appended verbatim before the closing "}" of
 * the event JSON, so the caller can add ,"tags":{...} or ,"contexts":
 * {...} — pass NULL for none. Non-blocking; returns false if the
 * queue is full. */
bool glitchtip_report(const char *level, const char *message,
                      const char *extra_fields_json);

/* Convenience: send a "fatal" event describing a crash boot. Should
 * be called from diag once the reset reason is known and MQTT diag
 * is already sent (so we don't slow the boot path). */
bool glitchtip_report_crash_boot(const char *reset_reason,
                                 uint32_t consecutive_crashes,
                                 size_t coredump_bytes);

/* Install the ESP-IDF log hook so ESP_LOGE / ESP_LOGW lines get
 * forwarded automatically. Original logger output is preserved. */
void glitchtip_install_log_hook(void);

#ifdef __cplusplus
}
#endif
