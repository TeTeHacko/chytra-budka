/* mqtt.c — esp-mqtt client.
 *
 * Topic layout is per-device, rooted at device_id() (= HOSTNAME-<mactail>),
 * so two boards sharing a broker don't stomp on each other's retained
 * state or HA discovery. Example for MAC aa:bb:cc:dd:ee:01:
 *
 *   cb-ex01/state/mode          : "Boot"|"Safe"|"Triggered"|"Continuous"
 *   cb-ex01/state/soc           : float, %
 *   cb-ex01/state/availability  : "online"|"offline" (LWT)
 *   cb-ex01/event/triggered     : JSON {"ts":<ms>,"rms":<dBFS>}
 *   cb-ex01/cmd/snapshot        : subscribed, sets snapshot flag
 *
 * HA discovery topics: homeassistant/<component>/<device_id>/<obj>/config
 * MQTT client_id     : device_id()
 * HA device.ids      : [device_id()] — registered as one HA device per board
 */
#include "mqtt.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "config.h"
#include "diag.h"
#include "http_server.h"
#include "pir.h"
#include "reed.h"
#include "device_id.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "ota.h"
#include "oled.h"
#include "secret_helpers.h"
#include "audiofx.h"
#include "melody.h"
#include "pcm.h"
#include "battery.h"
#include "sensors.h"
#include "speaker.h"
#include "status_led.h"
#include "tls_store.h"
#include "wifi_mgr.h"
#include "wifi_store.h"
#include "auth_store.h"
#include "flat_json.h"
#include "net_store.h"
#include "tls_enroll.h"
#include "esp_crt_bundle.h"

static const char *TAG = "mqtt";

/* Runtime topics, populated by build_topics() at init from device_id(). */
static struct {
    char base[40];
    char avail[64];
    char profile[64];
    char ds[64];
    char soc[64];
    char vbat[64];
    char rssi[64];
    char temp[64];
    char humidity[64];
    char temp_ext[64];
    char humidity_ext[64];
    char crate[64];
    char heap[64];
    char uptime[64];
    char reset_reason[64];
    char rms[64];
    char burst[64];
    char chunks[64];
    char streaming[64];
    char audio_active[64];
    char triggered[64];
    char photo_event[64];
    char capture_count[64];
    char cmd_snapshot[64];
    char cmd_photo[64];
    char cmd_reboot[64];
    char cmd_ota[64];
    char cmd_wifi[64];
    char cmd_auth[64];
    char wifi_state[64];
    char cmd_cfg_reset[64];
    char cmd_factory_reset[64];
    char cmd_oled_logo[64];
    char oled_logo_dump[64];
    char cmd_beep[64];
    char cmd_melody[64];
    char cmd_alarm[64];
    char cmd_sfx[64];
    char cmd_pcm[64];
    char cmd_endpoint[64];
    char endpoint_state[64];
    char net_state[64];
    char cmd_cert[64];
    char enroll_state[64];
    char cmd_cfg_pfx[64];
    char motion[64];
    char motion_count[64];
    char reed[64];
    char reed_count[64];
    char solar_v[64];
    char solar_i[64];
    char solar_p[64];
    char distance[64];
    char soil_moist[64];
    char soil_mv[64];
    char selftest[64];
    char image_photo[64];
    char mcu_temp[64];
    char fw_version[64];
    char ambient_agc[64];
} T;

/* HA discovery shared blocks — built once at init, after T. */
static char s_avail_block[128];
static char s_device_block[256];

/* Compose <id><suffix> into `buf` (size 64 per the T struct fields).
 * Logs ESP_LOGE if truncation would occur — better than a silent
 * wrong-topic publish if the device_id or a suffix ever grows past
 * the implicit budget. sizeof(field) is always 64 in T; passing it
 * explicitly keeps this generic if a single field is ever resized. */
static void build_topic(char *buf, size_t buf_sz, const char *id, const char *suffix) {
    int n = snprintf(buf, buf_sz, "%s%s", id, suffix);
    if (n < 0 || (size_t)n >= buf_sz) {
        ESP_LOGE(TAG, "topic truncated: '%s%s' (need %d, have %u)",
                 id, suffix, n, (unsigned)buf_sz);
    }
}

#define BT(field, suffix) build_topic(T.field, sizeof(T.field), id, suffix)

static void build_topics(void) {
    const char *id = device_id();
    snprintf(T.base, sizeof(T.base), "%s", id);
    BT(avail,         "/state/availability");
    BT(profile,       "/state/profile");
    BT(ds,            "/state/ds");
    BT(soc,           "/state/soc");
    BT(vbat,          "/state/v_bat");
    BT(rssi,          "/state/rssi");
    BT(temp,          "/state/temp");
    BT(humidity,      "/state/humidity");
    BT(temp_ext,      "/state/temp_ext");
    BT(humidity_ext,  "/state/humidity_ext");
    BT(crate,         "/state/crate");
    BT(heap,          "/state/heap_free");
    BT(uptime,        "/state/uptime_s");
    BT(reset_reason,  "/state/reset_reason");
    BT(rms,           "/state/rms_dbfs");
    BT(burst,         "/state/burst_count");
    BT(chunks,        "/state/chunks_sent");
    BT(streaming,     "/state/streaming");
    BT(audio_active,  "/state/audio_active");
    BT(triggered,     "/event/triggered");
    BT(photo_event,   "/event/photo");
    BT(capture_count, "/state/capture_count");
    BT(cmd_snapshot,  "/cmd/snapshot");
    BT(cmd_photo,     "/cmd/photo");
    BT(cmd_reboot,    "/cmd/reboot");
    BT(cmd_ota,       "/cmd/ota");
    BT(cmd_wifi,      "/cmd/wifi");
    BT(cmd_auth,      "/cmd/auth");
    BT(wifi_state,    "/state/wifi");
    BT(cmd_cfg_reset,     "/cmd/cfg_reset");
    BT(cmd_factory_reset, "/cmd/factory_reset");
    BT(cmd_oled_logo,     "/cmd/oled_logo");
    BT(oled_logo_dump,    "/oled/logo_dump");
    BT(cmd_beep,          "/cmd/beep");
    BT(cmd_melody,        "/cmd/melody");
    BT(cmd_alarm,         "/cmd/alarm");
    BT(cmd_sfx,           "/cmd/sfx");
    BT(cmd_pcm,           "/cmd/pcm");
    BT(cmd_endpoint,      "/cmd/endpoint");
    BT(endpoint_state,    "/state/endpoint");
    BT(net_state,         "/state/net");
    BT(cmd_cert,          "/cmd/cert");
    BT(enroll_state,      "/state/enroll");
    BT(cmd_cfg_pfx,   "/cmd/cfg/");
    BT(motion,        "/state/motion");
    BT(motion_count,  "/state/motion_count");
    BT(reed,          "/state/reed");
    BT(reed_count,    "/state/reed_count");
    BT(distance,      "/state/distance_cm");
    BT(soil_moist,    "/state/soil_moist");
    BT(soil_mv,       "/state/soil_mv");
    BT(solar_v,       "/state/solar_v");
    BT(solar_i,       "/state/solar_i");
    BT(solar_p,       "/state/solar_p");
    BT(selftest,      "/diag/selftest");
    BT(image_photo,   "/image/photo");
    BT(mcu_temp,      "/state/mcu_temp");
    BT(ambient_agc,   "/state/ambient_agc");
    BT(fw_version,    "/state/fw_version");

    snprintf(s_avail_block, sizeof(s_avail_block),
             "\"avty_t\":\"%s\","
             "\"pl_avail\":\"online\","
             "\"pl_not_avail\":\"offline\"",
             T.avail);
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(s_device_block, sizeof(s_device_block),
             "\"dev\":{"
             "\"ids\":[\"%s\"],"
             "\"name\":\"Chytrá Budka %s\","
             "\"mf\":\"DIY\","
             "\"mdl\":\"XIAO ESP32-S3 Sense + PDM mic + OV3660\","
             "\"sw\":\"%s (%s)\""
             "}",
             id, device_id_suffix(), app ? app->version : "?", app ? app->date : "?");
}
#undef BT

/* Exposed for app_config.c so its discovery payloads share the same
 * topic root + entity blocks. */
const char *mqtt_topic_base(void) {
    return T.base;
}
const char *mqtt_topic_availability(void) {
    return T.avail;
}
const char *mqtt_avail_block(void) {
    return s_avail_block;
}
const char *mqtt_device_block(void) {
    return s_device_block;
}

static esp_mqtt_client_handle_t s_client;
static atomic_bool s_connected = false;
static atomic_bool s_snapshot = false;
static atomic_bool s_photo_req = false;

/* TLS enrollment one-shot state. The publish + wait helper sets up
 * s_enroll_*, subscribes to the cert topic, publishes the CSR, then
 * blocks on s_enroll_sem until MQTT_EVENT_DATA fills the buffer and
 * gives the semaphore.
 *
 * Concurrency: the helper runs on the caller's task while the MQTT event
 * task fills the buffer + gives the semaphore — two tasks. The semaphore is
 * created ONCE in mqtt_init() and NEVER deleted (a previous version created +
 * vSemaphoreDelete'd it per request, which let the event task give/touch a
 * freed handle on a reply that landed right at the timeout boundary — a UAF
 * triggered by exactly the slow-link case enrollment retries exist for).
 * s_enroll_lock serialises every access to the s_enroll_* state between the
 * two tasks; s_enroll_active (under the lock) is the single-flight busy flag.
 * The helper drains a stale give before arming and clears s_enroll_active on
 * teardown so a late event is ignored rather than corrupting the next round. */
static SemaphoreHandle_t s_enroll_sem      = NULL;  /* persistent; never deleted */
static SemaphoreHandle_t s_enroll_lock     = NULL;  /* guards s_enroll_* state */
static bool              s_enroll_active   = false; /* in-flight (under lock) */
static uint8_t          *s_enroll_buf      = NULL;
static size_t            s_enroll_buf_sz   = 0;
static size_t            s_enroll_buf_len  = 0;
static char              s_enroll_topic[64] = {0};
static esp_err_t         s_enroll_result   = ESP_OK;

