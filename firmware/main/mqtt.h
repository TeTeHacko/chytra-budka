/* mqtt.h — MQTT client (HA telemetry + auto-discovery) on top of esp-mqtt. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ble_parse.h"   /* ble_bthome_reading_t for mqtt_publish_bthome() */

#ifdef __cplusplus
extern "C" {
#endif

bool mqtt_init(void);

/* True after MQTT broker handshake completed successfully. */
bool mqtt_is_connected(void);

/* True when the client was started WITHOUT an mTLS identity but the store has
 * since gained a clientAuth cert — i.e. enrollment finished after mqtt_init()
 * cached its verdict. The client keeps offering no certificate and the broker
 * keeps rejecting it ("peer did not return a certificate") until something
 * re-runs mqtt_init, so the caller is expected to reboot. Only ever true on
 * the boot where enrollment first succeeded, so it cannot loop.
 * Call from the main task: the X.509 parse needs more stack than mqtt_task. */
bool mqtt_client_identity_is_stale(void);

/* Idempotent: republished automatically on every reconnect. Safe to call
 * manually too. No-op when disconnected. */
void mqtt_publish_discovery(void);
void mqtt_publish_availability(bool online);

/* Publish a short WiFi status line to <id>/state/wifi (non-retained).
 * Carries a status string + the active SSID only — the WiFi password is
 * NEVER published. Called from the credential verify path in app_main. */
void mqtt_publish_wifi_status(const char *status);

/* Retained, redacted endpoint diagnostics on <id>/state/net (tier, broker
 * host, auth mode, OTA/relay hosts — never credentials). Republished on
 * connect and after cmd/endpoint changes. */
void mqtt_publish_net_state(void);

/* Telemetry publishers — no-ops when disconnected. */
void mqtt_publish_profile(const char *profile);
/* Hibernate sleeping marker (retained) so HA tells intentional deep sleep from a
 * fault. Published just before esp_deep_sleep_start() by cb_ds. */
void mqtt_publish_ds_state(bool sleeping, int next_wake_s, uint32_t wake_count,
                           const char *reason, bool reed_wake);
/* True when the MQTT client outbox is empty (or disconnected) — the drain
 * signal cb_ds waits on before deep sleep. */
bool mqtt_outbox_empty(void);
/* Mic pipeline on/off (profile + audio active-hours window). Published on change. */
void mqtt_publish_audio_active(bool on);
void mqtt_publish_telemetry(float soc, float vbat, float crate, int rssi, float temp_c,
                            float humidity_pct);

/* External SHT41 (bit-bang bus1) telemetry. Skips publish on NaN
 * sentinels — the operator sees "unknown" in HA rather than a stale
 * value when the sensor is absent or a read failed. */
void mqtt_publish_telemetry_ext(float temp_c, float humidity_pct);

/* Publish all physical I²C sensor channels (from the sensor registry in
 * sensors.[ch]) to their state/<obj> topics. Reads the per-sensor caches —
 * call cb_sensors_refresh() first (the telemetry tick does). Replaces the
 * old per-sensor temp/humidity[_ext] publishes. */
void mqtt_publish_sensors(void);

void mqtt_publish_diag(uint32_t heap_free, uint32_t uptime_s, const char *reset_reason,
                       float mcu_temp_c);

/* Publish firmware identity (version, project_name, build date/time,
 * idf_ver, app_elf_sha256) as JSON to <id>/state/fw_version, retained
 * QoS 1. Call once after the first MQTT connect — the payload is
 * static for the lifetime of the running image. The matching HA
 * discovery entity (sensor fw_version) exposes the version string as
 * state and the rest as attributes. */
void mqtt_publish_fw_version(void);
void mqtt_publish_audio_telemetry(float rms_dbfs, uint32_t burst_count, uint32_t chunks_sent,
                                  bool streaming);
/* Publishes a JSON VAD-trigger event to <id>/event/triggered. The
 * function reads the current wall-clock time internally — when SNTP
 * has synced the payload carries `"ts":<epoch_ms>`, otherwise it
 * omits `ts` rather than emitting a bogus pre-sync uptime that HA
 * can't translate back to real time. */
void mqtt_publish_triggered(float rms_dbfs);

/* Photo event metadata: published as JSON {"seq":N,"size":<bytes>,
 * "trigger":"…","path":"/sdcard/…","url":"http://<ip>/photo?f=…",
 * "agc":<gain>,"ir":<0|1>,"framesize":<enum>,"quality":<q>}. agc=-1 means
 * not measured. framesize/quality are the actual sensor values at capture
 * time — capturing during an active MJPEG stream means the shot used
 * stream settings, not capture settings; this field lets HA correlate.
 * Raw JPEG bytes go separately via mqtt_publish_photo_image. */
void mqtt_publish_photo_event(const uint8_t *jpeg_buf, size_t jpeg_len, const char *trigger,
                              const char *sd_path,
                              int agc_gain, bool ir_active,
                              int framesize, int quality);

/* Ambient AGC sample published outside the capture path so the operator
 * can see the gain curve through the day even when nothing has triggered.
 * Retained on <base>/state/ambient_agc. Cheap (single SCCB refresh) but
 * not free — call from the telemetry tick, not the hot loop. */
void mqtt_publish_ambient_agc(int agc_gain);

/* Push raw JPEG bytes to the MQTT image topic (HA mqtt camera
 * platform reads this). QoS 1 retained so HA still has the last shot
 * after a restart and a brief broker hiccup doesn't drop the image
 * silently. */
void mqtt_publish_photo_image(const uint8_t *jpeg_buf, size_t jpeg_len);

/* Cumulative count of mqtt_publish_photo_image calls that returned
 * rc<0 (oversize payload, disconnect mid-call, outbox saturated).
 * Surfaced in selftest JSON; without this counter an undeliverable
 * frame just drop-oldest's through photo_queue after 8 retries with
 * no visible signal. */
uint32_t mqtt_photo_publish_errors_total(void);

/* Returns true once when a `cmd/snapshot` message was received. Resets flag. */
bool mqtt_snapshot_requested(void);

/* Returns true once when a `cmd/photo` message was received. Resets flag. */
bool mqtt_photo_requested(void);

/* Motion publishers — for AM312 PIR. */
void mqtt_publish_motion(bool active);
void mqtt_publish_motion_count(uint32_t count);
/* Per-instance variants for multi-PIR boards. Instance 0 maps to the
 * singleton topic (state/motion, state/motion_count) for backward
 * compat; instances 1+ use state/motion_<n> + state/motion_count_<n>. */
void mqtt_publish_motion_nth(int idx, bool active);
void mqtt_publish_motion_count_nth(int idx, uint32_t count);

/* Reed switch (door/lid contact). closed=true → ON (door open in HA
 * binary_sensor terms, since HA's "door" device class uses ON for
 * the active/triggered state which we interpret as "closed magnet
 * contact"). count is monotonic across boots within a session. */
void mqtt_publish_reed(bool closed);
void mqtt_publish_reed_count(uint32_t count);
/* Per-instance variants for multi-reed boards. Instance 0 maps to the
 * singleton topic (state/reed, state/reed_count) for backward compat
 * with HA recorder history; instances 1+ use state/reed_<n> + state/
 * reed_count_<n>. Callers are reed.c (publishes per arming) and
 * main.cpp's tick consumer (publishes on each debounced state change). */
void mqtt_publish_reed_nth(int idx, bool closed);
void mqtt_publish_reed_count_nth(int idx, uint32_t count);

/* Grove ultrasonic ranger distance (cm) → state/distance_cm. */
void mqtt_publish_distance(float cm);
/* Grove soil moisture: raw mV → state/soil_mv (always when finite),
 * calibrated percent → state/soil_moist (skipped when NAN). */
void mqtt_publish_soil(float mv, float pct);

/* Solar power monitor (INA226). NaN values are skipped. */
void mqtt_publish_solar(float bus_v, float current_a, float power_w);

/* External BLE UC96 power meters, keyed by MAC id (no colons). Several may be
 * present (field daisy-chains two), each published under <base>/meter/<id>/….
 * NaN values are skipped. _discovery() announces the meter's HA entities, sent
 * once when a meter starts streaming. */
void mqtt_publish_uc96(const char *id, float v, float i, float p, float wh, int temp_c);
void mqtt_publish_uc96_discovery(const char *id);

/* BTHome v2 passive sensors read over BLE (allowlisted only), keyed by MAC id,
 * published under <base>/sensor/<id>/…. Only present fields are emitted.
 * _discovery() announces the present sensors' HA entities (once per device). */
void mqtt_publish_bthome(const char *id, const ble_bthome_reading_t *r);
void mqtt_publish_bthome_discovery(const char *id, const ble_bthome_reading_t *r);

/* Self-test summary, JSON payload. Published to <device_id>/diag/selftest. */
void mqtt_publish_selftest(const char *json);

/* Topic + HA discovery building blocks shared with app_config.c. All return
 * pointers into static buffers populated by build_topics() in mqtt_init();
 * callers must not call these before mqtt_init() runs. */
const char *mqtt_topic_base(void);         /* e.g. "cb-ex01" */
const char *mqtt_topic_availability(void); /* full availability topic    */
const char *mqtt_avail_block(void);        /* JSON snippet for HA avty_t */
const char *mqtt_device_block(void);       /* JSON snippet for HA dev    */

/* TLS enrollment one-shot helper. Publishes CSR PEM on
 * <base>/cmd/enroll, subscribes to <base>/state/cert, blocks until a
 * payload arrives (or timeout expires), copies it into out_cert_buf
 * NUL-terminated, then unsubscribes. Returns:
 *   ESP_OK            — got cert, *out_cert_len set
 *   ESP_ERR_TIMEOUT   — no reply within timeout_ms
 *   ESP_ERR_INVALID_STATE — MQTT not connected
 *   ESP_ERR_NO_MEM    — response too big for out_cert_buf
 *   ESP_FAIL          — publish/subscribe failed
 * Single-flight: a second concurrent caller gets ESP_ERR_INVALID_STATE. */
esp_err_t mqtt_enroll_request(const char *csr_pem, size_t csr_len,
                               uint8_t *out_cert_buf, size_t out_cert_buf_sz,
                               size_t *out_cert_len,
                               uint32_t timeout_ms);

/* Publish an enrollment progress payload (JSON) retained to
 * <id>/state/enroll — requested/pending/issued/denied/failed, emitted by
 * the tls_enroll pipeline. Safe no-op while MQTT is disconnected (the
 * HTTPS transport runs without a broker session; progress is best-effort
 * observability, not a protocol step). */
void mqtt_publish_enroll_state(const char *json);

#ifdef __cplusplus
}
#endif