static void pub_discovery_one(const char *comp, const char *obj, const char *body_fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void pub_discovery_one(const char *comp, const char *obj, const char *body_fmt, ...) {
    char topic[160];
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", comp, device_id(), obj);

    /* body[480] + payload[640] live on the HEAP, not the stack. This is called
     * not only from the MQTT event task (~6 KB stack) on connect, but also from
     * the NimBLE host task (only 4 KB) when a UC96 meter subscribes / a BTHome
     * advert arrives (mqtt_publish_uc96_discovery / _bthome_discovery → here).
     * 1.1 KB of stack buffers on top of NimBLE's own deep GATT-callback frames
     * risked a stack overflow exactly when a meter connects. Heap keeps the
     * frame tiny; freed before return, so no growth. */
    char *body = malloc(480);
    char *payload = malloc(640);
    if (!body || !payload) {
        ESP_LOGW(TAG, "discovery alloc failed for %s", obj);
        free(body);
        free(payload);
        return;
    }

    va_list ap;
    va_start(ap, body_fmt);
    int bn = vsnprintf(body, 480, body_fmt, ap);
    va_end(ap);
    if (bn <= 0 || bn >= 480) {
        ESP_LOGW(TAG, "discovery body truncated for %s (%d)", obj, bn);
        goto out;
    }

    int n = snprintf(payload, 640, "{%s,\"uniq_id\":\"%s_%s\",%s,%s}", body,
                     device_id(), obj, s_avail_block, s_device_block);
    if (n <= 0 || n >= 640) {
        ESP_LOGW(TAG, "discovery payload truncated for %s (%d)", obj, n);
        goto out;
    }
    if (esp_mqtt_client_publish(s_client, topic, payload, n, /*qos*/ 0, /*retain*/ 1) < 0)
        ESP_LOGW(TAG, "discovery publish failed for %s", obj);
out:
    free(body);
    free(payload);
}

void mqtt_publish_discovery(void) {
    if (!atomic_load(&s_connected))
        return;

    /* Read-only state sensor showing the resolved power tier (distinct from the
     * power_profile config select). Renamed from the old "mode" sensor — unpublish
     * the orphan so HA drops it. */
    {
        char dead[160];
        snprintf(dead, sizeof(dead), "homeassistant/sensor/%s/mode/config", device_id());
        esp_mqtt_client_publish(s_client, dead, "", 0, /*qos*/ 0, /*retain*/ 1);
    }
    pub_discovery_one("sensor", "profile",
                      "\"name\":\"Power profile\","
                      "\"stat_t\":\"%s\","
                      "\"icon\":\"mdi:state-machine\"",
                      T.profile);

    pub_discovery_one("sensor", "soc",
                      "\"name\":\"Battery SOC\","
                      "\"stat_t\":\"%s\","
                      "\"unit_of_meas\":\"%%\","
                      "\"dev_cla\":\"battery\","
                      "\"stat_cla\":\"measurement\"",
                      T.soc);

    pub_discovery_one("sensor", "v_bat",
                      "\"name\":\"Battery Voltage\","
                      "\"stat_t\":\"%s\","
                      "\"unit_of_meas\":\"V\","
                      "\"dev_cla\":\"voltage\","
                      "\"stat_cla\":\"measurement\"",
                      T.vbat);

    pub_discovery_one("sensor", "rssi",
                      "\"name\":\"WiFi RSSI\","
                      "\"stat_t\":\"%s\","
                      "\"unit_of_meas\":\"dBm\","
                      "\"dev_cla\":\"signal_strength\","
                      "\"stat_cla\":\"measurement\","
                      "\"ent_cat\":\"diagnostic\"",
                      T.rssi);

    /* Physical I²C sensors (SHT41×N, BMP388, …): discovery generated from
     * the shared registry, so adding a sensor needs zero MQTT changes. The
     * `obj` ids are stable (they double as the state-topic suffix), so the
     * historic "temp"/"humidity"/"temp_ext"/"humidity_ext" entities keep
     * their HA identity; BMP388 adds "temp_bmp" + "pressure". */
    for (size_t si = 0; si < CB_SENSORS_N; si++) {
        const cb_sensor_t *sen = &CB_SENSORS[si];
        if (!sen->mqtt)
            continue;  /* battery + bus1 quarantine MAX: dedicated path / no MQTT */
        for (size_t ci = 0; ci < sen->n_chans; ci++) {
            const cb_chan_t *c = &sen->chans[ci];
            char stat[160], body[320];
            snprintf(stat, sizeof(stat), "%s/state/%s", device_id(), c->obj);
            int bn = snprintf(body, sizeof(body),
                              "\"name\":\"%s\",\"stat_t\":\"%s\","
                              "\"unit_of_meas\":\"%s\",%s%s%s"
                              "\"stat_cla\":\"measurement\"",
                              c->name, stat, c->unit,
                              c->dev_cla ? "\"dev_cla\":\"" : "",
                              c->dev_cla ? c->dev_cla : "",
                              c->dev_cla ? "\"," : "");
            if (bn > 0 && bn < (int)sizeof(body))
                pub_discovery_one("sensor", c->obj, "%s", body);
        }
    }

    pub_discovery_one("sensor", "crate",
                      "\"name\":\"Charge Rate\","
                      "\"stat_t\":\"%s\","
                      "\"unit_of_meas\":\"%%/h\","
                      "\"stat_cla\":\"measurement\","
                      "\"icon\":\"mdi:battery-charging\"",
                      T.crate);

    pub_discovery_one("sensor", "heap_free",
                      "\"name\":\"Free Heap\","
                      "\"stat_t\":\"%s\","
                      "\"unit_of_meas\":\"B\","
                      "\"stat_cla\":\"measurement\","
                      "\"icon\":\"mdi:memory\","
                      "\"ent_cat\":\"diagnostic\"",
                      T.heap);

    pub_discovery_one("sensor", "uptime",
                      "\"name\":\"Uptime\","
                      "\"stat_t\":\"%s\","
                      "\"unit_of_meas\":\"s\","
                      "\"dev_cla\":\"duration\","
                      "\"stat_cla\":\"total_increasing\","
                      "\"ent_cat\":\"diagnostic\"",
                      T.uptime);

    pub_discovery_one("sensor", "reset_reason",
                      "\"name\":\"Reset Reason\","
                      "\"stat_t\":\"%s\","
                      "\"icon\":\"mdi:restart\","
                      "\"ent_cat\":\"diagnostic\"",
                      T.reset_reason);

    pub_discovery_one("sensor", "mcu_temp",
                      "\"name\":\"MCU Temperature\","
                      "\"stat_t\":\"%s\","
                      "\"unit_of_meas\":\"°C\","
                      "\"dev_cla\":\"temperature\","
                      "\"stat_cla\":\"measurement\","
                      "\"icon\":\"mdi:chip\","
                      "\"ent_cat\":\"diagnostic\"",
                      T.mcu_temp);

    /* Firmware identity — state = version string; build date/time,
     * project_name, idf_ver, app_elf_sha256 surface as attributes. */
    pub_discovery_one("sensor", "fw_version",
                      "\"name\":\"Firmware Version\","
                      "\"stat_t\":\"%s\","
                      "\"val_tpl\":\"{{ value_json.version }}\","
                      "\"json_attr_t\":\"%s\","
                      "\"icon\":\"mdi:tag\","
                      "\"ent_cat\":\"diagnostic\"",
                      T.fw_version, T.fw_version);

    pub_discovery_one("sensor", "rms_dbfs",
                      "\"name\":\"Audio RMS\","
                      "\"stat_t\":\"%s\","
                      "\"unit_of_meas\":\"dBFS\","
                      "\"stat_cla\":\"measurement\","
                      "\"icon\":\"mdi:waveform\","
                      "\"ent_cat\":\"diagnostic\"",
                      T.rms);

    /* Ambient AGC: sampled in the telemetry tick, retained so HA shows
     * the last value even when the board is asleep. Useful for tuning
     * ir_agc_thresh — graph this over 24h to see the dusk/dawn curve. */
    pub_discovery_one("sensor", "ambient_agc",
                      "\"name\":\"Camera AGC\","
                      "\"stat_t\":\"%s\","
                      "\"stat_cla\":\"measurement\","
                      "\"icon\":\"mdi:weather-night\","
                      "\"ent_cat\":\"diagnostic\"",
                      T.ambient_agc);

    pub_discovery_one("sensor", "burst_count",
                      "\"name\":\"Burst Count\","
                      "\"stat_t\":\"%s\","
                      "\"stat_cla\":\"total_increasing\","
                      "\"icon\":\"mdi:counter\","
                      "\"ent_cat\":\"diagnostic\"",
                      T.burst);

    pub_discovery_one("sensor", "chunks_sent",
                      "\"name\":\"Audio Chunks Sent\","
                      "\"stat_t\":\"%s\","
                      "\"stat_cla\":\"total_increasing\","
                      "\"icon\":\"mdi:upload\","
                      "\"ent_cat\":\"diagnostic\"",
                      T.chunks);

    pub_discovery_one("binary_sensor", "streaming",
                      "\"name\":\"Streaming\","
                      "\"stat_t\":\"%s\","
                      "\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
                      "\"dev_cla\":\"running\"",
                      T.streaming);

    pub_discovery_one("binary_sensor", "audio_active",
                      "\"name\":\"Audio active\","
                      "\"stat_t\":\"%s\","
                      "\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
                      "\"dev_cla\":\"running\","
                      "\"icon\":\"mdi:microphone\"",
                      T.audio_active);

    pub_discovery_one("sensor", "last_trigger",
                      "\"name\":\"Last Trigger RMS\","
                      "\"stat_t\":\"%s\","
                      "\"unit_of_meas\":\"dBFS\","
                      "\"val_tpl\":\"{{ value_json.rms }}\","
                      "\"json_attr_t\":\"%s\","
                      "\"icon\":\"mdi:bird\"",
                      T.triggered, T.triggered);

    pub_discovery_one("sensor", "capture_count",
                      "\"name\":\"Photo Captures\","
                      "\"stat_t\":\"%s\","
                      "\"stat_cla\":\"total_increasing\","
                      "\"icon\":\"mdi:camera\","
                      "\"ent_cat\":\"diagnostic\"",
                      T.capture_count);

    pub_discovery_one("sensor", "last_photo",
                      "\"name\":\"Last Photo\","
                      "\"stat_t\":\"%s\","
                      "\"val_tpl\":\"{{ value_json.trigger }}\","
                      "\"json_attr_t\":\"%s\","
                      "\"icon\":\"mdi:image\"",
                      T.photo_event, T.photo_event);

    /* PIR motion — multi-instance like reed. Instance 0 keeps the
     * singleton-era discovery + topic for backward compat. */
    int pir_n = pir_active_count();
    if (pir_n < 1) pir_n = 1;  /* always register at least instance 0 */
    for (int i = 0; i < pir_n; i++) {
        char obj[24], name[40];
        char stat_topic[160], count_topic[160];
        if (i == 0) {
            snprintf(obj, sizeof(obj), "motion");
            snprintf(name, sizeof(name), "Motion");
            snprintf(stat_topic, sizeof(stat_topic), "%s", T.motion);
            snprintf(count_topic, sizeof(count_topic), "%s", T.motion_count);
        } else {
            snprintf(obj, sizeof(obj), "motion_%d", i);
            snprintf(name, sizeof(name), "Motion %d", i);
            snprintf(stat_topic, sizeof(stat_topic), "%s_%d", T.motion, i);
            snprintf(count_topic, sizeof(count_topic),
                     "%s/state/motion_count_%d", mqtt_topic_base(), i);
        }
        pub_discovery_one("binary_sensor", obj,
                          "\"name\":\"%s\","
                          "\"stat_t\":\"%s\","
                          "\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
                          "\"dev_cla\":\"motion\"",
                          name, stat_topic);
        char count_obj[32], count_name[48];
        if (i == 0) {
            snprintf(count_obj, sizeof(count_obj), "motion_count");
            snprintf(count_name, sizeof(count_name), "Motion Events");
        } else {
            snprintf(count_obj, sizeof(count_obj), "motion_count_%d", i);
            snprintf(count_name, sizeof(count_name), "Motion Events %d", i);
        }
        pub_discovery_one("sensor", count_obj,
                          "\"name\":\"%s\","
                          "\"stat_t\":\"%s\","
                          "\"stat_cla\":\"total_increasing\","
                          "\"icon\":\"mdi:motion-sensor\","
                          "\"ent_cat\":\"diagnostic\"",
                          count_name, count_topic);
    }

    /* Reed switch (optional door/lid contact). dev_cla=door, where HA
     * convention is ON=open / OFF=closed — we map closed-contact (magnet
     * present) to "OFF" so the displayed semantics match the physical
     * door state.
     *
     * Multi-instance: instance 0 keeps the singleton-era discovery
     * (object="reed", topic state/reed, name "Door / Lid") so existing
     * HA entities + recorder history survive the migration. Instances
     * 1..reed_active_count()-1 publish with `_N` suffixes. We always
     * publish AT LEAST instance 0 so a board that boots with reed
     * disabled still gets the entity registered — HA will show
     * "unavailable" until the first publish, same as the singleton era. */
    int reed_n = reed_active_count();
    if (reed_n < 1) reed_n = 1;
    for (int i = 0; i < reed_n; i++) {
        char obj[24], name[40];
        char stat_topic[160], count_topic[160];
        if (i == 0) {
            snprintf(obj, sizeof(obj), "reed");
            snprintf(name, sizeof(name), "Door / Lid");
            snprintf(stat_topic, sizeof(stat_topic), "%s", T.reed);
            snprintf(count_topic, sizeof(count_topic), "%s", T.reed_count);
        } else {
            snprintf(obj, sizeof(obj), "reed_%d", i);
            snprintf(name, sizeof(name), "Door / Lid %d", i);
            snprintf(stat_topic, sizeof(stat_topic), "%s_%d", T.reed, i);
            snprintf(count_topic, sizeof(count_topic),
                     "%s/state/reed_count_%d", mqtt_topic_base(), i);
        }
        pub_discovery_one("binary_sensor", obj,
                          "\"name\":\"%s\","
                          "\"stat_t\":\"%s\","
                          "\"pl_on\":\"OPEN\",\"pl_off\":\"CLOSED\","
                          "\"dev_cla\":\"door\"",
                          name, stat_topic);

        char count_obj[32], count_name[48];
        if (i == 0) {
            snprintf(count_obj, sizeof(count_obj), "reed_count");
            snprintf(count_name, sizeof(count_name), "Reed Events");
        } else {
            snprintf(count_obj, sizeof(count_obj), "reed_count_%d", i);
            snprintf(count_name, sizeof(count_name), "Reed Events %d", i);
        }
        pub_discovery_one("sensor", count_obj,
                          "\"name\":\"%s\","
                          "\"stat_t\":\"%s\","
                          "\"stat_cla\":\"total_increasing\","
                          "\"icon\":\"mdi:door\","
                          "\"ent_cat\":\"diagnostic\"",
                          count_name, count_topic);
    }

    /* Grove ultrasonic + soil moisture — discovery gated on the enable
     * flags (unlike reed/motion instance 0 there's no legacy entity to
     * keep alive, and most boards never carry this hardware — an
     * always-registered entity would sit permanently "unknown" on all
     * of them). app_config's apply hook re-runs this whole function
     * when either flag flips ON, so the entity appears immediately. */
    if (app_config_get_bool("sonar_enabled")) {
        pub_discovery_one("sensor", "distance_cm",
                          "\"name\":\"Distance\","
                          "\"stat_t\":\"%s\","
                          "\"unit_of_meas\":\"cm\","
                          "\"dev_cla\":\"distance\","
                          "\"stat_cla\":\"measurement\","
                          "\"icon\":\"mdi:signal-distance-variant\"",
                          T.distance);
    }
    if (app_config_get_bool("soil_enabled")) {
        pub_discovery_one("sensor", "soil_moist",
                          "\"name\":\"Soil Moisture\","
                          "\"stat_t\":\"%s\","
                          "\"unit_of_meas\":\"%%\","
                          "\"dev_cla\":\"moisture\","
                          "\"stat_cla\":\"measurement\","
                          "\"icon\":\"mdi:water-percent\"",
                          T.soil_moist);
        /* Raw millivolts — the calibration source for soil_dry_mv /
         * soil_wet_mv, diagnostic-category so it doesn't clutter the
         * main dashboard. */
        pub_discovery_one("sensor", "soil_mv",
                          "\"name\":\"Soil Moisture (raw)\","
                          "\"stat_t\":\"%s\","
                          "\"unit_of_meas\":\"mV\","
                          "\"dev_cla\":\"voltage\","
                          "\"stat_cla\":\"measurement\","
                          "\"ent_cat\":\"diagnostic\","
                          "\"icon\":\"mdi:sine-wave\"",
                          T.soil_mv);
    }

    pub_discovery_one("sensor", "solar_v",
                      "\"name\":\"Solar Voltage\","
                      "\"stat_t\":\"%s\","
                      "\"unit_of_meas\":\"V\","
                      "\"dev_cla\":\"voltage\","
                      "\"stat_cla\":\"measurement\","
                      "\"icon\":\"mdi:solar-panel\"",
                      T.solar_v);

    pub_discovery_one("sensor", "solar_i",
                      "\"name\":\"Solar Current\","
                      "\"stat_t\":\"%s\","
                      "\"unit_of_meas\":\"A\","
                      "\"dev_cla\":\"current\","
                      "\"stat_cla\":\"measurement\","
                      "\"icon\":\"mdi:current-dc\"",
                      T.solar_i);

    pub_discovery_one("sensor", "solar_p",
                      "\"name\":\"Solar Power\","
                      "\"stat_t\":\"%s\","
                      "\"unit_of_meas\":\"W\","
                      "\"dev_cla\":\"power\","
                      "\"stat_cla\":\"measurement\","
                      "\"icon\":\"mdi:flash\"",
                      T.solar_p);

    pub_discovery_one("sensor", "selftest",
                      "\"name\":\"Self-test\","
                      "\"stat_t\":\"%s\","
                      "\"val_tpl\":\"{{ value_json.summary }}\","
                      "\"json_attr_t\":\"%s\","
                      "\"icon\":\"mdi:check-circle\","
                      "\"ent_cat\":\"diagnostic\"",
                      T.selftest, T.selftest);

    /* Camera entity — HA's mqtt camera platform subscribes to the
     * binary topic and renders each retained payload as the current
     * image. No URL fetch, no IP discovery required on the HA side. */
    pub_discovery_one("camera", "photo",
                      "\"name\":\"Camera\","
                      "\"t\":\"%s\","
                      "\"icon\":\"mdi:camera\"",
                      T.image_photo);

    /* Last-photo event sensor — exposes the metadata payload (seq,
     * size, trigger, path, url) as HA entity attributes so the user
     * can dashboard-tap an URL to fetch the full-res archive copy. */
    pub_discovery_one("sensor", "photo_event",
                      "\"name\":\"Last Photo\","
                      "\"stat_t\":\"%s\","
                      "\"val_tpl\":\"{{ value_json.trigger }}\","
                      "\"json_attr_t\":\"%s\","
                      "\"icon\":\"mdi:image-text\"",
                      T.photo_event, T.photo_event);
}

/* ─── publishers ────────────────────────────────────────────────────────── */
/* State topics (availability, mode, cfg/+) ship at QoS 1 so HA sees
 * the transition even if a keep-alive ACK gets dropped right when the
 * client disconnects. Telemetry stays at QoS 0 — losing a single
 * sample is fine and the next one is along in seconds. */
static void pub(const char *topic, const char *data, int qos, int retain) {
    if (!atomic_load(&s_connected))
        return;
    esp_mqtt_client_publish(s_client, topic, data, 0, qos, retain);
}

/* helpers exported for app_config.c */
void mqtt_pub_retained(const char *topic, const char *value) {
    pub(topic, value, /*qos*/ 1, /*retain*/ 1);
}
void mqtt_pub(const char *topic, const char *value) {
    pub(topic, value, /*qos*/ 0, /*retain*/ 0);
}
void mqtt_pub_discovery_raw(const char *topic, const char *payload, int len) {
    if (!atomic_load(&s_connected))
        return;
    /* Discovery is volumetric (one publish per entity on every connect)
     * and retained — QoS 0 keeps the broker fan-out cheap. */
    esp_mqtt_client_publish(s_client, topic, payload, len, 0, /*retain*/ 1);
}

/* Publish a WiFi status line to <id>/state/wifi (non-retained). The
 * payload carries a short status + the active SSID at most — the WiFi
 * PASSWORD IS NEVER PUBLISHED to any topic. */
void mqtt_publish_wifi_status(const char *status) {
    char msg[160];
    snprintf(msg, sizeof(msg), "{\"status\":\"%s\",\"ssid\":\"%s\"}",
             status ? status : "", wifi_mgr_get_ssid());
    mqtt_pub(T.wifi_state, msg);
}

/* Minimal flat-JSON helpers (no cJSON dependency — not bundled in IDF
 * v6.0.1). Sufficient for the flat {"ssid":..,"password":..,"reset":..}
 * payloads cmd/wifi accepts; NOT a general JSON parser. */

/* Extract a JSON string value: "<key>" : "<value>" → out (un-escaped:
 * \" \\ \/ \n \t \r). Returns true if the key+string was found. */
static bool json_str(const char *json, const char *key, char *out, size_t cap) {
    out[0] = 0;
    char pat[24];
    int pn = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pn <= 0 || pn >= (int)sizeof(pat)) return false;
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += pn;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[o++] = '\n'; break;
                case 't': out[o++] = '\t'; break;
                case 'r': out[o++] = '\r'; break;
                default:  out[o++] = *p;   break; /* \" \\ \/ → literal */
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return true;
}

/* True if "<key>" is present with boolean literal true. */
static bool json_bool_true(const char *json, const char *key) {
    char pat[24];
    int pn = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pn <= 0 || pn >= (int)sizeof(pat)) return false;
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += pn;
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') p++;
    return strncmp(p, "true", 4) == 0;
}

/* If "<key>" is present with a boolean literal, set *val and return true. */
static bool json_bool(const char *json, const char *key, bool *val) {
    char pat[24];
    int pn = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pn <= 0 || pn >= (int)sizeof(pat)) return false;
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += pn;
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') p++;
    if (strncmp(p, "true", 4) == 0)  { *val = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *val = false; return true; }
    return false;
}

/* Handle a cmd/wifi payload. Two forms:
 *   {"reset":true}                  → erase NVS creds, reboot to the
 *                                     compile-time default
 *   {"ssid":"...","password":"..."} → stage as CANDIDATE + reboot; the
 *                                     verify-before-commit ladder in
 *                                     app_main promotes or auto-reverts it.
 * Never reboots on a rejected/failed set (nothing changed). The password
 * is parsed and stored but NEVER echoed back to MQTT. */
/* parse_melody lives in melody.c (melody_parse) so it's host-unit-testable. */

/* cmd/auth — runtime web-admin (HTTP basic-auth) credentials, mirroring
 * cmd/wifi. {"user":"…","pass":"…"} sets them (applied live, no reboot);
 * {"reset":true} clears the override back to the secrets.h default. Status
 * (no secret) is echoed on <base>/state/auth. */
static void handle_cmd_auth(const char *json) {
    char st_topic[64];
    snprintf(st_topic, sizeof(st_topic), "%s/state/auth", T.base);
    if (json_bool_true(json, "reset")) {
        ESP_LOGW(TAG, "cmd/auth: reset web-admin creds to compile default");
        esp_err_t e = auth_store_erase();
        mqtt_pub(st_topic, e == ESP_OK ? "{\"status\":\"reset\"}"
                                       : "{\"status\":\"error\"}");
        return;
    }
    char user[AUTH_STORE_USER_CAP] = {0}, pass[AUTH_STORE_PASS_CAP] = {0};
    if (json_str(json, "user", user, sizeof(user))) {
        json_str(json, "pass", pass, sizeof(pass));
        esp_err_t e = auth_store_set(user, pass);
        ESP_LOGI(TAG, "cmd/auth: set web-admin creds (%s)",
                 e == ESP_OK ? "ok" : "rejected");
        mqtt_pub(st_topic, e == ESP_OK
                               ? "{\"status\":\"set\"}"
                               : "{\"status\":\"error\",\"reason\":\"bad-creds\"}");
        return;
    }
    mqtt_pub(st_topic, "{\"status\":\"error\",\"reason\":\"no-user\"}");
}

/* Host[:port] slice of a URL/URI for the redacted state/net payload —
 * everything after scheme:// up to the first '/'. Whole input when no
 * scheme (defensive). */
static void url_host(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t o = 0;
    while (p[o] && p[o] != '/' && o + 1 < cap) {
        out[o] = p[o];
        o++;
    }
    out[o] = 0;
}

/* Cached clientAuth-EKU verdict for the stored leaf. Computed ONCE in
 * mqtt_init (main task, roomy stack) — the mbedtls X.509 parse inside
 * tls_store_cert_has_client_auth needs more stack than mqtt_task has,
 * and mqtt_publish_net_state runs on mqtt_task (observed 41-crash
 * stack-overflow boot loop when it parsed inline). Staleness is fine:
 * a successful re-enrollment always reboots (enroll_retry_task), which
 * recomputes this. */
static bool s_cert_eku_client = false;

bool mqtt_client_identity_is_stale(void) {
    if (s_cert_eku_client) return false;         /* already presenting it */
    net_cfg_t nc;
    net_cfg_src_t src;
    if (net_store_get_effective(&nc, &src) != ESP_OK) return false;
    if (nc.mqtt_auth == NET_AUTH_USERPASS) return false;   /* never wanted one */
    return tls_store_cert_has_client_auth();
}

/* Publish the retained, REDACTED endpoint diagnostic to <id>/state/net —
 * the fleet-migration observability instrument. Hosts and tier only; no
 * credentials, no tokens, no full URLs with paths. The cert block
 * (clientAuth EKU + days to expiry) tells the operator whether this
 * board's leaf can carry the mTLS rung BEFORE the broker flip. */
void mqtt_publish_net_state(void) {
    net_cfg_t nc;
    net_cfg_src_t src;
    if (net_store_get_effective(&nc, &src) != ESP_OK) return;
    char mqtt_h[96], ota_h[96], relay_h[96];
    url_host(nc.mqtt_uri, mqtt_h, sizeof(mqtt_h));
    url_host(nc.ota_url, ota_h, sizeof(ota_h));
    url_host(nc.relay_url[0] ? nc.relay_url : RELAY_HOST,
             relay_h, sizeof(relay_h));
    int cert_days = -1;  /* -1 = no cert stored */
    int64_t exp = 0;
    if (tls_store_has_cert() && tls_store_get_expiry(&exp) == ESP_OK) {
        cert_days = (int)((exp - (int64_t)time(NULL)) / 86400);
    }
    char msg[512];
    snprintf(msg, sizeof(msg),
             "{\"tier\":\"%s\",\"mqtt\":\"%s\",\"auth\":\"%s\","
             "\"ota\":\"%s\",\"relay\":\"%s\",\"enroll\":\"%s\","
             "\"candidate\":%s,\"eku_client\":%s,\"cert_days\":%d}",
             net_store_src_str(src), mqtt_h, net_store_auth_str(nc.mqtt_auth),
             ota_h, relay_h, nc.enroll_url[0] ? "https" : "mqtt-legacy",
             net_store_has_candidate() ? "true" : "false",
             s_cert_eku_client ? "true" : "false",
             cert_days);
    pub(T.net_state, msg, 1, 1);
}

void mqtt_publish_enroll_state(const char *json) {
    if (!json) return;
    pub(T.enroll_state, json, 1, 1);
}

/* cmd/cert — certificate lifecycle operations:
 *   {"renew":true} → forced re-enrollment (async pipeline; progress lands
 *                    retained on state/enroll, reboots on success). The
 *                    migration step that upgrades a legacy serverAuth-only
 *                    leaf to a clientAuth one BEFORE the mtls broker flip.
 *   {"show":true}  → republish state/net (carries eku_client/cert_days). */
static void handle_cmd_cert(const char *json) {
    if (fj_bool_true(json, "renew")) {
        ESP_LOGW(TAG, "cmd/cert: operator-forced re-enrollment");
        if (tls_enroll_force_async(/*timeout_ms*/ 30000) != ESP_OK) {
            mqtt_publish_enroll_state(
                "{\"status\":\"failed\",\"reason\":\"spawn\"}");
        }
        return;
    }
    if (fj_bool_true(json, "show")) {
        mqtt_publish_net_state();
        return;
    }
    mqtt_publish_enroll_state(
        "{\"status\":\"error\",\"reason\":\"unknown-cmd\"}");
}

/* cmd/endpoint — runtime network-endpoint reconfiguration (net_store.c).
 * Three mutually exclusive forms:
 *   {"show":true}     → republish state/net (+ ack)
 *   {"clear":true}    → erase net_cfg → compile defaults + reboot
 *   {"set":{...}}     → broker fields staged as candidate + reboot into the
 *                       verify ladder; live fields merged + applied now.
 * Ack (non-retained) on <id>/state/endpoint. Credentials are accepted in
 * the payload but never echoed anywhere. */
static void handle_cmd_endpoint(const char *json) {
    if (fj_bool_true(json, "show")) {
        mqtt_publish_net_state();
        mqtt_pub(T.endpoint_state, "{\"status\":\"shown\"}");
        return;
    }
    if (fj_bool_true(json, "clear")) {
        ESP_LOGW(TAG, "cmd/endpoint: clearing net_cfg → compile defaults");
        if (net_store_erase() != ESP_OK) {
            mqtt_pub(T.endpoint_state, "{\"status\":\"error\",\"reason\":\"nvs\"}");
            return;
        }
        mqtt_pub(T.endpoint_state, "{\"status\":\"cleared-reboot\"}");
        mqtt_publish_availability(false);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }

    size_t set_len = 0;
    const char *set_obj = fj_object(json, "set", &set_len);
    if (!set_obj || set_len == 0 || set_len >= 600) {
        mqtt_pub(T.endpoint_state,
                 "{\"status\":\"error\",\"reason\":\"no-set-object\"}");
        return;
    }
    char body[608];
    /* Re-wrap the slice as a standalone object for the fj_* helpers. */
    snprintf(body, sizeof(body), "{%.*s}", (int)set_len, set_obj);

    /* Broker fields → candidate + reboot; the two classes must not mix in
     * one payload (net_store refuses mixed live-sets, and a combined apply
     * would race the reboot). */
    static const char *BROKER_PROBE[] = {"mqtt_uri", "mqtt_auth",
                                         "mqtt_user", "mqtt_pass", NULL};
    bool has_broker = false;
    for (const char **f = BROKER_PROBE; *f && !has_broker; f++) {
        char tmp[8];
        /* presence check only — value may exceed tmp, fj_str still true */
        has_broker = fj_str(body, *f, tmp, sizeof(tmp)) || has_broker;
    }

    if (has_broker) {
        esp_err_t err = net_store_set_candidate_json(body);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            mqtt_pub(T.endpoint_state,
                     "{\"status\":\"error\",\"reason\":\"tls-not-supported-yet\"}");
            return;
        }
        if (err != ESP_OK) {
            mqtt_pub(T.endpoint_state,
                     "{\"status\":\"error\",\"reason\":\"invalid\"}");
            return;
        }
        ESP_LOGW(TAG, "cmd/endpoint: broker candidate staged — rebooting into "
                      "verify ladder");
        mqtt_pub(T.endpoint_state, "{\"status\":\"staged-reboot-pending\"}");
        mqtt_publish_availability(false);
        vTaskDelay(pdMS_TO_TICKS(2000)); /* let the ack drain */
        esp_restart();
        return;
    }

    esp_err_t err = net_store_set_live_json(body);
    if (err != ESP_OK) {
        mqtt_pub(T.endpoint_state, "{\"status\":\"error\",\"reason\":\"invalid\"}");
        return;
    }
    mqtt_pub(T.endpoint_state, "{\"status\":\"applied\"}");
    mqtt_publish_net_state();
}

static void handle_cmd_wifi(const char *json) {
    if (json_bool_true(json, "reset")) {
        ESP_LOGW(TAG, "cmd/wifi: reset to compile default requested");
        if (wifi_store_erase() == ESP_OK) {
            mqtt_pub(T.wifi_state, "{\"status\":\"reset\"}");
            mqtt_publish_availability(false);
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        mqtt_pub(T.wifi_state, "{\"status\":\"error\",\"reason\":\"erase-failed\"}");
        return;
    }

    /* Full AP-only mode toggle (sticky). Reboots to switch radio mode. */
    bool apo;
    if (json_bool(json, "ap_only", &apo)) {
        ESP_LOGW(TAG, "cmd/wifi: ap_only=%d requested", (int)apo);
        if (wifi_store_set_ap_only(apo) == ESP_OK) {
            mqtt_pub(T.wifi_state, apo ? "{\"status\":\"ap-only-enabled\"}"
                                       : "{\"status\":\"ap-only-disabled\"}");
            mqtt_publish_availability(false);
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        mqtt_pub(T.wifi_state, "{\"status\":\"error\",\"reason\":\"ap-only-set-failed\"}");
        return;
    }

    /* Operator-set AP creds (SoftAP + AP-only). Applies on next AP bring-up;
     * no reboot. {"ap_ssid":"","ap_pass":""} clears back to the derived default. */
    char ap_ssid[WIFI_STORE_SSID_CAP] = {0};
    char ap_pass[WIFI_STORE_PASS_CAP] = {0};
    if (json_str(json, "ap_ssid", ap_ssid, sizeof(ap_ssid))) {
        json_str(json, "ap_pass", ap_pass, sizeof(ap_pass));
        esp_err_t er = wifi_store_set_ap(ap_ssid, ap_pass);
        if (er == ESP_OK) {
            mqtt_pub(T.wifi_state, "{\"status\":\"ap-creds-set\"}");
        } else {
            char m[96];
            snprintf(m, sizeof(m), "{\"status\":\"error\",\"reason\":\"%s\"}",
                     esp_err_to_name(er));
            mqtt_pub(T.wifi_state, m);
        }
        return;
    }

    char ssid[WIFI_STORE_SSID_CAP] = {0};
    char pass[WIFI_STORE_PASS_CAP] = {0};
    if (!json_str(json, "ssid", ssid, sizeof(ssid)) || ssid[0] == 0) {
        ESP_LOGW(TAG, "cmd/wifi: missing/empty ssid");
        mqtt_pub(T.wifi_state, "{\"status\":\"error\",\"reason\":\"missing-ssid\"}");
        return;
    }
    json_str(json, "password", pass, sizeof(pass)); /* optional (open net) */

    esp_err_t er = wifi_store_set_candidate(ssid, pass);
    if (er != ESP_OK) {
        ESP_LOGW(TAG, "cmd/wifi: candidate rejected: %s", esp_err_to_name(er));
        char msg[96];
        snprintf(msg, sizeof(msg), "{\"status\":\"error\",\"reason\":\"%s\"}",
                 esp_err_to_name(er));
        mqtt_pub(T.wifi_state, msg);
        return; /* nothing changed — do NOT reboot */
    }

    /* SSID only — password is never published. */
    mqtt_publish_wifi_status("candidate-staged");
    ESP_LOGW(TAG, "cmd/wifi: candidate staged — rebooting to verify");
    mqtt_publish_availability(false);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

void mqtt_publish_availability(bool online) {
    pub(T.avail, online ? "online" : "offline", /*qos*/ 1, /*retain*/ 1);
}

void mqtt_publish_profile(const char *profile) {
    pub(T.profile, profile, /*qos*/ 1, /*retain*/ 1);
}

void mqtt_publish_ds_state(bool sleeping, int next_wake_s, uint32_t wake_count,
                           const char *reason, bool reed_wake) {
    /* Retained marker so HA distinguishes an intentional hibernate sleep from a
     * fault. On wake the supervisor republishes telemetry + availability=online;
     * the LWT still flips availability=offline when deep sleep cuts power. */
    char json[160];
    snprintf(json, sizeof(json),
             "{\"sleeping\":%s,\"next_wake_s\":%d,\"wake_count\":%" PRIu32
             ",\"reason\":\"%s\",\"reed_wake\":%s}",
             sleeping ? "true" : "false", next_wake_s, wake_count,
             reason ? reason : "?", reed_wake ? "true" : "false");
    pub(T.ds, json, /*qos*/ 1, /*retain*/ 1);
}

bool mqtt_outbox_empty(void) {
    /* True when the client has no queued/in-flight messages — the drain signal
     * cb_ds waits on before deep sleep so QoS-1 telemetry + the sleeping marker
     * actually leave the box. Returns true when disconnected (nothing pending we
     * can send anyway). */
    if (!s_client) return true;
    return esp_mqtt_client_get_outbox_size(s_client) == 0;
}

void mqtt_publish_audio_active(bool on) {
    /* Whether the mic pipeline is currently meant to run — gated by mode
     * (Continuous/Triggered) AND the audio active-hours window. Retained +
     * QoS 1 so HA survives a restart; published only on change (cheap). */
    pub(T.audio_active, on ? "ON" : "OFF", /*qos*/ 1, /*retain*/ 1);
}

void mqtt_publish_telemetry(float soc, float vbat, float crate, int rssi, float temp_c,
                            float humidity_pct) {
    if (!atomic_load(&s_connected))
        return;
    char buf[24];
    /* Battery (bus0 MAX17048): skip when the gauge is absent/unreadable
     * (battery_soc returns <0) so HA shows "unavailable" rather than a
     * bogus -1, matching /sensors + /i2c which report it ABSENT. */
    if (soc >= 0) {
        snprintf(buf, sizeof(buf), "%.1f", soc);
        pub(T.soc, buf, 0, 0);
        snprintf(buf, sizeof(buf), "%.3f", vbat);
        pub(T.vbat, buf, 0, 0);
        snprintf(buf, sizeof(buf), "%.2f", crate);
        pub(T.crate, buf, 0, 0);
    }
    snprintf(buf, sizeof(buf), "%d", rssi);
    pub(T.rssi, buf, 0, 0);
    if (isfinite(temp_c)) {
        snprintf(buf, sizeof(buf), "%.1f", temp_c);
        pub(T.temp, buf, 0, 0);
    }
    if (isfinite(humidity_pct)) {
        snprintf(buf, sizeof(buf), "%.1f", humidity_pct);
        pub(T.humidity, buf, 0, 0);
    }
}

/* Publish every available physical-sensor channel to state/<obj>, straight
 * from the registry. Values come from the per-sensor caches (refreshed by
 * cb_sensors_refresh() in the telemetry tick), so this never touches I²C. */
void mqtt_publish_sensors(void) {
    if (!atomic_load(&s_connected))
        return;
    for (size_t si = 0; si < CB_SENSORS_N; si++) {
        const cb_sensor_t *sen = &CB_SENSORS[si];
        if (!sen->mqtt)
            continue;
        for (size_t ci = 0; ci < sen->n_chans; ci++) {
            const cb_chan_t *c = &sen->chans[ci];
            float v;
            if (!c->read(&v) || !isfinite(v))
                continue;
            char topic[160], buf[24];
            snprintf(topic, sizeof(topic), "%s/state/%s", device_id(), c->obj);
            snprintf(buf, sizeof(buf), "%.*f", c->decimals, (double)v);
            pub(topic, buf, 0, 0);
        }
    }
}

void mqtt_publish_telemetry_ext(float temp_c, float humidity_pct) {
    if (!atomic_load(&s_connected))
        return;
    char buf[24];
    if (isfinite(temp_c)) {
        snprintf(buf, sizeof(buf), "%.1f", temp_c);
        pub(T.temp_ext, buf, 0, 0);
    }
    if (isfinite(humidity_pct)) {
        snprintf(buf, sizeof(buf), "%.1f", humidity_pct);
        pub(T.humidity_ext, buf, 0, 0);
    }
}

void mqtt_publish_diag(uint32_t heap_free, uint32_t uptime_s, const char *reset_reason,
                       float mcu_temp_c) {
    if (!atomic_load(&s_connected))
        return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%" PRIu32, heap_free);
    pub(T.heap, buf, 0, 0);
    snprintf(buf, sizeof(buf), "%" PRIu32, uptime_s);
    pub(T.uptime, buf, 0, 0);
    if (reset_reason)
        pub(T.reset_reason, reset_reason, /*qos*/ 1, /*retain*/ 1);
    if (isfinite(mcu_temp_c)) {
        snprintf(buf, sizeof(buf), "%.1f", mcu_temp_c);
        pub(T.mcu_temp, buf, 0, 0);
    }
}

void mqtt_publish_fw_version(void) {
    /* Once-per-boot guard: the topic is retained, so re-publishing on
     * every telemetry tick burns broker bandwidth for zero value.
     * Caller can blindly call this on every tick now. */
    static bool s_sent = false;
    if (s_sent) return;
    if (!atomic_load(&s_connected))
        return;
    const esp_app_desc_t *app = esp_app_get_description();
    if (!app)
        return;
    /* app_elf_sha256 is the build's unique 32-byte ELF hash. Render
     * the leading 16 bytes (32 hex chars) — enough for change
     * detection and keeps the payload comfortably under 256 B. */
    char sha_hex[33] = {0};
    for (int i = 0; i < 16; i++) {
        snprintf(sha_hex + i * 2, 3, "%02x", app->app_elf_sha256[i]);
    }
    char json[320];
    int n = snprintf(json, sizeof(json),
                     "{\"version\":\"%s\","
                     "\"project_name\":\"%s\","
                     "\"date\":\"%s\","
                     "\"time\":\"%s\","
                     "\"idf_ver\":\"%s\","
                     "\"sha\":\"%s\"}",
                     app->version, app->project_name, app->date, app->time, app->idf_ver, sha_hex);
    if (n <= 0 || n >= (int)sizeof(json)) {
        ESP_LOGW(TAG, "fw_version payload truncated (%d)", n);
        return;
    }
    pub(T.fw_version, json, /*qos*/ 1, /*retain*/ 1);
    ESP_LOGI(TAG, "published fw_version → %s", json);
    s_sent = true;
}

void mqtt_publish_audio_telemetry(float rms_dbfs, uint32_t burst_count, uint32_t chunks_sent,
                                  bool streaming) {
    if (!atomic_load(&s_connected))
        return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f", rms_dbfs);
    pub(T.rms, buf, 0, 0);
    snprintf(buf, sizeof(buf), "%" PRIu32, burst_count);
    pub(T.burst, buf, 0, 0);
    snprintf(buf, sizeof(buf), "%" PRIu32, chunks_sent);
    pub(T.chunks, buf, 0, 0);
    /* Binary state (HA binary_sensor) — retained + QoS 1 so the value
     * survives HA restart instead of showing "unknown" until the next
     * audio tick. Low publish rate (~once per VAD transition) keeps
     * QoS 1 broker overhead negligible. */
    pub(T.streaming, streaming ? "ON" : "OFF", /*qos*/ 1, /*retain*/ 1);
}

void mqtt_publish_triggered(float rms_dbfs) {
    if (!atomic_load(&s_connected))
        return;
    /* Audio used to pass `now_ms()` (uptime ms) here as if it were
     * epoch_ms — the JSON `ts` field was therefore meaningless in HA.
     * Compute time once here, gate on SNTP-synced, and either emit a
     * real epoch or omit `ts` entirely. uint32_t epoch_s wraps in
     * 2106 which is well past the expected device lifetime. */
    time_t now = time(NULL);
    char buf[64];
    if (now > CB_CLOCK_SYNCED_EPOCH) {
        snprintf(buf, sizeof(buf),
                 "{\"ts\":%lld,\"rms\":%.1f}",
                 (long long)((int64_t)now * 1000), rms_dbfs);
    } else {
        snprintf(buf, sizeof(buf), "{\"rms\":%.1f}", rms_dbfs);
    }
    pub(T.triggered, buf, 0, 0);
}

void mqtt_publish_photo_event(const uint8_t *jpeg_buf, size_t jpeg_len, const char *trigger,
                              const char *sd_path,
                              int agc_gain, bool ir_active,
                              int framesize, int quality) {
    (void)jpeg_buf; /* raw bytes go via mqtt_publish_photo_image; this is metadata only */
    if (!atomic_load(&s_connected))
        return;
    static atomic_uint_fast32_t s_photo_seq = 0;
    uint32_t seq = atomic_fetch_add(&s_photo_seq, 1) + 1;

    /* Build an absolute URL the HA side can dial — assumes it shares
     * a routable network with the board. Falls back to empty string
     * when no IP yet (board still bootstrapping), in which case the
     * MQTT camera entity (image topic, see below) is the only way HA
     * can see the shot. basename(sd_path) is what /photo?f= expects. */
    char ip[20] = {0};
    bool have_ip = wifi_mgr_get_ip_str(ip, sizeof(ip));
    char url[160] = {0};
    if (have_ip && sd_path) {
        const char *base = strrchr(sd_path, '/');
        base = base ? base + 1 : sd_path;
        /* Scheme follows whatever the local server came up on. After
         * tls_enroll runs the device serves HTTPS; before, plain HTTP.
         * HA + browsers consuming this URL must have the budka sub-CA
         * in their trust store to actually fetch via HTTPS — see
         * HTTPS.md "HA integration" for the import steps. */
        const char *scheme = http_server_is_https() ? "https" : "http";
        snprintf(url, sizeof(url), "%s://%s/photo?f=%s", scheme, ip, base);
    }

    /* Capture-environment extras for the HA photo caption (mirrors the EXIF
     * UserComment in jpeg_stamp.c): link RSSI, wall-clock, ambient temp (SHT41)
     * + pressure (BMP388). agc/ir are already in the payload. Cached sensor
     * reads — no blocking I²C in the capture path. */
    int ev_rssi = wifi_mgr_rssi();
    time_t ev_now = time(NULL);
    /* Capture-environment reads (cached, no I²C) for the caption. */
    float air_t, air_rh, press;
    bool have_t  = cb_sensor_chan("sht0", "temp", &air_t);
    bool have_rh = cb_sensor_chan("sht0", "humidity", &air_rh);
    bool have_p  = cb_sensor_chan("bmp", "pressure", &press);
    bool have_bat = battery_ready();
    float vbatt = have_bat ? battery_vbat() : NAN;
    float soc   = have_bat ? battery_soc()  : NAN;

    /* Pre-format the ONE-LINE caption here, byte-for-byte the same as the
     * on-board HP overlay (exif_caption_html): "<YYYY-MM-DD HH:MM:SS> · <trigger>
     * · <batt|rssi> · <temp> °C · <rh> % · <press> hPa · AGC <n>". UTF-8
     * ·(·) / °(°) / nbsp( ). HA paints it as a CSS overlay on the
     * frame (picture-elements), so the dashboard strip matches the board on the
     * hair — no field re-assembly or styling drift on the HA side. */
    char cap[224];
    int cn = 0;
    if (ev_now > 1600000000) {
        struct tm tmv;
        localtime_r(&ev_now, &tmv);
        cn += (int)strftime(cap + cn, sizeof(cap) - (size_t)cn, "%Y-%m-%d %H:%M:%S", &tmv);
    }
    cn += snprintf(cap + cn, sizeof(cap) - (size_t)cn, "%s%s",
                   cn ? " · " : "", trigger ? trigger : "unknown");
    if (have_bat && isfinite(vbatt))
        cn += snprintf(cap + cn, sizeof(cap) - (size_t)cn,
                       " · %.2f V (%.0f %%)", (double)vbatt, (double)soc);
    else
        cn += snprintf(cap + cn, sizeof(cap) - (size_t)cn, " · %d dBm", ev_rssi);
    if (have_t && isfinite(air_t))
        cn += snprintf(cap + cn, sizeof(cap) - (size_t)cn, " · %.1f °C", (double)air_t);
    if (have_rh && isfinite(air_rh))
        cn += snprintf(cap + cn, sizeof(cap) - (size_t)cn, " · %.0f %%", (double)air_rh);
    if (have_p && isfinite(press))
        cn += snprintf(cap + cn, sizeof(cap) - (size_t)cn, " · %.0f hPa", (double)press);
    cn += snprintf(cap + cn, sizeof(cap) - (size_t)cn, " · AGC %d", agc_gain);
    (void)cn;

    char ev[640];
    int n = snprintf(ev, sizeof(ev),
                     "{\"seq\":%" PRIu32
                     ",\"size\":%u,\"trigger\":\"%s\","
                     "\"path\":\"%s\",\"url\":\"%s\","
                     "\"agc\":%d,\"ir\":%d,"
                     "\"framesize\":%d,\"quality\":%d,\"cap\":\"%s\"}",
                     seq, (unsigned)jpeg_len, trigger ? trigger : "unknown", sd_path ? sd_path : "",
                     url, agc_gain, ir_active ? 1 : 0,
                     framesize, quality, cap);
    if (n > 0) {
        /* HA's sensor.last_photo reads `seq/trigger/url/...` from the
         * json_attr_t pointed at T.photo_event. Retain the last event so
         * a fresh HA boot/reload pulls the last shot's attributes from
         * the broker instead of waiting for the next capture. QoS 0 is
         * fine — fresh events overwrite older retained payloads. */
        pub(T.photo_event, ev, /*qos*/ 0, /*retain*/ 1);
    }

    char cnt[16];
    snprintf(cnt, sizeof(cnt), "%" PRIu32, seq);
    pub(T.capture_count, cnt, 0, 0);
}

void mqtt_publish_ambient_agc(int agc_gain) {
    if (!atomic_load(&s_connected)) return;
    if (agc_gain < 0) return;  /* skip sentinel "not measured" */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", agc_gain);
    mqtt_pub_retained(T.ambient_agc, buf);
}

/* Buffer size set in mqtt_init(); kept here so the caller's "too big"
 * log line can name a concrete threshold instead of "?". Keep in sync
 * with .buffer.out_size below.
 *
 * Sized to MATCH the photo_queue slot cap (ENTRY_CAP_BYTES = 400 KB):
 * the queue will accept any frame up to 400 KB, so the publish buffer
 * must too, or a frame gets queued and then fails every publish with
 * rc<0 — silent image loss. The two caps must never diverge. 400 KB
 * covers the esp32-camera JPEG fb bound (UXGA w·h/5 ≈ 384 KB); lit
 * indoor UXGA q=12 shots measured 342 KB on 2026-07-10, over the old
 * 200 KB cap — that was the "HA shows stale photos" bug. The buffer is
 * heap-allocated under SPIRAM_USE_MALLOC, so this lives in PSRAM (zero
 * DRAM cost). Typical field frames are ~100 KB; this is the tail. */
#define MQTT_OUT_BUFFER_SIZE (400u * 1024u)

/* Cumulative count of image-publish failures since boot (oversize,
 * MQTT not connected at the wrong moment, transient outbox full).
 * Surfaced in selftest JSON so a board that captures fine but can't
 * deliver bytes is visible as a non-zero counter — without this
 * accounting an oversized frame goes drop-oldest in photo_queue
 * after 8 retries and the loss is silent. */
static atomic_uint_fast32_t s_photo_publish_errors = 0;

uint32_t mqtt_photo_publish_errors_total(void) {
    return (uint32_t)atomic_load(&s_photo_publish_errors);
}

void mqtt_publish_photo_image(const uint8_t *jpeg_buf, size_t jpeg_len) {
    if (!atomic_load(&s_connected))
        return;
    if (!jpeg_buf || jpeg_len == 0)
        return;
    /* QoS 1 + retained: HA's mqtt camera platform consumes the raw
     * bytes from this topic. Retained so a late HA restart still has
     * the last photo to show; QoS 1 so a brief broker hiccup doesn't
     * silently drop a shot. The 160 KB out_size in mqtt_init covers
     * the full UXGA q=12 envelope; an unusually high-detail frame can
     * occasionally exceed it, so log the rejection instead of letting
     * the publish disappear silently. */
    int rc = esp_mqtt_client_publish(s_client, T.image_photo, (const char *)jpeg_buf, (int)jpeg_len,
                                     /*qos*/ 1, /*retain*/ 1);
    if (rc < 0) {
        atomic_fetch_add(&s_photo_publish_errors, 1);
        ESP_LOGW(TAG,
                 "image publish failed: rc=%d, payload=%u B, out_buf=%u B "
                 "(rc=-1 typically means oversize; bump buffer.out_size)",
                 rc, (unsigned)jpeg_len, (unsigned)MQTT_OUT_BUFFER_SIZE);
    }
}

bool mqtt_is_connected(void) {
    return atomic_load(&s_connected);
}

void mqtt_publish_motion(bool active) {
    mqtt_publish_motion_nth(0, active);
}

static void build_motion_topic(int idx, char *out, size_t out_sz) {
    if (idx == 0) {
        snprintf(out, out_sz, "%s", T.motion);
    } else {
        snprintf(out, out_sz, "%s_%d", T.motion, idx);
    }
}
static void build_motion_count_topic(int idx, char *out, size_t out_sz) {
    if (idx == 0) {
        snprintf(out, out_sz, "%s", T.motion_count);
    } else {
        snprintf(out, out_sz, "%s/state/motion_count_%d", mqtt_topic_base(), idx);
    }
}

void mqtt_publish_motion_nth(int idx, bool active) {
    char topic[160];
    build_motion_topic(idx, topic, sizeof(topic));
    pub(topic, active ? "ON" : "OFF", /*qos*/ 1, /*retain*/ 1);
}

void mqtt_publish_solar(float bus_v, float current_a, float power_w) {
    if (!atomic_load(&s_connected))
        return;
    char buf[24];
    if (isfinite(bus_v)) {
        snprintf(buf, sizeof(buf), "%.2f", bus_v);
        pub(T.solar_v, buf, 0, 0);
    }
    if (isfinite(current_a)) {
        snprintf(buf, sizeof(buf), "%.3f", current_a);
        pub(T.solar_i, buf, 0, 0);
    }
    if (isfinite(power_w)) {
        snprintf(buf, sizeof(buf), "%.2f", power_w);
        pub(T.solar_p, buf, 0, 0);
    }
}

/* External BLE UC96 power meters (firmware/BLE.md). Unlike the single on-board
 * INA226 solar sensor, there can be several meters (field rig daisy-chains
 * two), so each is keyed by its MAC under <base>/meter/<id>/… and gets its own
 * HA entities. HA friendly-names them downstream, as elsewhere in this repo. */
void mqtt_publish_uc96(const char *id, float v, float i, float p, float wh, int temp_c) {
    if (!atomic_load(&s_connected) || !id || !id[0])
        return;
    char topic[96], buf[24];
    if (isfinite(v)) {
        snprintf(topic, sizeof(topic), "%s/meter/%s/voltage", mqtt_topic_base(), id);
        snprintf(buf, sizeof(buf), "%.2f", v);
        pub(topic, buf, 0, 0);
    }
    if (isfinite(i)) {
        snprintf(topic, sizeof(topic), "%s/meter/%s/current", mqtt_topic_base(), id);
        snprintf(buf, sizeof(buf), "%.3f", i);
        pub(topic, buf, 0, 0);
    }
    if (isfinite(p)) {
        snprintf(topic, sizeof(topic), "%s/meter/%s/power", mqtt_topic_base(), id);
        snprintf(buf, sizeof(buf), "%.2f", p);
        pub(topic, buf, 0, 0);
    }
    if (isfinite(wh)) {
        snprintf(topic, sizeof(topic), "%s/meter/%s/energy", mqtt_topic_base(), id);
        snprintf(buf, sizeof(buf), "%.2f", wh);
        pub(topic, buf, 0, 0);
    }
    snprintf(topic, sizeof(topic), "%s/meter/%s/temperature", mqtt_topic_base(), id);
    snprintf(buf, sizeof(buf), "%d", temp_c);
    pub(topic, buf, 0, 0);
}

void mqtt_publish_uc96_discovery(const char *id) {
    if (!atomic_load(&s_connected) || !id || !id[0])
        return;
    char obj[40], statt[96];

    snprintf(obj, sizeof(obj), "uc96_%s_v", id);
    snprintf(statt, sizeof(statt), "%s/meter/%s/voltage", mqtt_topic_base(), id);
    pub_discovery_one("sensor", obj,
        "\"name\":\"UC96 %s Voltage\",\"stat_t\":\"%s\","
        "\"unit_of_meas\":\"V\",\"dev_cla\":\"voltage\",\"stat_cla\":\"measurement\"",
        id, statt);

    snprintf(obj, sizeof(obj), "uc96_%s_i", id);
    snprintf(statt, sizeof(statt), "%s/meter/%s/current", mqtt_topic_base(), id);
    pub_discovery_one("sensor", obj,
        "\"name\":\"UC96 %s Current\",\"stat_t\":\"%s\","
        "\"unit_of_meas\":\"A\",\"dev_cla\":\"current\",\"stat_cla\":\"measurement\"",
        id, statt);

    snprintf(obj, sizeof(obj), "uc96_%s_p", id);
    snprintf(statt, sizeof(statt), "%s/meter/%s/power", mqtt_topic_base(), id);
    pub_discovery_one("sensor", obj,
        "\"name\":\"UC96 %s Power\",\"stat_t\":\"%s\","
        "\"unit_of_meas\":\"W\",\"dev_cla\":\"power\",\"stat_cla\":\"measurement\"",
        id, statt);

    snprintf(obj, sizeof(obj), "uc96_%s_wh", id);
    snprintf(statt, sizeof(statt), "%s/meter/%s/energy", mqtt_topic_base(), id);
    pub_discovery_one("sensor", obj,
        "\"name\":\"UC96 %s Energy\",\"stat_t\":\"%s\","
        "\"unit_of_meas\":\"Wh\",\"dev_cla\":\"energy\",\"stat_cla\":\"total_increasing\"",
        id, statt);

    snprintf(obj, sizeof(obj), "uc96_%s_t", id);
    snprintf(statt, sizeof(statt), "%s/meter/%s/temperature", mqtt_topic_base(), id);
    pub_discovery_one("sensor", obj,
        "\"name\":\"UC96 %s Temp\",\"stat_t\":\"%s\","
        "\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\"",
        id, statt);

    ESP_LOGI(TAG, "published HA discovery for meter %s", id);
}

/* BTHome v2 passive sensors (thermo-hygrometers etc.) read over BLE — only the
 * allowlisted ones, published per device under <base>/sensor/<mac>/… mirroring
 * the UC96 path. Only fields actually present in the advert are emitted. */
void mqtt_publish_bthome(const char *id, const ble_bthome_reading_t *r) {
    if (!atomic_load(&s_connected) || !id || !id[0] || !r) return;
    char topic[96], buf[24];
    if (r->temp_present) {
        snprintf(topic, sizeof(topic), "%s/sensor/%s/temperature", mqtt_topic_base(), id);
        snprintf(buf, sizeof(buf), "%.2f", r->temp_c);
        pub(topic, buf, 0, 0);
    }
    if (r->humidity_present) {
        snprintf(topic, sizeof(topic), "%s/sensor/%s/humidity", mqtt_topic_base(), id);
        snprintf(buf, sizeof(buf), "%.2f", r->humidity_pct);
        pub(topic, buf, 0, 0);
    }
    if (r->battery_present) {
        snprintf(topic, sizeof(topic), "%s/sensor/%s/battery", mqtt_topic_base(), id);
        snprintf(buf, sizeof(buf), "%u", (unsigned)r->battery_pct);
        pub(topic, buf, 0, 0);
    }
    if (r->voltage_present) {
        snprintf(topic, sizeof(topic), "%s/sensor/%s/voltage", mqtt_topic_base(), id);
        snprintf(buf, sizeof(buf), "%.3f", r->voltage_v);
        pub(topic, buf, 0, 0);
    }
}

void mqtt_publish_bthome_discovery(const char *id, const ble_bthome_reading_t *r) {
    if (!atomic_load(&s_connected) || !id || !id[0] || !r) return;
    char obj[40], statt[96];
    if (r->temp_present) {
        snprintf(obj, sizeof(obj), "bth_%s_t", id);
        snprintf(statt, sizeof(statt), "%s/sensor/%s/temperature", mqtt_topic_base(), id);
        pub_discovery_one("sensor", obj,
            "\"name\":\"BLE %s Temp\",\"stat_t\":\"%s\","
            "\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\"",
            id, statt);
    }
    if (r->humidity_present) {
        snprintf(obj, sizeof(obj), "bth_%s_h", id);
        snprintf(statt, sizeof(statt), "%s/sensor/%s/humidity", mqtt_topic_base(), id);
        pub_discovery_one("sensor", obj,
            "\"name\":\"BLE %s Humidity\",\"stat_t\":\"%s\","
            "\"unit_of_meas\":\"%%\",\"dev_cla\":\"humidity\",\"stat_cla\":\"measurement\"",
            id, statt);
    }
    if (r->battery_present) {
        snprintf(obj, sizeof(obj), "bth_%s_b", id);
        snprintf(statt, sizeof(statt), "%s/sensor/%s/battery", mqtt_topic_base(), id);
        pub_discovery_one("sensor", obj,
            "\"name\":\"BLE %s Battery\",\"stat_t\":\"%s\","
            "\"unit_of_meas\":\"%%\",\"dev_cla\":\"battery\",\"stat_cla\":\"measurement\","
            "\"ent_cat\":\"diagnostic\"",
            id, statt);
    }
    if (r->voltage_present) {
        snprintf(obj, sizeof(obj), "bth_%s_v", id);
        snprintf(statt, sizeof(statt), "%s/sensor/%s/voltage", mqtt_topic_base(), id);
        pub_discovery_one("sensor", obj,
            "\"name\":\"BLE %s Voltage\",\"stat_t\":\"%s\","
            "\"unit_of_meas\":\"V\",\"dev_cla\":\"voltage\",\"stat_cla\":\"measurement\","
            "\"ent_cat\":\"diagnostic\"",
            id, statt);
    }
    ESP_LOGI(TAG, "published HA discovery for BTHome %s", id);
}

void mqtt_publish_selftest(const char *json) {
    pub(T.selftest, json, /*qos*/ 1, /*retain*/ 1);
}

void mqtt_publish_motion_count(uint32_t count) {
    if (!atomic_load(&s_connected))
        return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%" PRIu32, count);
    /* Retain so HA's sensor.motion_events doesn't go "unavailable"
     * across a recorder restart / discovery republish — same semantics
     * as reed_count (mqtt_publish_reed_count below). Diverged earlier
     * by oversight, not by design. */
    pub(T.motion_count, buf, /*qos*/ 1, /*retain*/ 1);
}

void mqtt_publish_motion_count_nth(int idx, uint32_t count) {
    if (!atomic_load(&s_connected))
        return;
    char topic[160], buf[16];
    build_motion_count_topic(idx, topic, sizeof(topic));
    snprintf(buf, sizeof(buf), "%" PRIu32, count);
    pub(topic, buf, /*qos*/ 1, /*retain*/ 1);
}

void mqtt_publish_reed(bool closed) {
    mqtt_publish_reed_nth(0, closed);
}

void mqtt_publish_reed_count(uint32_t count) {
    mqtt_publish_reed_count_nth(0, count);
}

/* Build the reed state topic for instance `idx`. Instance 0 reuses the
 * singleton-era topic name (T.reed = "<id>/state/reed") so existing HA
 * dashboards + recorder history survive the migration unchanged.
 * Instances 1+ get the indexed `_N` suffix. */
static void build_reed_topic(int idx, char *out, size_t out_sz) {
    if (idx == 0) {
        snprintf(out, out_sz, "%s", T.reed);
    } else {
        snprintf(out, out_sz, "%s_%d", T.reed, idx);
    }
}
static void build_reed_count_topic(int idx, char *out, size_t out_sz) {
    if (idx == 0) {
        snprintf(out, out_sz, "%s", T.reed_count);
    } else {
        snprintf(out, out_sz, "%s/state/reed_count_%d", mqtt_topic_base(), idx);
    }
}

void mqtt_publish_reed_nth(int idx, bool closed) {
    /* HA dev_cla=door wants ON=open, OFF=closed (and we set pl_on/off
     * to "OPEN"/"CLOSED" in discovery so the value is also human-
     * readable in mosquitto_sub output). Retained QoS 1 so HA sees
     * the current state even when reed events are sparse. */
    char topic[160];
    build_reed_topic(idx, topic, sizeof(topic));
    pub(topic, closed ? "CLOSED" : "OPEN", /*qos*/ 1, /*retain*/ 1);
}

void mqtt_publish_reed_count_nth(int idx, uint32_t count) {
    if (!atomic_load(&s_connected))
        return;
    char topic[160];
    char buf[16];
    build_reed_count_topic(idx, topic, sizeof(topic));
    snprintf(buf, sizeof(buf), "%" PRIu32, count);
    pub(topic, buf, /*qos*/ 1, /*retain*/ 1);
}

/* Grove ultrasonic distance. Telemetry-style QoS 0 non-retained — the
 * poll task sends a fresh one every sonar_poll_s and the telemetry tick
 * re-sends the cached value, so a lost sample self-heals in seconds. */
void mqtt_publish_distance(float cm) {
    if (!atomic_load(&s_connected) || !isfinite(cm))
        return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", (double)cm);
    pub(T.distance, buf, 0, 0);
}

/* Grove soil moisture: raw mV always (it's the calibration source), the
 * derived percent only when the dry/wet knobs are non-degenerate. */
void mqtt_publish_soil(float mv, float pct) {
    if (!atomic_load(&s_connected))
        return;
    char buf[16];
    if (isfinite(mv)) {
        snprintf(buf, sizeof(buf), "%.0f", (double)mv);
        pub(T.soil_mv, buf, 0, 0);
    }
    if (isfinite(pct)) {
        snprintf(buf, sizeof(buf), "%.0f", (double)pct);
        pub(T.soil_moist, buf, 0, 0);
    }
}

bool mqtt_snapshot_requested(void) {
    return atomic_exchange(&s_snapshot, false);
}

bool mqtt_photo_requested(void) {
    return atomic_exchange(&s_photo_req, false);
}

/* ─── event handler ─────────────────────────────────────────────────────── */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "connected as %s", device_id());
            atomic_store(&s_connected, true);
            status_led_mqtt_connected(true);
            esp_mqtt_client_subscribe(s_client, T.cmd_snapshot, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_photo, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_reboot, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_ota, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_wifi, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_auth, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_cfg_reset, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_factory_reset, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_oled_logo, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_beep, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_melody, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_alarm, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_sfx, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_pcm, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_endpoint, 0);
            esp_mqtt_client_subscribe(s_client, T.cmd_cert, 0);
            char cfg_wild[80];
            snprintf(cfg_wild, sizeof(cfg_wild), "%s+", T.cmd_cfg_pfx);
            esp_mqtt_client_subscribe(s_client, cfg_wild, 0);
            mqtt_publish_availability(true);
            mqtt_publish_discovery();
            app_config_publish_discovery();
            app_config_publish_state_all();
            mqtt_publish_net_state();
            /* Wake the photo-queue drain task — any frames stashed
             * while we were offline can now be republished. No-op if
             * queue is empty or photo_queue_init wasn't called. */
            extern void photo_queue_kick(void);
            photo_queue_kick();
            /* If a coredump from a prior crash is sitting in flash, ship it now
             * (its own task) so a field unit gets its panic out even if it's
             * about to hang again. Once per boot; gated + no-op without a dump. */
            diag_ship_coredump_mqtt();
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected");
            atomic_store(&s_connected, false);
            status_led_mqtt_connected(false);
            break;
        case MQTT_EVENT_DATA: {
            /* Enrollment response — single-flight, so only inspect the
             * topic when the helper has set s_enroll_sem. Multi-fragment
             * payloads land here as multiple events sharing the same
             * msg_id; esp-mqtt sets current_data_offset / total_data_len
             * so we can assemble. For cert PEM (~1.2 KB) under
             * buffer.size=4096 it'll usually be a single event, but
             * handle the chunked case anyway since esp-mqtt's
             * fragmentation threshold is set by .buffer.size at runtime. */
            /* Consume a cert fragment under s_enroll_lock so the helper's
             * teardown (which clears s_enroll_active + buf under the same
             * lock) can't free/NULL the state between our guard and our
             * memcpy/give. Everything touched here — active flag, buf, topic,
             * result, and the give — happens inside the lock; the semaphore is
             * persistent so the give is always on a live handle. */
            bool enroll_consumed = false;
            if (s_enroll_lock) {
                xSemaphoreTake(s_enroll_lock, portMAX_DELAY);
                if (s_enroll_active && s_enroll_buf != NULL &&
                    s_enroll_topic[0] != 0 &&
                    e->topic_len > 0 &&
                    e->topic_len == (int)strlen(s_enroll_topic) &&
                    strncmp(e->topic, s_enroll_topic, e->topic_len) == 0) {
                    enroll_consumed = true;
                    size_t off = (size_t)e->current_data_offset;
                    size_t tot = (size_t)e->total_data_len;
                    size_t dl  = (size_t)e->data_len;
                    /* Bounds-check the BROKER-supplied fragment offset/length
                     * against the buffer, not just the declared total. esp-mqtt
                     * fragments large publishes; a malformed/hostile publish on
                     * our state/cert topic (plaintext LAN broker, anyone with
                     * the shared creds) could otherwise present off/dl that pass
                     * the total check yet drive memcpy past s_enroll_buf — a
                     * network-reachable heap overflow. Require the fragment to
                     * fit inside the declared total AND the total inside the
                     * buffer (the +1 leaves room for the NUL written below). */
                    if (tot == 0 || tot + 1 > s_enroll_buf_sz ||
                        off > tot || dl > tot - off) {
                        /* tot==0: empty/degenerate payload (e.g. a stale
                         * retained message on state/cert) — reject rather than
                         * accept a 0-length "cert" the parser would choke on.
                         * off/dl out of range: malformed/hostile fragmentation. */
                        ESP_LOGE(TAG, "enroll: cert frag invalid (off=%zu dl=%zu tot=%zu buf=%zu)",
                                 off, dl, tot, s_enroll_buf_sz);
                        s_enroll_result = ESP_ERR_INVALID_SIZE;
                        xSemaphoreGive(s_enroll_sem);
                    } else {
                        memcpy(s_enroll_buf + off, e->data, dl);
                        if (off + dl >= tot) {
                            /* Last fragment — NUL-terminate so
                             * mbedtls_x509_crt_parse can consume it as PEM. */
                            s_enroll_buf_len = tot;
                            s_enroll_buf[tot] = 0;
                            s_enroll_result = ESP_OK;
                            xSemaphoreGive(s_enroll_sem);
                        }
                    }
                }
                xSemaphoreGive(s_enroll_lock);
            }
            if (enroll_consumed) break;
            size_t cfg_pfx_len = strlen(T.cmd_cfg_pfx);
            if (e->topic_len == (int)strlen(T.cmd_snapshot) &&
                strncmp(e->topic, T.cmd_snapshot, e->topic_len) == 0) {
                ESP_LOGI(TAG, "snapshot requested");
                atomic_store(&s_snapshot, true);
            } else if (e->topic_len == (int)strlen(T.cmd_photo) &&
                       strncmp(e->topic, T.cmd_photo, e->topic_len) == 0) {
                ESP_LOGI(TAG, "photo capture requested");
                atomic_store(&s_photo_req, true);
            } else if (e->topic_len == (int)strlen(T.cmd_reboot) &&
                       strncmp(e->topic, T.cmd_reboot, e->topic_len) == 0) {
                ESP_LOGW(TAG, "reboot requested via MQTT");
                mqtt_publish_availability(false);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            } else if (e->topic_len == (int)strlen(T.cmd_ota) &&
                       strncmp(e->topic, T.cmd_ota, e->topic_len) == 0) {
                ESP_LOGI(TAG, "OTA check requested via MQTT");
                ota_trigger_now();
            } else if (e->topic_len == (int)strlen(T.cmd_beep) &&
                       strncmp(e->topic, T.cmd_beep, e->topic_len) == 0) {
                /* "freq,ms" → one tone. Defaults if absent/partial. Non-blocking
                 * (speaker.c queues onto its own task); no-op unless a pad is
                 * mapped to the "buzzer" pin function. */
                char body[32];
                int blen = e->data_len < (int)sizeof(body) - 1
                               ? e->data_len
                               : (int)sizeof(body) - 1;
                memcpy(body, e->data, blen);
                body[blen] = 0;
                int freq = 1000, ms = 200;
                sscanf(body, "%d,%d", &freq, &ms);
                if (freq < 0)     freq = 0;
                if (freq > 12000) freq = 12000;
                if (ms < 10)      ms = 10;
                if (ms > 5000)    ms = 5000;
                ESP_LOGI(TAG, "cmd/beep: %d Hz, %d ms", freq, ms);
                audiofx_beep((uint16_t)freq, (uint16_t)ms);
            } else if (e->topic_len == (int)strlen(T.cmd_melody) &&
                       strncmp(e->topic, T.cmd_melody, e->topic_len) == 0) {
                /* "f:ms,f:ms,..." → play once. Empty payload → built-in demo.
                 * (tone mode = real pitches; buzzer mode = rhythm.) */
                static const speaker_note_t demo[] = {
                    { 523, 150 }, { 659, 150 }, { 784, 150 }, { 1047, 250 },
                };
                char body[384];
                int blen = e->data_len < (int)sizeof(body) - 1
                               ? e->data_len : (int)sizeof(body) - 1;
                memcpy(body, e->data, blen);
                body[blen] = 0;
                speaker_note_t notes[40];
                size_t n = melody_parse(body, notes, 40);
                if (n) {
                    ESP_LOGI(TAG, "cmd/melody: %u notes", (unsigned)n);
                    audiofx_play(notes, n);
                } else {
                    ESP_LOGI(TAG, "cmd/melody: demo");
                    audiofx_play(demo, sizeof(demo) / sizeof(demo[0]));
                }
            } else if (e->topic_len == (int)strlen(T.cmd_sfx) &&
                       strncmp(e->topic, T.cmd_sfx, e->topic_len) == 0) {
                /* "f:ms,..." → play once, legato (smooth SFX, no staccato) —
                 * same rendering as the boot power-up. */
                char body[384];
                int blen = e->data_len < (int)sizeof(body) - 1
                               ? e->data_len : (int)sizeof(body) - 1;
                memcpy(body, e->data, blen);
                body[blen] = 0;
                speaker_note_t notes[40];
                size_t n = melody_parse(body, notes, 40);
                if (n) {
                    ESP_LOGI(TAG, "cmd/sfx: %u notes", (unsigned)n);
                    audiofx_sfx(notes, n);
                }
            } else if (e->topic_len == (int)strlen(T.cmd_pcm) &&
                       strncmp(e->topic, T.cmd_pcm, e->topic_len) == 0) {
                /* Embedded PCM sample / RC test tone via the PDM "1-bit DAC",
                 * mirrored to the buzzer chiptune so it sounds on whichever
                 * output(s) are wired. */
                char body[32];
                int blen = e->data_len < (int)sizeof(body) - 1
                               ? e->data_len : (int)sizeof(body) - 1;
                memcpy(body, e->data, blen);
                body[blen] = 0;
                ESP_LOGI(TAG, "cmd/pcm: %s", body);
                if (strcmp(body, "coin") == 0) {
                    audiofx_coin();         /* sample on PDM + chiptune on buzzer */
                } else if (strcmp(body, "test") == 0) {
                    pcm_play_named("test"); /* 1 kHz sine on PDM (RC tuning) */
                    static const speaker_note_t t[] = { { 1000, 200 } };
                    speaker_loop(t, 1);     /* matching 1 kHz tone on buzzer */
                } else if (strcmp(body, "stop") == 0 || strcmp(body, "off") == 0) {
                    audiofx_stop();
                } else {
                    pcm_play_named(body);   /* other embedded sample names */
                }
            } else if (e->topic_len == (int)strlen(T.cmd_alarm) &&
                       strncmp(e->topic, T.cmd_alarm, e->topic_len) == 0) {
                /* "f:ms,..." → loop until stopped; "stop"/"off"/empty → stop. */
                char body[384];
                int blen = e->data_len < (int)sizeof(body) - 1
                               ? e->data_len : (int)sizeof(body) - 1;
                memcpy(body, e->data, blen);
                body[blen] = 0;
                if (blen == 0 || strcmp(body, "stop") == 0 ||
                    strcmp(body, "off") == 0) {
                    ESP_LOGI(TAG, "cmd/alarm: stop");
                    audiofx_stop();
                } else {
                    speaker_note_t notes[40];
                    size_t n = melody_parse(body, notes, 40);
                    if (n) {
                        ESP_LOGI(TAG, "cmd/alarm: loop %u notes", (unsigned)n);
                        audiofx_loop(notes, n);
                    }
                }
            } else if (e->topic_len == (int)strlen(T.cmd_endpoint) &&
                       strncmp(e->topic, T.cmd_endpoint, e->topic_len) == 0) {
                /* Network endpoint reconfig — {"show"|"clear"|"set":{...}}.
                 * Payload may carry several URLs + creds → generous local. */
                char body[640];
                int blen = e->data_len < (int)sizeof(body) - 1
                               ? e->data_len
                               : (int)sizeof(body) - 1;
                memcpy(body, e->data, blen);
                body[blen] = 0;
                ESP_LOGI(TAG, "cmd/endpoint received (%d B)", blen);
                handle_cmd_endpoint(body);
            } else if (e->topic_len == (int)strlen(T.cmd_cert) &&
                       strncmp(e->topic, T.cmd_cert, e->topic_len) == 0) {
                char body[64];
                int blen = e->data_len < (int)sizeof(body) - 1
                               ? e->data_len
                               : (int)sizeof(body) - 1;
                memcpy(body, e->data, blen);
                body[blen] = 0;
                ESP_LOGI(TAG, "cmd/cert received (%d B)", blen);
                handle_cmd_cert(body);
            } else if (e->topic_len == (int)strlen(T.cmd_wifi) &&
                       strncmp(e->topic, T.cmd_wifi, e->topic_len) == 0) {
                /* WiFi reconfig/reset — payload can exceed the cfg val[]
                 * buffer (ssid + password JSON), so copy into a local. */
                char body[256];
                int blen = e->data_len < (int)sizeof(body) - 1
                               ? e->data_len
                               : (int)sizeof(body) - 1;
                memcpy(body, e->data, blen);
                body[blen] = 0;
                ESP_LOGI(TAG, "cmd/wifi received (%d B)", blen);
                handle_cmd_wifi(body);
            } else if (e->topic_len == (int)strlen(T.cmd_auth) &&
                       strncmp(e->topic, T.cmd_auth, e->topic_len) == 0) {
                /* Web-admin login reconfig — {"user","pass"} or {"reset":true}.
                 * Applied live, no reboot. Local copy (creds JSON). */
                char body[256];
                int blen = e->data_len < (int)sizeof(body) - 1
                               ? e->data_len
                               : (int)sizeof(body) - 1;
                memcpy(body, e->data, blen);
                body[blen] = 0;
                ESP_LOGI(TAG, "cmd/auth received (%d B)", blen);
                handle_cmd_auth(body);
            } else if (e->topic_len == (int)strlen(T.cmd_cfg_reset) &&
                       strncmp(e->topic, T.cmd_cfg_reset, e->topic_len) == 0) {
                /* Reset tier (b): config knobs → schema defaults. Reboot so
                 * pin-map / apply-on-change side effects re-run cleanly.
                 * WiFi + TLS are untouched. */
                ESP_LOGW(TAG, "config reset requested via MQTT");
                app_config_reset_defaults();
                mqtt_publish_availability(false);
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            } else if (e->topic_len == (int)strlen(T.cmd_factory_reset) &&
                       strncmp(e->topic, T.cmd_factory_reset, e->topic_len) == 0) {
                /* Reset tier (c): full factory. Order matters — wipe config
                 * + TLS FIRST, WiFi LAST, so a power loss mid-sequence still
                 * leaves WiFi up to receive another command. TLS erase forces
                 * re-enroll on next boot; WiFi erase falls back to the
                 * compile-time default (then SoftAP if that fails).
                 *
                 * net_store goes too, or the reset leaves the worst of both:
                 * a stored broker that wants the client certificate we just
                 * erased. A board in that state cannot reach MQTT at all and
                 * only recovers once enrollment happens to succeed — which is
                 * how a HIL run stranded the bench after the stack migration. */
                ESP_LOGW(TAG, "FACTORY RESET requested via MQTT");
                app_config_reset_defaults();
                tls_store_erase();
                net_store_erase();
                wifi_store_erase();
                auth_store_erase();
                mqtt_publish_availability(false);
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            } else if (e->topic_len == (int)strlen(T.cmd_oled_logo) &&
                       strncmp(e->topic, T.cmd_oled_logo, e->topic_len) == 0) {
                /* Custom boot logo: 1024-byte 128x64 SSD1306 bitmap → store;
                 * "clear" → remove. Fits a single MQTT event (<4 KB buffer);
                 * reject a fragmented/partial publish rather than store junk. */
                if (e->data_len == 5 && strncmp(e->data, "clear", 5) == 0) {
                    oled_clear_logo();
                    oled_show_boot();
                    ESP_LOGI(TAG, "cmd/oled_logo: cleared");
                } else if (e->data_len == 4 && strncmp(e->data, "test", 4) == 0) {
                    oled_show_boot();   /* preview boot screen on the OLED */
                    ESP_LOGI(TAG, "cmd/oled_logo: previewing boot screen");
                } else if (e->data_len == 4 && strncmp(e->data, "dump", 4) == 0) {
                    /* Publish the stored logo's raw bytes so it can be baked
                     * in as a compile-time default. */
                    static uint8_t lb[OLED_LOGO_BYTES];
                    if (oled_get_logo(lb, sizeof(lb))) {
                        esp_mqtt_client_publish(s_client, T.oled_logo_dump,
                                                (const char *)lb, OLED_LOGO_BYTES, 1, 0);
                        ESP_LOGI(TAG, "cmd/oled_logo: dumped %d B to %s", OLED_LOGO_BYTES, T.oled_logo_dump);
                    } else {
                        ESP_LOGW(TAG, "cmd/oled_logo: no logo to dump");
                    }
                } else if (e->current_data_offset == 0 &&
                           e->data_len == OLED_LOGO_BYTES &&
                           e->total_data_len == OLED_LOGO_BYTES) {
                    bool ok = oled_set_logo((const unsigned char *)e->data, e->data_len);
                    if (ok)
                        oled_show_boot();   /* preview the new logo immediately */
                    ESP_LOGI(TAG, "cmd/oled_logo: %s (%d B)",
                             ok ? "stored, previewing" : "store failed", e->data_len);
                } else {
                    ESP_LOGW(TAG, "cmd/oled_logo: want %d bytes or \"clear\" (got %d/%d)",
                             OLED_LOGO_BYTES, e->data_len, (int)e->total_data_len);
                }
            } else if (e->topic_len > (int)cfg_pfx_len &&
                       strncmp(e->topic, T.cmd_cfg_pfx, cfg_pfx_len) == 0) {
                /* extract key + payload as null-terminated copies */
                char key[24], val[64];
                int keylen = e->topic_len - (int)cfg_pfx_len;
                if (keylen <= 0 || keylen >= (int)sizeof(key))
                    break;
                memcpy(key, e->topic + cfg_pfx_len, keylen);
                key[keylen] = 0;
                int vlen = e->data_len < (int)sizeof(val) - 1 ? e->data_len : (int)sizeof(val) - 1;
                memcpy(val, e->data, vlen);
                val[vlen] = 0;
                esp_err_t err = app_config_set_from_string(key, val);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "cfg %s = %s rejected: %s", key, val, esp_err_to_name(err));
                }
            }
            break;
        }
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "error event");
            break;
        default:
            break;
    }
}

/* Treat the secrets.h placeholder password as "not configured" — skip
 * MQTT init entirely so we don't burn a reconnect loop against a broker
 * that will reject us. Set a real password in secrets.h to enable. */
static bool mqtt_password_is_placeholder(void) {
    return secret_is_placeholder(MQTT_PASSWORD);
}

bool mqtt_init(void) {
    /* Runtime endpoint resolution (net_store candidate → good → compile
     * default). The record + creds must outlive esp_mqtt_client_init, which
     * keeps pointers into the config — hence static. */
    static net_cfg_t s_net;
    net_cfg_src_t net_src;
    net_store_get_effective(&s_net, &net_src);
    ESP_LOGI(TAG, "broker: %s (tier=%s, auth=%s)", s_net.mqtt_uri,
             net_store_src_str(net_src), net_store_auth_str(s_net.mqtt_auth));

    /* EKU verdict cached here on the main task — see s_cert_eku_client. */
    s_cert_eku_client = tls_store_cert_has_client_auth();

    /* The placeholder gate only makes sense for the compile-time floor — an
     * NVS record carries its own (operator-provided) credentials. */
    if (net_src == NET_CFG_DEFAULT && mqtt_password_is_placeholder()) {
        ESP_LOGW(TAG,
                 "disabled: MQTT_PASSWORD in secrets.h is a placeholder. "
                 "Set a real password to enable MQTT/HA discovery.");
        return false;
    }

    build_topics();

    /* Enrollment sync primitives — created once, lived for the process. The
     * semaphore is deliberately never deleted (see the s_enroll_* notes) so
     * the event task can never give a freed handle. */
    if (!s_enroll_sem)  s_enroll_sem  = xSemaphoreCreateBinary();
    if (!s_enroll_lock) s_enroll_lock = xSemaphoreCreateMutex();
    if (!s_enroll_sem || !s_enroll_lock) {
        ESP_LOGE(TAG, "enroll sync primitives OOM — TLS enrollment disabled");
        /* Non-fatal: MQTT still runs; enroll requests will refuse cleanly. */
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_net.mqtt_uri,
        .credentials.client_id = device_id(),
        .credentials.username = s_net.mqtt_user,
        .credentials.authentication.password = s_net.mqtt_pass,
        .session.keepalive = 60,
        /* Clean session each connect (disable_clean_session=false means
         * clean_session=true on the wire). Rationale:
         *  - We re-subscribe to every cmd_* topic from on_event in the
         *    MQTT_EVENT_CONNECTED handler, so subscriptions survive any
         *    reconnect even with clean_session=true.
         *  - cmd_t topics aren't retained by the publisher (HA, or
         *    mosquitto_pub without -r), so there's nothing to "miss"
         *    across a disconnect.
         *  - HA replays its own retained config + state_t + cmd_t cache
         *    on its side independently.
         * Persistent sessions (clean=false) would only add broker-side
         * book-keeping with no consumer-side benefit for this fleet. Stated
         * explicitly so the contract doesn't depend on esp-mqtt's default. */
        .session.disable_clean_session = false,
        .session.last_will.topic = T.avail,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 7,
        /* QoS 1 LWT so the broker durably delivers "offline" if our
         * keep-alive lapses between TCP ACKs — HA misses transitions
         * otherwise. */
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        /* Input buffer: HA discovery payloads, cmd/cfg replies etc.
         * fit well under 1 KB; bump to 4 KB to leave headroom for
         * future HA replies without re-tuning. */
        .buffer.size = 4096,
        /* Output buffer must hold a full JPEG publish: UXGA q=12 frames
         * are 70-115 KB plus a few bytes of MQTT framing. 160 KB gives
         * a comfortable margin and ends up in PSRAM thanks to
         * SPIRAM_USE_MALLOC=y, so it doesn't burn DRAM. */
        .buffer.out_size = MQTT_OUT_BUFFER_SIZE,
        /* Bound every transport write/read. The esp-mqtt default (10 s) is PER
         * write-fragment, and a slow-drip socket (a few bytes drained each
         * window) keeps making partial progress so the publish call never
         * returns — a ~100 KB image publish can then block the CALLING task
         * past the 30 s task-WDT and reboot the board. That was the capture
         * "no photo yet → wedge → reboot" failure: cam_wrk / the photo_queue
         * drain task stuck inside esp_mqtt_client_publish on a stalled link.
         * 3 s is far above a healthy LAN publish (sub-second) yet caps a
         * stalled socket fast; a bounded abort just defers to the photo_queue
         * reconnect retry. */
        .network.timeout_ms = 3000,
    };

    /* ── TLS (mqtts:// URIs) ──
     * Broker verification: esp_crt_bundle (public CAs — the stack broker
     * presents a Let's Encrypt cert), except on the bench-only INSECURE
     * build (self-signed dev stack; esp-tls skips verify when no anchor is
     * configured and CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y).
     * Client identity: with mqtt_auth=mtls/both, present the enrollment
     * leaf+sub-CA bundle + key from tls_store — mosquitto turns the bare-CN
     * leaf into the ACL identity. Legacy serverAuth-only leaves are skipped
     * (both → username/password rung carries the session and the re-enroll
     * path upgrades the cert; mtls strict → connect is left to fail and the
     * candidate ladder reverts). */
    if (strncmp(s_net.mqtt_uri, "mqtts://", 8) == 0) {
#if CONFIG_CHYTRA_BUDKA_TLS_INSECURE
        ESP_LOGW(TAG, "MQTTS: server cert verification DISABLED (bench build)");
#else
        if (s_net.mqtt_auth != NET_AUTH_USERPASS) {
            /* Internal broker (mtls/both): verify the broker's server cert
             * against the firmware-embedded budka sub-CA — our OWN PKI anchor,
             * NOT the public CA bundle. The broker presents a sub-CA-signed
             * leaf whose SAN is the broker host, so trust stays scoped to the
             * budka sub-CA and the device needs no public cert on the broker.
             * Hostname (CN/SAN) verification stays ON by default → a device
             * leaf (different SAN) can't impersonate the broker. */
            size_t subca_len = 0;
            const char *subca = tls_enroll_embedded_subca_pem(&subca_len);
            cfg.broker.verification.certificate = subca;
            cfg.broker.verification.certificate_len = subca_len;
        } else {
            /* Plain userpass over TLS (legacy/public LE broker) → public bundle. */
            cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
#endif
        if (s_net.mqtt_auth != NET_AUTH_USERPASS) {
            static tls_store_blob_t *s_tls;  /* outlives the client */
            if (!s_tls) s_tls = calloc(1, sizeof(*s_tls));
            if (s_tls && s_cert_eku_client &&
                tls_store_load(s_tls) == ESP_OK) {
                cfg.credentials.authentication.certificate =
                    (const char *)s_tls->cert_pem;
                cfg.credentials.authentication.certificate_len = s_tls->cert_len;
                cfg.credentials.authentication.key = (const char *)s_tls->key_der;
                cfg.credentials.authentication.key_len = s_tls->key_len;
                ESP_LOGI(TAG, "MQTTS: presenting enrollment cert as client identity");
            } else if (s_net.mqtt_auth == NET_AUTH_BOTH) {
                ESP_LOGW(TAG, "MQTTS: no clientAuth-capable cert stored — "
                              "falling back to username/password (auth=both)");
            } else {
                ESP_LOGE(TAG, "MQTTS: auth=mtls but no clientAuth-capable cert "
                              "stored — broker will reject; candidate ladder "
                              "will revert. Enroll first.");
            }
        }
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client)
        return false;
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "started → %s as %s", s_net.mqtt_uri, device_id());
    return true;
}

/* Clear the in-flight enroll state under the lock so the MQTT event task
 * stops accepting fragments for this round. Never touches the (persistent)
 * semaphore. */
static void enroll_disarm(void) {
    if (!s_enroll_lock) return;
    xSemaphoreTake(s_enroll_lock, portMAX_DELAY);
    s_enroll_active   = false;
    s_enroll_buf      = NULL;
    s_enroll_buf_sz   = 0;
    s_enroll_topic[0] = 0;
    xSemaphoreGive(s_enroll_lock);
}

esp_err_t mqtt_enroll_request(const char *csr_pem, size_t csr_len,
                               uint8_t *out_cert_buf, size_t out_cert_buf_sz,
                               size_t *out_cert_len,
                               uint32_t timeout_ms) {
    if (!csr_pem || csr_len == 0 || !out_cert_buf || out_cert_buf_sz == 0 ||
        !out_cert_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&s_connected) || !s_client) {
        ESP_LOGE(TAG, "enroll: MQTT not connected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_enroll_sem || !s_enroll_lock) {
        ESP_LOGE(TAG, "enroll: sync primitives unavailable (mqtt_init not run?)");
        return ESP_ERR_INVALID_STATE;
    }

    /* Compose request + response topics from the topic root. The signer
     * (cbd) subscribes to `+/cmd/enroll` wildcards across all devices
     * sharing the broker, replies on the same device's state/cert. */
    char req_topic[64];
    char resp_topic[64];
    snprintf(req_topic,  sizeof(req_topic),  "%s/cmd/enroll", T.base);
    snprintf(resp_topic, sizeof(resp_topic), "%s/state/cert", T.base);

    /* Arm under the lock: reject a concurrent caller (single-flight via
     * s_enroll_active), drain any stale signal left by a previous timed-out
     * round, then publish the state the event task will read. The semaphore is
     * persistent — never created/deleted here — so no give can hit a freed
     * handle. */
    xSemaphoreTake(s_enroll_lock, portMAX_DELAY);
    if (s_enroll_active) {
        xSemaphoreGive(s_enroll_lock);
        ESP_LOGE(TAG, "enroll: another enrollment already in flight");
        return ESP_ERR_INVALID_STATE;
    }
    (void)xSemaphoreTake(s_enroll_sem, 0);  /* drain any stale give */
    s_enroll_buf     = out_cert_buf;
    s_enroll_buf_sz  = out_cert_buf_sz;
    s_enroll_buf_len = 0;
    s_enroll_result  = ESP_FAIL;
    strncpy(s_enroll_topic, resp_topic, sizeof(s_enroll_topic) - 1);
    s_enroll_topic[sizeof(s_enroll_topic) - 1] = 0;
    s_enroll_active  = true;
    xSemaphoreGive(s_enroll_lock);

    /* QoS 1 subscribe → the broker queues the reply if the signer
     * publishes before our SUBACK lands. */
    int sub_id = esp_mqtt_client_subscribe(s_client, resp_topic, 1);
    if (sub_id < 0) {
        ESP_LOGE(TAG, "enroll: subscribe %s failed", resp_topic);
        enroll_disarm();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "enroll: subscribed to %s (msg_id=%d)", resp_topic, sub_id);

    /* QoS 1 publish — signer must ACK so we know the CSR reached the
     * broker; otherwise a network drop right after PUBLISH would leave
     * us waiting for a reply that never comes. */
    int pub_id = esp_mqtt_client_publish(s_client, req_topic, csr_pem,
                                          (int)csr_len, /*qos*/ 1, /*retain*/ 0);
    if (pub_id < 0) {
        ESP_LOGE(TAG, "enroll: publish %s failed (rc=%d)", req_topic, pub_id);
        esp_mqtt_client_unsubscribe(s_client, resp_topic);
        enroll_disarm();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "enroll: published CSR %zu B → %s (msg_id=%d)",
             csr_len, req_topic, pub_id);

    /* Block until on_event signals — or timeout. The sem is persistent. */
    esp_err_t result;
    if (xSemaphoreTake(s_enroll_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        /* Read the event task's writes under the lock (it's the publish
         * barrier — see the s_enroll_* notes). */
        xSemaphoreTake(s_enroll_lock, portMAX_DELAY);
        result = s_enroll_result;
        *out_cert_len = s_enroll_buf_len;
        xSemaphoreGive(s_enroll_lock);
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "enroll: got cert %zu B from %s",
                     *out_cert_len, resp_topic);
        }
    } else {
        ESP_LOGW(TAG, "enroll: timed out after %u ms waiting on %s",
                 (unsigned)timeout_ms, resp_topic);
        result = ESP_ERR_TIMEOUT;
    }

    /* Teardown: disarm so any late event is ignored (the semaphore is NOT
     * deleted — persistent), then unsubscribe. */
    enroll_disarm();
    esp_mqtt_client_unsubscribe(s_client, resp_topic);
    return result;
}
