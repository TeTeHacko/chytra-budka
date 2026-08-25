// main.cpp — Chytrá Budka app entry. Wires battery + audio + ModeFsm + MQTT.
//
// Stage C: full operational firmware.
//   - Battery (MAX17048 over I²C) drives ModeFsm hysteresis.
//   - Audio (INMP441 → cb::Vad → cb::ChunkedPoster) runs in Continuous or
//     Triggered modes; idle in Boot/Safe.
//   - MQTT publishes mode transitions, telemetry (mode-aware period), and
//     audio telemetry when streaming.
//
// Dual-core layout: app_main supervisor on CPU0 (prio 1, 10 Hz tick),
// audio_task on CPU1 (prio 10, ~32 ms PDM cadence), camera_worker on
// CPU1 (prio 5, drains async capture requests). WiFi/LWIP/MQTT stay on
// CPU0 per IDF defaults. See core_assignment.h for the full task map.

#include <cinttypes>
#include <atomic>
#include <cmath>
#include <ctime>
#include <cstdio>

#include "app_config.h"
#include "i18n.h"
#include "app_main_exports.h"
#include "audio.h"
#include "battery.h"
#include "ble.h"
#include "ble_store.h"
#include "camera.h"
#include "cb/mode_fsm.h"
#include "cb/power_mode.h"
#include "cb_pm.h"
#include "cb_ds.h"
#include "config.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "diag.h"
#include "esp_log.h"
#include "glitchtip.h"
#include "esp_attr.h"
#include "esp_task_wdt.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "mqtt.h"
#include "nvs_flash.h"
#include "dns_hijack.h"
#include "oled.h"
#include "ota.h"
#include "sensors.h"
#include "tls_enroll.h"
#include "pcm.h"
#include "pir.h"
#include "reed.h"
#include "soil.h"
#include "sonar.h"
#include "speaker.h"
#include "uart_servo.h"
#include "sd_storage.h"
#include "sdkconfig.h"
#include "status_led.h"
#include "text_util.h"
#include "tls_store.h"
#include "wifi_mgr.h"
#include "wifi_store.h"
#include "net_store.h"
#include "auth_store.h"
#if CONFIG_CHYTRA_BUDKA_BOOT_BUTTON_RESET
#include "driver/gpio.h"
#endif

namespace {

constexpr const char *TAG = "chytra-budka";

cb::Profile s_profile = cb::Profile::Boot;
/* Whether the mic pipeline is currently meant to run (profile + audio window).
 * Written by apply_power_state(), refreshed onto state/audio_active each
 * telemetry tick so the retained value self-heals after a boot where the
 * first publish raced MQTT-connect. Supervisor-thread-only — no atomic. */
bool s_audio_active = false;
/* Atomic mirror of s_profile, exported via app_mode_current() so audio_task
 * (CPU1) and camera_worker (CPU1) can read the supervisor's profile without
 * a torn read across cores. enter_profile() updates both — s_profile stays as
 * the supervisor's local cache for branch-heavy code paths in this TU. */
std::atomic<int> g_mode_atomic{static_cast<int>(cb::Profile::Boot)};
/* Power-ladder hysteresis thresholds are NVS-backed (soc_max_en/soc_act_en/
 * soc_eco_en + soc_hib_en, defaults in app_config.c) and read LIVE in
 * profile_tick(), so a deployment can retune battery behaviour over the air
 * without a reflash. */

int64_t s_next_telemetry_us = 0;
int64_t s_next_mode_check_us = 0;
#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
/* Set by GET /debug/hang to stress-test the TWDT pipeline end-to-end.
 * Main loop spins without yielding or feeding the watchdog until this
 * deadline, which forces a TWDT panic + coredump + reboot. */
std::atomic<int64_t> s_hang_main_until_us{0};

}  // anonymous namespace

extern "C" void debug_hang_main_for_ms(int ms) {
    int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    s_hang_main_until_us.store(deadline);
}

namespace {
#endif                        /* CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS */
/* Per-PIR-instance hold-down state. _0 keeps the singleton-era
 * topic/discovery for backward compat with existing HA dashboards.
 * PIR_MAX_INSTANCES (4) sized arrays so a multi-PIR board has
 * independent timers + active flags per sensor. */
int64_t s_motion_off_us[PIR_MAX_INSTANCES] = {0};
bool s_motion_active[PIR_MAX_INSTANCES] = {false};

/* PIR hold: keep binary_sensor.motion="ON" for this long after each edge. */
constexpr int64_t MOTION_HOLD_MS = 10000;

/* How long app_main blocks for the first STA association before continuing the
 * boot anyway (network-dependent services retry on their own afterwards). */
constexpr uint32_t WIFI_STA_WAIT_MS = 20000;

/* OTA rollback safety. Mark the running image valid only after it has run for
 * MARK_VALID_DELAY_US *and* the MQTT control plane is up (so a bad image can
 * still receive a corrective OTA). If MQTT isn't up at the deadline, defer by
 * MARK_VALID_RETRY_US rather than refuse forever. */
constexpr int64_t MARK_VALID_DELAY_US = 180LL * 1000 * 1000;  // 180 s
constexpr int64_t MARK_VALID_RETRY_US = 30LL * 1000 * 1000;   // 30 s

void telemetry_period_for_profile() {
    int64_t period_s;
    switch (s_profile) {
        case cb::Profile::Max:       period_s = app_config_get_int("tlm_max_s"); break;
        case cb::Profile::Sentinel:  period_s = app_config_get_int("tlm_low_s"); break;
        /* Hibernate publishes once per wake — cadence is the wake interval. */
        case cb::Profile::Hibernate: period_s = app_config_get_int("ds_sleep_s"); break;
        /* Active + Eco + Boot share the mid cadence. */
        default:                     period_s = app_config_get_int("tlm_mid_s"); break;
    }
    if (period_s < 5)
        period_s = 5;
    s_next_telemetry_us = esp_timer_get_time() + period_s * 1000000;
}

void publish_full_telemetry() {
    float soc = battery_soc();
    float vbat = battery_vbat();
    float crate = battery_charge_rate();
    int rssi = wifi_mgr_rssi();
    /* Recover any SHT41 whose one-shot boot probe lost to a transient
     * flaky-bus window (bus0 now carries several devices). The registry
     * re-probes its not-ready instances once a minute, firing immediately on
     * the first publish so the operator's "just plugged it in" case recovers
     * fast, not after 60 s. */
    int64_t now_us = esp_timer_get_time();
    cb_sensors_retry_absent();
    if (!battery_ready()) {
        /* Same recovery for the MAX17048 fuel gauge — so a hot-plugged
         * gauge (the bench just gained one) is picked up within a minute
         * instead of needing a reboot. battery_init is re-entrant. */
        static int64_t s_bat_reprobe_us = 0;
        if (now_us >= s_bat_reprobe_us) {
            s_bat_reprobe_us = now_us + 60LL * 1000000LL;
            if (battery_init() == ESP_OK)
                ESP_LOGI(TAG, "MAX17048 recovered on periodic re-probe");
        }
    }

    /* System metrics keep their dedicated path; temp/humidity now flow from
     * the sensor registry (pass NaN so this doesn't double-publish them). */
    mqtt_publish_telemetry(soc, vbat, crate, rssi, NAN, NAN);

    /* All physical I²C sensors (SHT41×N + BMP388 + future), uniformly: one
     * cache refresh per present sensor, then publish every channel from the
     * registry. Adding a sensor needs no change here. */
    cb_sensors_refresh();
    mqtt_publish_sensors();

    /* Reed switch: periodic refresh so HA sees the current door/lid
     * state after a reconnect even if no edges fired since the last
     * tick. Event-driven publishes from the main loop handle live
     * changes; this is the "still here, still closed/open" heartbeat. */
    /* Periodic refresh for every armed reed instance so HA sees the
     * current state even when no events fired since the last tick.
     * Empty loop body for boards without reeds (reed_active_count()=0). */
    for (int ri = 0; ri < reed_active_count(); ri++) {
        mqtt_publish_reed_nth(ri, reed_is_closed_nth(ri));
        mqtt_publish_reed_count_nth(ri, reed_event_count_nth(ri));
    }

    /* Grove sensors. Both publish from their own poll tasks; this is
     * the post-reconnect heartbeat re-sending the cached values (the
     * state topics are non-retained). 999 = the "clear" sentinel
     * (nothing in range), matching the poll task's publishes. */
    {
        float cm;
        if (sonar_ready() && sonar_last_cm(&cm))
            mqtt_publish_distance(cm);
        else if (sonar_ready() && sonar_is_clear())
            mqtt_publish_distance(999.0f);
        float soil_mv, soil_pct;
        if (soil_ready() && soil_last(&soil_mv, &soil_pct))
            mqtt_publish_soil(soil_mv, soil_pct);
    }

    /* Ambient AGC: sampled here (not in the hot capture path) so the
     * operator can watch the gain curve through the day and tune
     * ir_agc_thresh against real dusk/dawn data. Skips publish when
     * sensor read fails (-1 sentinel). */
    if (camera_ready()) {
        mqtt_publish_ambient_agc(camera_get_agc_gain());
    }

    /* reset_reason is retained — send once after first MQTT connect. */
    static bool s_reset_sent = false;
    const char *rr = nullptr;
    if (!s_reset_sent && mqtt_is_connected()) {
        switch (esp_reset_reason()) {
            case ESP_RST_POWERON:
                rr = "poweron";
                break;
            case ESP_RST_EXT:
                rr = "ext";
                break;
            case ESP_RST_SW:
                rr = "sw";
                break;
            case ESP_RST_PANIC:
                rr = "panic";
                break;
            case ESP_RST_INT_WDT:
                rr = "int_wdt";
                break;
            case ESP_RST_TASK_WDT:
                rr = "task_wdt";
                break;
            case ESP_RST_WDT:
                rr = "wdt";
                break;
            case ESP_RST_DEEPSLEEP:
                rr = "deepsleep";
                break;
            case ESP_RST_BROWNOUT:
                rr = "brownout";
                break;
            case ESP_RST_SDIO:
                rr = "sdio";
                break;
            default:
                rr = "unknown";
                break;
        }
        s_reset_sent = true;
    }
    /* fw_version is retained — publish once after the first MQTT
     * connect. Payload is static for the lifetime of the running
     * image, so re-sending it on every telemetry tick would just
     * waste broker bandwidth. */
    /* mqtt_publish_fw_version() carries its own once-per-boot guard
     * (the topic is retained, no point re-publishing every tick). */
    if (mqtt_is_connected()) {
        mqtt_publish_fw_version();
    }
    mqtt_publish_diag((uint32_t)esp_get_free_heap_size(),
                      (uint32_t)(esp_timer_get_time() / 1000000), rr, diag_mcu_temp_c());
    if (audio_streaming() || audio_burst_count() > 0) {
        mqtt_publish_audio_telemetry(audio_last_rms_dbfs(), audio_burst_count(),
                                     audio_chunks_sent(), audio_streaming());
    }
    /* Refresh the mic-on/off state (mode + audio window) so the retained
     * topic self-heals if the boot-time publish raced MQTT-connect. */
    mqtt_publish_audio_active(s_audio_active);

    /* Solar V/I/P from INA226 — published from the registry cache that
     * cb_sensors_refresh() populated above (no second live read). Skipped
     * when the sensor is absent (cb_sensor_chan returns false). */
    float bv = NAN, ia = NAN, pw = NAN;
    if (cb_sensor_chan("ina", "solar_v", &bv) &&
        cb_sensor_chan("ina", "solar_i", &ia) &&
        cb_sensor_chan("ina", "solar_p", &pw)) {
        mqtt_publish_solar(bv, ia, pw);
    }
    float t_c = NAN, rh_pct = NAN;
    (void)cb_sensor_chan("sht0", "temp", &t_c);      /* on-board SHT41, for the log line */
    (void)cb_sensor_chan("sht0", "humidity", &rh_pct);
    ESP_LOGI(TAG,
             "tlm soc=%.1f vbat=%.3f crate=%.2f rssi=%d t=%.1f rh=%.1f mode=%s "
             "rms=%.1f bursts=%" PRIu32 " chunks=%" PRIu32
             " streaming=%d "
             "heap=%" PRIu32,
             soc, vbat, crate, rssi, t_c, rh_pct, cb::profile_name(s_profile), audio_last_rms_dbfs(),
             audio_burst_count(), audio_chunks_sent(), (int)audio_streaming(),
             esp_get_free_heap_size());

    /* Stack high-water marks for our long-lived tasks — useful to catch
     * a slow drift toward overflow before it panics. One line per
     * telemetry tick (i.e. once per mode-dependent period). */
    diag_log_task_stacks();
}

void enter_profile(cb::Profile p) {
    if (p == s_profile)
        return;
    ESP_LOGI(TAG, "profile: %s → %s", cb::profile_name(s_profile), cb::profile_name(p));
    s_profile = p;
    g_mode_atomic.store(static_cast<int>(p), std::memory_order_release);
    mqtt_publish_profile(cb::profile_name(p));

    /* Audio-engine lifecycle + WiFi power-save are NOT applied here — they
     * depend on the audio active-hours window too, which can flip without a
     * profile change. apply_power_state() (called every 1 Hz tick right after
     * profile_tick()) is the single writer of both. enter_profile() only owns
     * the profile-identity side effects (publish + telemetry cadence). */
    telemetry_period_for_profile();
}

/* Is the audio active-hours window open right now (local wall clock)?
 * Fails OPEN before SNTP sync (epoch < ~2023-11) so a cold-boot or wedged-
 * clock unit never silences the mic or perturbs WiFi PS on a bad clock. */
bool audio_window_open_now() {
    time_t now = time(NULL);
    if (now < CB_CLOCK_SYNCED_EPOCH) return true;  // clock not yet synced → always open
    struct tm lt;
    localtime_r(&now, &lt);
    return cb::audio_window_open(lt.tm_hour, app_config_get_int("audio_on_h"),
                                 app_config_get_int("audio_off_h"));
}

/* Single writer of the runtime power levers: audio-engine lifecycle + WiFi
 * power-save. Called every 1 Hz supervisor tick (after profile_tick) so a window
 * open/close takes effect even with no mode change. All reads are from the
 * supervisor thread; the static change-guards are single-thread, no locking.
 *   - Audio runs only in Continuous/Triggered AND inside the active-hours
 *     window. Outside it (or in Safe/Boot) the mic is torn down — the only
 *     lever that meaningfully cuts the flat ~1.1 W baseline (see POWER.md).
 *   - WiFi PS: PS_NONE only while a Continuous stream is actually live (so the
 *     relay isn't paced by DTIM); Safe dozes hardest (MAX_MODEM); everything
 *     else MIN_MODEM. WiFi stays ASSOCIATED in every mode (field is OTA-only). */
void apply_power_state() {
    const cb::Profile p = s_profile;
    /* Audio runs only in the non-sleeping tiers (Max streams continuously,
     * Active streams per-burst; BOTH fire the VAD photo+event trigger) AND
     * inside the active-hours window. Eco/Sentinel/Hibernate force
     * audio OFF — the mic's continuous I2S holds cb_pm's NO_LIGHT_SLEEP lock,
     * so light/deep sleep can't engage while it runs. */
    const bool profile_wants_audio =
        (p == cb::Profile::Max || p == cb::Profile::Active);
    const bool want_audio = profile_wants_audio && audio_window_open_now();

    s_audio_active = want_audio;  // telemetry tick refreshes the retained topic
    static int s_last_want_audio = -1;  // -1 = force first apply
    if ((int)want_audio != s_last_want_audio) {
        s_last_want_audio = (int)want_audio;
        if (want_audio)
            audio_begin();
        else
            audio_end();
        mqtt_publish_audio_active(want_audio);  // immediate on change (HA + HIL)
    }

    /* WiFi power-save per tier: NONE only while a Max stream is actually live
     * (so the relay isn't paced by DTIM); the light-sleep tiers (Eco/Sentinel)
     * doze hardest (MAX_MODEM); Active uses MIN_MODEM. WiFi stays ASSOCIATED in
     * every awake tier (field is OTA-only). Hibernate tears WiFi down via deep
     * sleep, so its PS choice here is moot. */
    wifi_ps_type_t ps = (p == cb::Profile::Max && want_audio) ? WIFI_PS_NONE
                      : (p == cb::Profile::Eco ||
                         p == cb::Profile::Sentinel)          ? WIFI_PS_MAX_MODEM
                                                              : WIFI_PS_MIN_MODEM;
    static wifi_ps_type_t s_last_ps = (wifi_ps_type_t)-1;
    if (ps != s_last_ps) {
        s_last_ps = ps;
        esp_err_t pse = esp_wifi_set_ps(ps);
        if (pse != ESP_OK)
            ESP_LOGW(TAG, "esp_wifi_set_ps(%d): %s", (int)ps, esp_err_to_name(pse));
    }

    /* Automatic light-sleep is now a pure function of the tier: Eco/Sentinel
     * engage it (audio is off by definition there, so the NO_LIGHT_SLEEP lock
     * is free); Max/Active never; Hibernate is deep sleep (handled by cb_ds at
     * the loop tail, not here). No-op unless CONFIG_PM_ENABLE; change-guarded
     * inside cb_pm. */
    cb_pm_set_lightsleep(p == cb::Profile::Eco || p == cb::Profile::Sentinel);
}

void profile_tick() {
    /* Manual selector (NVS-tunable) wins over the SOC ladder. 0=auto so
     * production behaviour is the battery-driven ladder; bench / no-battery
     * test rigs (or a deliberate hibernate deployment) pin a tier here.
     * Schema clamps to 0..5; 1..5 map to the Profile enum. */
    int32_t sel = app_config_get_int("power_profile");
    if (sel > 0) {
        cb::Profile forced = (sel == 1) ? cb::Profile::Max
                           : (sel == 2) ? cb::Profile::Active
                           : (sel == 3) ? cb::Profile::Eco
                           : (sel == 4) ? cb::Profile::Sentinel
                                        : cb::Profile::Hibernate;
        if (forced != s_profile)
            enter_profile(forced);
        return;
    }

    /* Re-evaluate the tier from current SOC, with hysteresis. */
    float soc = battery_soc();
    if (soc < 0.0f) {
        /* No battery detected (USB/bench) — seed Active on first tick, then
         * stay put. Never auto-descend into a sleeping tier on a mains unit. */
        if (s_profile == cb::Profile::Boot)
            enter_profile(cb::Profile::Active);
        return;
    }
    /* Live NVS-backed ENTER thresholds (cheap — cache reads, not flash). Derive
     * each leave-threshold by subtracting the boundary's default hysteresis gap
     * (max −10, act −7, eco −6), preserving the FSM's tested hysteresis when
     * knobs sit at defaults. soc_hib_en==0 floors the auto ladder at Sentinel. */
    auto clampf = [](float v) { return v < 0.0f ? 0.0f : v; };
    const float max_en = (float)app_config_get_int("soc_max_en");
    const float act_en = (float)app_config_get_int("soc_act_en");
    const float eco_en = (float)app_config_get_int("soc_eco_en");
    cb::ProfileThresholds thr;
    thr.max_enter = max_en;  thr.max_leave = clampf(max_en - 10.0f);
    thr.act_enter = act_en;  thr.act_leave = clampf(act_en - 7.0f);
    thr.eco_enter = eco_en;  thr.eco_leave = clampf(eco_en - 6.0f);
    thr.hib_enter = (float)app_config_get_int("soc_hib_en");
    cb::Profile next = cb::next_profile(s_profile, soc, thr);
    if (next != s_profile)
        enter_profile(next);
}

}  // namespace

/* Cross-core accessor for the supervisor's profile. Audio task on CPU1
 * polls this every iteration; camera worker is profile-agnostic but the
 * accessor is shared. Returns the underlying cb::Profile integer (cast
 * back to enum at the call site). */
extern "C" int app_mode_current(void) {
    return g_mode_atomic.load(std::memory_order_acquire);
}

/* Light-sleeping posture: the resolved profile is Eco or Sentinel (the tiers
 * where automatic light-sleep is engaged — audio is off there). Poll tasks read
 * this to lengthen their delay so the CPU stays in light sleep between the WiFi
 * DTIM wakes instead of being woken every 20-100 ms by a poll. Reads the atomic
 * profile — safe from any task/core. (Hibernate is deep sleep, handled by cb_ds;
 * Max/Active never light-sleep.) */
extern "C" bool app_profile_sleeps(void) {
    int p = g_mode_atomic.load(std::memory_order_acquire);
    return p == (int)cb::Profile::Eco || p == (int)cb::Profile::Sentinel;
}

/* True iff the resolved profile is Hibernate. Lets cb_ds (a C TU) gate the
 * deep-sleep decision without seeing cb::Profile. Cross-core safe. */
extern "C" bool app_profile_is_hibernate(void) {
    return g_mode_atomic.load(std::memory_order_acquire) == (int)cb::Profile::Hibernate;
}

/* Self-test: probe each peripheral and publish a JSON summary. Called
 * once shortly after MQTT connects, and again on demand from /selftest. */
extern "C" void selftest_run_and_publish(char *out, size_t out_sz) {
    /* Single source of truth for the selftest size + required-vs-
     * optional split. Adding a sensor = appending one row here; the
     * "%d/N" count, the JSON keys, and the "ok vs degraded" verdict
     * all derive from this table — no hardcoded totals anywhere. */
    struct {
        const char *key;
        bool        ok;
        bool        required;
    } checks[] = {
        {"battery",   battery_ready(),         false}, /* optional: USB-only boards have no gauge */
        {"sht41",     cb_sensor_ready("sht0"), true},
        {"sht41_ext", cb_sensor_ready("sht1"), false}, /* optional outside sensor */
        {"bmp388",    cb_sensor_ready("bmp"),  false}, /* pressure/temp; bench + planned field */
        {"ina226",    cb_sensor_ready("ina"),  false}, /* solar shunt; not yet ordered */
        {"sd",        sd_storage_ready(),      true},
        {"camera",    camera_ready(),          true},
        {"pir",       pir_ready(),             true},
        {"reed",      reed_ready(),            false}, /* optional door/lid contact */
        {"mic",       audio_ready(),           true},
        /* The two hot-path tasks pinned to CPU1. Both `required` so a
         * silent xTaskCreate OOM at boot surfaces as "degraded" in
         * selftest instead of presenting as "no audio AND no captures
         * with no obvious cause". */
        {"audio_task",  audio_task_running(),     true},
        {"cam_worker",  camera_worker_running(),  true},
        {"wifi",      wifi_mgr_is_connected(), true},
        {"mqtt",      mqtt_is_connected(),     true},
        {"uart_servo", uart_servo_ready(),      false}, /* optional UART bus */
    };
    const int total = (int)(sizeof(checks) / sizeof(checks[0]));
    int n_ok = 0;
    bool all_required = true;
    for (int i = 0; i < total; i++) {
        if (checks[i].ok) n_ok++;
        if (checks[i].required && !checks[i].ok) all_required = false;
    }

    /* 640, not 512: 15 checks + pir_wedged + the photo-queue block + the SD
     * capacity/prune block can reach ~530 B with large counters, which would
     * truncate the last (SD) field into invalid JSON exactly when a card is
     * mounted. text_append clamps safely, but we want the whole object. */
    char json[704];
    char *jp = json;
    size_t left = sizeof(json);
    text_append(&jp, &left, "{\"summary\":\"%s (%d/%d)\"",
                all_required ? "ok" : "degraded", n_ok, total);
    for (int i = 0; i < total; i++) {
        if (!text_append(&jp, &left, ",\"%s\":%s",
                         checks[i].key, checks[i].ok ? "true" : "false"))
            break;
    }
    /* Extra diagnostic field: surfaces a stuck-HIGH PIR sensor that
     * the boolean `pir` check can't distinguish from "no sensor wired".
     * Optional consumers ignore unknown keys; HA / scripts that care
     * about pinpointing a broken sensor can key off this. */
    text_append(&jp, &left, ",\"pir_wedged\":%s",
                pir_wedged() ? "true" : "false");
    /* Optional BLE meter scan status (off / scanning,no-uc96 / uc96 <N>s ago).
     * "off" when not compiled in or ble_enabled=false — never a fault. */
    char ble_st[24];
    ble_status(ble_st, sizeof(ble_st));
    text_append(&jp, &left, ",\"ble\":\"%s\"", ble_st);
    /* Audio outputs — resolved GPIO each backend init'd on, or -1 if no pad is
     * mapped to "buzzer"/"pcm" (the default — never a fault). Lets a script /
     * the HIL confirm a wired audio variant came up on the expected pads. */
    text_append(&jp, &left, ",\"audio_buzzer_gpio\":%d,\"audio_pcm_gpio\":%d",
                speaker_gpio(), pcm_gpio());
    /* Internal DMA-capable DRAM headroom — the scarce pool the BT controller,
     * i2s/camera DMA and lwip all draw from (PSRAM can't serve DMA). The
     * PSRAM-inclusive esp_get_free_heap_size() on the status page HID the
     * exhaustion that broke the BLE bring-up (i2s DMA alloc failed → audio
     * task wedge → task_wdt). dma_largest = biggest contiguous DMA block, the
     * number that actually gates a DMA buffer allocation. */
    text_append(&jp, &left,
                ",\"int_free\":%u,\"dma_free\":%u,\"dma_largest\":%u",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    /* Photo retry-queue telemetry. depth=current entries, dropped=FIFO
     * overflows since boot (real outage indicator — if >0 the box was
     * offline long enough to lose shots), drained=successful re-
     * publishes after a reconnect. HA dashboard alerts on dropped>0. */
    extern size_t photo_queue_depth(void);
    extern uint32_t photo_queue_dropped_total(void);
    extern uint32_t photo_queue_drained_total(void);
    text_append(&jp, &left,
                ",\"photo_queue_depth\":%u,"
                "\"photo_queue_dropped\":%" PRIu32 ","
                "\"photo_queue_drained\":%" PRIu32 ","
                "\"cam_request_drops\":%" PRIu32 ","
                "\"photo_publish_errors\":%" PRIu32,
                (unsigned)photo_queue_depth(),
                photo_queue_dropped_total(),
                photo_queue_drained_total(),
                camera_request_drops_total(),
                mqtt_photo_publish_errors_total());

    /* SD-card capacity surfacing: only emit when mounted, so a board
     * without SD doesn't pollute the JSON with a 0% reading. */
    uint64_t sd_free = 0, sd_total = 0;
    sd_storage_stats(&sd_free, &sd_total);
    if (sd_total > 0) {
        uint64_t used = sd_total - sd_free;
        unsigned free_pct = (unsigned)((sd_free * 100ULL) / sd_total);
        uint32_t pruned_files = 0;
        uint64_t pruned_bytes = 0;
        sd_storage_pruned_stats(&pruned_files, &pruned_bytes);
        text_append(&jp, &left,
                    ",\"sd_free_pct\":%u,"
                    "\"sd_used_mb\":%llu,"
                    "\"sd_total_mb\":%llu,"
                    "\"sd_pruned_files\":%" PRIu32 ","
                    "\"sd_pruned_mb\":%llu",
                    free_pct,
                    used / (1024ULL * 1024ULL),
                    sd_total / (1024ULL * 1024ULL),
                    pruned_files,
                    pruned_bytes / (1024ULL * 1024ULL));
    }
    /* Post-boot mic + camera death detection — computed BEFORE closing the
     * JSON so mic_stalled/cam_stalled ship as fields (cbprom maps them to
     * Prometheus for alerting), not only into the GlitchTip degraded summary.
     * mic: audio_ready() stays true on its lifetime>0 check, so a mic that
     * loses contact after boot (bad B2B connector) goes unnoticed; in
     * Continuous mode (the only mode that pumps the mic every loop) a flat
     * frame counter vs the previous selftest means it stopped delivering.
     * camera: camera_ready() stays true after init, so a wedged sensor keeps
     * reporting "ok" while every capture fails — flag it when failures advanced
     * but successes didn't (attempts happened, none succeeded). Both fire only
     * when work is actually attempted, so an idle mic/camera never false-alarms.
     * Black/frozen-frame content detection stays downstream in HA (NOTES.md). */
    static uint32_t s_last_frames = 0;
    static bool s_frames_armed = false;
    bool mic_stalled = false;
    if ((cb::Profile)app_mode_current() == cb::Profile::Max && audio_task_running()) {
        uint32_t fr = audio_frames_captured();
        if (s_frames_armed && fr == s_last_frames)
            mic_stalled = true;
        s_last_frames = fr;
        s_frames_armed = true;
    } else {
        s_frames_armed = false;  // re-arm on next entry to Continuous
    }
    static uint32_t s_last_cam_ok = 0, s_last_cam_fail = 0;
    static bool s_cam_armed = false;
    bool cam_stalled = false;
    if (camera_ready()) {
        uint32_t cok = camera_capture_count();
        uint32_t cfail = camera_capture_failures();
        if (s_cam_armed && cfail > s_last_cam_fail && cok == s_last_cam_ok)
            cam_stalled = true;
        s_last_cam_ok = cok;
        s_last_cam_fail = cfail;
        s_cam_armed = true;
    } else {
        s_cam_armed = false;
    }
    text_append(&jp, &left, ",\"mic_stalled\":%s,\"cam_stalled\":%s",
                mic_stalled ? "true" : "false", cam_stalled ? "true" : "false");

    /* Close the JSON object. text_append guarantees *left == 0 means
     * truncation already occurred and the buffer is NUL-terminated by
     * the last vsnprintf write; otherwise we still have room for '}'. */
    if (left > 1) { *jp++ = '}'; *jp = '\0'; }
    else { json[sizeof(json) - 1] = '\0'; }

    mqtt_publish_selftest(json);
    ESP_LOGI("selftest", "%s", json);

    /* Escalate a degraded verdict to GlitchTip. The selftest is the one
     * place HW health is aggregated truthfully, but the lines above only
     * reach a retained MQTT topic + INFO log — invisible to the GlitchTip
     * hook, which ships ESP_LOGE only. For an OTA-only field board that
     * means a dead camera / mic / SD is silent off-box (nobody may be
     * watching the topic). Emit ONE event per change of the failed-required
     * set: a CONSTANT message ("selftest degraded") so GlitchTip groups every
     * degrade into a single issue, with the variable failed-key list carried
     * as a `failed` tag (filterable, doesn't fragment the title); edge-
     * triggered so a persistent fault doesn't re-fire every telemetry tick.
     * Recovery clears the tracker so
     * the next degrade re-arms; recovery itself is not shipped as an error.
     * (mic_stalled/cam_stalled were computed above, before the JSON close.) */
    static char s_last_degraded[176] = {0};
    char degraded[176];
    char *dp = degraded;
    size_t dleft = sizeof(degraded);
    for (int i = 0; i < total; i++) {
        if (checks[i].required && !checks[i].ok)
            text_append(&dp, &dleft, "%s%s",
                        dp == degraded ? "" : " ", checks[i].key);
    }
    if (mic_stalled)
        text_append(&dp, &dleft, "%smic_stalled", dp == degraded ? "" : " ");
    if (cam_stalled)
        text_append(&dp, &dleft, "%scam_stalled", dp == degraded ? "" : " ");

    if (strcmp(degraded, s_last_degraded) != 0) {
        strncpy(s_last_degraded, degraded, sizeof(s_last_degraded) - 1);
        s_last_degraded[sizeof(s_last_degraded) - 1] = 0;
        if (degraded[0]) {
            /* Serial at WARN so the GlitchTip log hook (ERROR-only) doesn't
             * double-report, plus an explicit hook-INDEPENDENT GlitchTip
             * event. The hook isn't installed until the 180 s mark-valid
             * and the boot selftest runs well before that, so a boot-time
             * degraded verdict would otherwise never ship. */
            ESP_LOGW("selftest", "degraded: %s", degraded);
            if (glitchtip_ready()) {
                /* Constant message (groups cleanly); failed keys as a tag.
                 * `degraded` is a space-joined list of compile-time check
                 * keys (camera/sd/wifi/… + mic_stalled/cam_stalled) — all
                 * bare identifiers, so no JSON escaping is needed. */
                char extra[208];
                snprintf(extra, sizeof(extra), "\"failed\":\"%s\"", degraded);
                glitchtip_report("error", "selftest degraded", extra);
            }
        }
    }

    if (out && out_sz) {
        strncpy(out, json, out_sz - 1);
        out[out_sz - 1] = 0;
    }
}

#if CONFIG_CHYTRA_BUDKA_BOOT_BUTTON_RESET
/* Falling-edge latch for the BOOT button. A human tap (~50–150 ms) is shorter
 * than the task's poll interval (200 ms, 500 ms in light-sleep), so polling
 * alone misses most taps; this ISR records every press so a quick tap still
 * registers (consumed in boot_button_task to cycle the OLED page). */
static volatile bool s_boot_press_edge = false;
static void IRAM_ATTR boot_btn_isr(void *arg) {
    (void)arg;
    s_boot_press_edge = true;
}

/* XIAO BOOT-button (GPIO0) factory-reset monitor — see Kconfig help.
 * Runs in ALL modes incl. safe mode (recovery is most needed then). */
static void boot_button_task(void *arg) {
    (void)arg;
    /* GPIO0 = BOOT button + strapping pin. Read only at runtime (the strap
     * is already latched at reset), pressed = LOW with the internal pull-up.
     * A negedge interrupt also latches presses too brief for the poll to see. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << 0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE)  /* may already be up */
        ESP_LOGW(TAG, "boot-button ISR service: %s", esp_err_to_name(isr_err));
    gpio_isr_handler_add(GPIO_NUM_0, boot_btn_isr, nullptr);

    /* Optional external push-button: whatever pin is mapped to "button" in the
     * pin map (wire the button active-low, between the pin and GND). Treated
     * exactly like the onboard BOOT button — a short tap cycles the OLED page,
     * a 10 s hold factory-resets — so an easier-to-press button can stand in for
     * the tiny BOOT one. -1 when unmapped. */
    int btn_pin = app_config_pin_for_first("button");
    if (btn_pin >= 0) {
        gpio_config_t b = {
            .pin_bit_mask = 1ULL << btn_pin,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&b);
        gpio_isr_handler_add((gpio_num_t)btn_pin, boot_btn_isr, nullptr);
        ESP_LOGI(TAG, "external 'button' on GPIO%d", btn_pin);
    }

    const int HOLD_MS = 10000;       /* deliberate long hold = factory reset */
    const int TAP_MS  = 900;         /* release at/under this = short tap → cycle OLED page */
    const int RESET_HINT_MS = 1000;  /* past this, surface the OLED "hold = reset" bar */
    const int FAST_MS = 25;          /* fine poll: clean debounce + sharp press timing */
    const int64_t GAP_US = 4000000;  /* forgiving cadence window between short presses */

    bool    down   = false;          /* debounced button level */
    int     stable = 0;              /* consecutive raw samples agreeing on a change */
    int64_t t_down = 0, t_up = 0;    /* timestamps of the last confirmed edges (µs) */
    bool    led = false, bar = false, swallow = false;
    uint8_t pat = 0;                 /* rolling short-press cadence */
    int     pn  = 0;
    while (true) {
        bool raw = (gpio_get_level(GPIO_NUM_0) == 0) ||         /* BOOT or external */
                   (btn_pin >= 0 && gpio_get_level((gpio_num_t)btn_pin) == 0);
        s_boot_press_edge = false;   /* ISR backstop; the fine poll catches presses */

        /* Debounce: accept a level change only after two agreeing samples
         * (~50 ms), and timestamp the confirmed edges so press/gap lengths are
         * measured precisely instead of quantised to the poll interval. */
        if (raw != down) {
            if (++stable >= 2) {
                stable = 0;
                down = raw;
                int64_t now = esp_timer_get_time();
                if (down) {
                    if (oled_anim_running()) { oled_anim_stop(); swallow = true; }
                    if (t_up && now - t_up > GAP_US) { pat = 0; pn = 0; }
                    t_down = now;
                } else {
                    int dur = (int)((now - t_down) / 1000);
                    t_up = now;
                    if (bar) { oled_set_reset_progress(-1, 0); bar = false; }
                    if (led) { gpio_set_level((gpio_num_t)STATUS_LED_PIN, 1); led = false; }
                    if (swallow) {
                        swallow = false;             /* the press that dismissed a flap — eat it */
                    } else if (dur <= TAP_MS) {
                        oled_next_page();
                        ds_note_activity();   /* hibernate: a button tap extends the wake window */
                        pat = (uint8_t)((pat << 1) | (dur >= 350 ? 1u : 0u));
                        if (++pn >= 8 && pat == 0xA8u) { oled_anim_logo(); pat = 0; pn = 0; }
                    } else if (dur >= 3000) {
                        ESP_LOGI(TAG, "BOOT released after %d ms — no reset", dur);
                    }
                }
            }
        } else {
            stable = 0;
        }

        if (down) {
            int held = (int)((esp_timer_get_time() - t_down) / 1000);
            /* Past RESET_HINT_MS, fill the OLED "HOLD = FACTORY RESET" bar so the
             * destructive gesture is unmistakable before it fires; past 3 s blink
             * the onboard LED as "keep holding" feedback. */
            if (held >= RESET_HINT_MS) { oled_set_reset_progress(held, HOLD_MS); bar = true; }
            if (held >= 3000) {
                led = !led;
                gpio_set_level((gpio_num_t)STATUS_LED_PIN, led ? 0 : 1);
            }
            if (held >= HOLD_MS) {
                ESP_LOGE(TAG, "BOOT held %d s — FULL FACTORY RESET", HOLD_MS / 1000);
                gpio_set_level((gpio_num_t)STATUS_LED_PIN, 0); /* solid = committing */
                oled_set_reboot_reason("FACTORY RESET");
                /* Same order as cmd/factory_reset: config + TLS first,
                 * WiFi last, then reboot. net_store included — leaving a
                 * stored broker behind after erasing the client cert it needs
                 * strands the board (see the mqtt.c handler). */
                app_config_reset_defaults();
                tls_store_erase();
                net_store_erase();
                wifi_store_erase();
                auth_store_erase();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        } else if (pn > 0 && esp_timer_get_time() - t_up > GAP_US) {
            pat = 0; pn = 0;
        }

        /* Fine poll while pressed or mid-cadence so timing stays sharp; the box
         * never deep-sleeps, but honour the deep-save hook if it's ever active. */
        int delay_ms = (app_profile_sleeps() && !down && pn == 0) ? 500 : FAST_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
#endif

/* SD retention (autoprune) + legacy flat-root migration, on its OWN low-prio,
 * NON-WDT-subscribed task. Deliberately isolated from BOTH the capture worker
 * (cam_wrk) AND the supervisor loop: deleting a deep backlog can take seconds-
 * to-minutes on a slow/dying card (unlinks are FAT-table writes, ~1 s each when
 * the card is tired), and that must NEVER stall captures or telemetry, nor trip
 * a watchdog reboot loop. Here a slow — or even wedged — fs op only delays this
 * one task; everything else runs regardless. Small per-pass budget so a backlog
 * drains gently in the background. */
static void sd_maintenance_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (!sd_storage_ready()) continue;
        if (app_config_get_bool("sd_autoprune"))
            sd_storage_autoprune((int)app_config_get_int("sd_min_free"),
                                 (int)app_config_get_int("sd_keep_days"),
                                 /*max_files*/ 30);
        sd_storage_migrate_step(32);
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "── Chytrá Budka boot (Stage C) ──");
    ESP_LOGI(TAG, "ESP-IDF %s", esp_get_idf_version());

    /* Capture reset reason + coredump state BEFORE anything touches NVS
     * or OTA. The boot-fail counter in RTC slow memory will warn us if
     * we're in a crash loop so the main loop can ramp down to safe mode. */
    diag_capture_boot();

    /* Classify the deep-sleep wake cause (timer vs PIR vs cold boot) and bump
     * the RTC-persisted hibernate counters. Must run after diag_capture_boot()
     * (which reads the reset reason) and before the main loop. On a clean
     * deep-sleep wake it also clears the consecutive-crash counter — a completed
     * sleep→wake cycle is proof of a non-crashing run, and the 180 s uptime
     * crash-clear below never elapses inside a short hibernate wake window. */
    ds_capture_wake();

    /* Crash-loop safe mode: if we've crash-reset several times in a row
     * without ever reaching a clean run, boot a MINIMAL control-plane-only
     * profile — skip the heavy camera + audio subsystems (biggest PSRAM/
     * DMA/I2S consumers and the most likely crash culprits) but keep
     * WiFi / MQTT / OTA / HTTP up so an operator can push a corrective OTA
     * to the unreflashable field unit. The bootloader already rolls a bad
     * *pending* image back on the first crash; this is the safety net for
     * an already-VALID image that starts crash-looping (a config or
     * environmental trigger), where there is nothing to roll back to. A
     * clean 180 s run clears the counter, so the next boot automatically
     * retries full function. */
    const bool safe_mode = diag_in_crash_loop();
    if (safe_mode) {
        ESP_LOGE(TAG,
                 "SAFE MODE: %" PRIu32 " consecutive crashes — booting "
                 "control-plane only (camera + audio disabled)",
                 diag_consecutive_crashes());
    }

    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip=%s rev=%d cores=%d", CONFIG_IDF_TARGET, chip.revision, chip.cores);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "flash=%" PRIu32 " MB", flash_size / (1024 * 1024));
    }

    /* NVS — required by WiFi/MQTT/OTA. Two-stage soft init: if version
     * mismatch, erase and retry; if it still fails, log loudly and
     * carry on (app_config falls back to defaults, WiFi keeps its
     * config in RAM, OTA caches in memory). A field unit shouldn't
     * brick because a single flash sector went bad. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, doing it now");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "NVS init permanently failed: %s — "
                 "continuing without persistent config",
                 esp_err_to_name(err));
    }

    /* Load runtime config from NVS first thing after NVS is up — every
     * subsystem init below (status LED, camera, audio, …) reads schema
     * defaults through app_config_get_*. Calling those before
     * app_config_init returns leaves the cache at BSS-zero, which for
     * BOOL knobs means `false` regardless of the schema default. Caused
     * a ~10 ms window where status_led_dbg read false (default true)
     * and the boot-progress blink pattern silently no-op'd. Reorder
     * makes the cache authoritative for every subsequent init call. */
    app_config_init();
    /* Boot guard: log any untranslated string cell (app_config_init already
     * seeded the live language from the ui_lang knob). */
    i18n_init();

    /* Power management (EXPERIMENTAL light-sleep): configure esp_pm inert
     * (240/240, no light-sleep) and create the NO_LIGHT_SLEEP lock before any
     * audio/camera path can acquire it. Stays inert until the pm_lightsleep
     * knob enables it via apply_power_state(). No-op when CONFIG_PM_ENABLE
     * is unset. */
    cb_pm_init();

    /* Onboard status LED is config-gated and defaults OFF for field.
     * Now that app_config is loaded, status_led_dbg reads as configured
     * and the boot-progress blink fires correctly when enabled. */
    status_led_init();

    /* Battery first — its presence determines whether ModeFsm has SOC input. */
    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG,
                 "battery: not detected — running on USB power, ModeFsm "
                 "will skip Safe transitions");
    }

    /* Every physical I²C sensor (SHT41×N, BMP388×N, INA226×N + the bus1
     * quarantine MAX) — the registry creates and probes each instance and
     * owns its bus binding, so any sensor works on any bus. Optional: boot
     * continues if any are missing; each driver logs its own detection
     * verdict. The on-board SHT41 (bus0) is the one that matters. */
    cb_sensors_init();
    if (!cb_sensor_ready("sht0")) {
        ESP_LOGW(TAG, "sht41: not detected — temp/humidity will be NaN");
    }

    /* Prime the sensor-registry caches once now so the HTML page and OLED
     * show real values immediately, not "—", before the first telemetry
     * tick (which can be up to a full telemetry period away). One read per
     * present sensor; no recurring cost. */
    cb_sensors_refresh();

    /* SD card on Sense expansion (SDIO 1-bit). Optional — captures
     * still publish to MQTT even without SD. */
    if (sd_storage_init() != ESP_OK) {
        ESP_LOGW(TAG, "sd: not mounted — photos will not be persisted");
    }

    /* Camera (OV3660 on Sense expansion). Optional — and skipped entirely
     * in crash-loop safe mode (heavy PSRAM/DMA init, prime crash suspect). */
    if (safe_mode) {
        ESP_LOGW(TAG, "camera: skipped (safe mode)");
    } else if (camera_init() != ESP_OK) {
        ESP_LOGW(TAG, "camera: not detected — photo features disabled");
    }

    /* Photo retry-queue: PSRAM-backed FIFO holding captures whose MQTT
     * publish couldn't be delivered (broker offline, WiFi flap). Drains
     * on next MQTT connect. Failure here is non-fatal — captures just
     * lose the safety net during outages. */
    extern esp_err_t photo_queue_init(void);
    if (photo_queue_init() != ESP_OK) {
        ESP_LOGW(TAG, "photo_queue init failed — captures during outages will be lost");
    }

    /* PIR motion sensor (AM312 on PIR_PIN). Optional. */
    if (pir_init() != ESP_OK) {
        ESP_LOGW(TAG, "pir: init failed — motion trigger disabled");
    }

    /* Reed switch (REED_PIN, D0). Optional — gated by reed_enabled NVS
     * bool; the module is a no-op when the gate is off. */
    if (reed_init() != ESP_OK) {
        ESP_LOGW(TAG, "reed: init failed — door/lid sensor disabled");
    }

    /* Grove ultrasonic ranger + soil moisture. Both optional — gated by
     * their *_enabled NVS bools + a pad mapped to "sonar" / "soil";
     * no-ops on unmodified boards. */
    if (sonar_init() != ESP_OK) {
        ESP_LOGW(TAG, "sonar: init failed — distance sensor disabled");
    }
    if (soil_init() != ESP_OK) {
        ESP_LOGW(TAG, "soil: init failed — moisture sensor disabled");
    }

    /* (INA226 + every other I²C sensor was created above by cb_sensors_init.) */

    /* UART servo bus (UART2 routed via GPIO matrix to pin-map slots).
     * No-op when no uart_tx/uart_rx assignment exists. Defaults to
     * "module idle" so unmodified boards see no change. */
    uart_servo_init();  /* return value handled internally; logs its verdict */

    /* Two audio-output backends, each a no-op unless a pad is mapped to its
     * pin function ("buzzer" / "pcm") — so unmodified boards (incl. the field
     * unit) stay silent, and either or both can be wired. */
    speaker_init();   /* LEDC square-wave chiptune */
    pcm_init();       /* I2S PDM sigma-delta "1-bit DAC" (RC filter + amp) */

    /* Bring the display up HERE — before the multi-second WiFi/MQTT/TLS-enroll
     * block below — so the boot screen appears early and roughly together with
     * the boot jingle, not long after everything else. oled_init() only spawns
     * the soft-detecting display task and returns immediately, so it never
     * delays or breaks a field boot (no panel ⇒ the task just no-ops + exits).
     * The WiFi-onboarding QR handoff stays late (it needs the AP creds). */
    oled_init();

    /* The power-up boot jingle is fired from the OLED splash render (oled.c),
     * NOT here, so the sound lands together with the boot screen rather than
     * ~4 s ahead of it (a display can't reliably come up until the camera/bus
     * settle ~10 s in). audiofx_boot() self-latches. Trade-off: a board with a
     * buzzer but NO display gets no boot jingle — none such exists in the fleet
     * (bench has a display; the field has no audio). */

    /* Brick-safe WiFi credential ladder (see wifi_store.h). If a new
     * credential set is pending verification, count this boot's attempt.
     * A candidate that never reaches IP+MQTT is auto-reverted by the
     * verify block in the main loop below; this RTC counter is the
     * cross-boot backstop for the nastier case where the candidate
     * associates but then panics/reboots before the verify window — after
     * WIFI_CAND_MAX_TRIES boots we drop it here, BEFORE wifi_mgr_init
     * reads the effective creds, so this very boot uses known-good. */
    wifi_store_init();
    /* Endpoint store BEFORE mqtt_init(): also enforces the cross-boot
     * candidate try-counter (auto-revert after NET_CAND_MAX_TRIES boots). */
    net_store_init();
    /* Web-admin (HTTP basic-auth) creds → RAM cache. MUST run here, on the
     * app_main (internal) stack and before the HTTP server starts: the gate
     * resolves creds on every request from PSRAM-stacked workers, and reading
     * NVS there (a flash op) would trip the cache-disable stack assert. */
    auth_store_init();
    /* BLE device allowlist (MAC→name) used by the /ble web UI + the connect/
     * ingest gate in ble.c. Idempotent, cheap; safe on a fresh device. */
    ble_store_init();
    /* Full AP-only mode (operator-selected, sticky): the box runs as an
     * access point only — no station, so no MQTT/OTA/remote recovery. Only
     * the local AP + web server come up. Cleared via the web /config toggle
     * or a BOOT-button factory reset. */
    const bool ap_only = wifi_store_is_ap_only();
    /* Unprovisioned: no sticky AP-only flag, AND no usable STA target (no
     * candidate, no known-good NVS set, and the compile-time WIFI_SSID is the
     * secrets.h.example placeholder / blank — a clean-clone build never given
     * real creds). Come up directly as the AP provisioning portal instead of
     * spending WIFI_SOFTAP_TRIGGER_S failing to associate to a phantom network.
     * ap_mode = "behave like AP-only this boot" (NOT persisted — the moment the
     * operator submits creds via /wifi, the candidate ladder takes over and the
     * next boot is STA). */
    const bool unprov  = !ap_only && !wifi_store_have_sta_target();
    const bool ap_mode = ap_only || unprov;
    if (unprov) {
        ESP_LOGW(TAG,
                 "UNPROVISIONED — no STA target (blank/placeholder WiFi creds); "
                 "booting AP provisioning portal at http://" AP_IP "/wifi");
    }
    if (!ap_mode && wifi_store_has_candidate()) {
        uint32_t tries = diag_wifi_try_inc();
        ESP_LOGW(TAG, "WiFi candidate pending — boot attempt %" PRIu32 "/%d",
                 tries, CONFIG_CHYTRA_BUDKA_WIFI_CAND_MAX_TRIES);
        if (tries > (uint32_t)CONFIG_CHYTRA_BUDKA_WIFI_CAND_MAX_TRIES) {
            ESP_LOGE(TAG, "WiFi candidate exhausted attempts — reverting to known-good");
            wifi_store_revert_candidate();
            diag_wifi_try_clear();
        }
    }

    /* WiFi onboarding (bench OLED). When we're coming up as an access point
     * (unprovisioned first boot, or the sticky AP-only mode) AND a display is
     * wired up, probe it SYNCHRONOUSLY now — before wifi_mgr_init configures
     * the AP — so an unprovisioned board can swap the well-known public
     * default AP password for a fresh per-boot random one (shown only as the
     * on-screen QR). The probe is bounded (~1 s if absent) and gated on AP
     * mode, so a normal STA boot pays nothing. The QR itself is painted later
     * by the OLED task once the panel is up (oled_show_wifi_qr below). */
    bool onboard_display = false;
    if (ap_mode) {
        onboard_display = oled_probe_present();
        if (onboard_display && unprov) {
            wifi_mgr_use_random_ap_pass();
            ESP_LOGW(TAG, "onboarding: display + unprovisioned — random AP "
                          "password in use (read it from the on-screen QR)");
        }
    }

    /* WiFi init returns an error rather than asserting on failure. If
     * that happens we'll never reach our broker, so escalate to a
     * delayed reboot — but only after we've already checked the boot
     * fail counter (set by diag_capture_boot() above). After enough
     * consecutive crashes, sleep longer between attempts to give the
     * environmental cause (brownout? hot CPU?) time to clear. */
    if (wifi_mgr_init(ap_mode) != ESP_OK) {
        uint32_t fails = diag_consecutive_boot_count();
        int delay_s = (fails >= 10) ? 600 : /* >=10 crashes: 10 min sleep */
                          (fails >= 5) ? 120
                                       : /*  5-9 crashes: 2 min sleep  */
                          30;            /*  <5 crashes: 30 s sleep    */
        ESP_LOGE(TAG, "WiFi init failed, consecutive_crashes=%" PRIu32 " — rebooting in %d s",
                 fails, delay_s);
        /* Tag this restart as a pre-boot-succeeded failure so the next
         * boot's diag_capture_boot() counts it toward the safe-mode
         * threshold. Without this RTC flag, the upcoming esp_restart()
         * surfaces as ESP_RST_SW (not classified as crash) and the
         * consecutive-crashes counter wouldn't move — a field unit
         * could loop here for hours/days with no GlitchTip event and
         * no escalating delay beyond the static 30/120/600 s ladder. */
        diag_pre_boot_fail_set();
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
        esp_restart();
    }
    /* AP mode (sticky AP-only OR unprovisioned first-boot portal) has no
     * station — skip every STA/MQTT-dependent service (MQTT, SNTP, GlitchTip,
     * OTA, TLS enroll). The local HTTP server + BOOT-button monitor further
     * below still run, so the box stays manageable over the AP. */
    if (!ap_mode) {
    if (!wifi_mgr_wait_connected(WIFI_STA_WAIT_MS)) {
        ESP_LOGW(TAG,
                 "WiFi did not connect in 20 s — continuing anyway "
                 "(wifi_mgr handles retries with exponential backoff)");
    }

    mqtt_init();
    mqtt_publish_profile(cb::profile_name(cb::Profile::Boot));

    /* SNTP — needed for SD photo filename timestamps + MQTT event
     * timestamps. Non-blocking; downstream code (jpeg_stamp.c,
     * mqtt_publish_triggered) gates epoch fields on year ≥ 2023.
     * The sync_cb logs the first sync moment so a wedged SNTP is
     * visible in the boot log instead of "time was wrong for hours". */
    {
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        sntp_cfg.sync_cb = [](struct timeval *tv) {
            ESP_LOGI("sntp", "synced — epoch=%lld.%03ld (wall clock now usable)",
                     (long long)tv->tv_sec, (long)(tv->tv_usec / 1000));
        };
        esp_netif_sntp_init(&sntp_cfg);
    }
    /* Localtime: Europe/Prague. POSIX TZ string with DST switch rules
     * (last Sun of March 02:00 → CEST = UTC+2; last Sun of October
     * 03:00 → CET = UTC+1). Applies to localtime_r() everywhere —
     * camera filenames stamp in local time, log timestamps stay as
     * IDF's monotonic ms (those are runtime-relative anyway). */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    /* Init GlitchTip but DON'T install the log hook yet — bisecting
     * the boot crash. crash-boot events are safe (just a one-shot
     * enqueue), the log hook is what intercepts vprintf and might
     * be deadlocking or recursing. */
    if (glitchtip_init()) {
        glitchtip_report_crash_boot(diag_reset_reason_name(), diag_consecutive_crashes(),
                                    diag_coredump_size());
        /* Surface safe-mode entry explicitly — the crash-boot event above
         * reports the reset, but this names the consequence (subsystems
         * disabled) so the operator immediately knows the unit is degraded
         * and recoverable rather than dead. Direct report: the ESP_LOG
         * hook isn't installed until the 180 s mark-valid. */
        if (safe_mode && glitchtip_ready()) {
            char m[140];
            snprintf(m, sizeof(m),
                     "safe-mode: %" PRIu32 " consecutive crashes — camera + "
                     "audio disabled, control-plane only",
                     diag_consecutive_crashes());
            glitchtip_report("error", m, NULL);
        }
    }

    ota_init();

    /* Boot-time TLS enrollment gate. If NVS has a cert whose SAN
     * fingerprint still matches our id+domain+IP, this is a no-op
     * (instant return) and http_server_start below picks up the cert
     * and goes HTTPS. If not, the helper waits up to 15 s for MQTT +
     * SNTP and runs the full keygen → CSR → cbd-enroll round-trip →
     * persist pipeline so the first http_server_start of this boot
     * already serves HTTPS — zero-touch field deployment. Failure is
     * non-fatal: we log and fall back to plain HTTP, operator can
     * retry via POST /debug/tls_enroll or the next boot re-tries. */
    tls_boot_enroll_if_needed(/*timeout_ms*/ 30000);

    /* mqtt_init() ran a moment ago and cached "no clientAuth cert", because at
     * that point there wasn't one. If the enrollment above just produced one,
     * the running client will go on offering no certificate and the mTLS
     * broker will go on refusing it — forever, since nothing re-reads that
     * verdict. Reboot into a clean boot that picks the cert up. Only possible
     * on the boot where enrollment first succeeded, so it cannot loop.
     *
     * Mark a pending OTA valid first, exactly as the deferred path does: the
     * enrollment we just completed was an HTTPS round-trip to the same origin
     * that serves OTA, so the corrective channel is proven and this reboot
     * must not roll the image back. */
    if (mqtt_client_identity_is_stale()) {
        diag_mark_ota_valid();
        ESP_LOGW(TAG, "enrolled after MQTT started — rebooting so the client "
                      "presents its new certificate");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    }  /* end if(!ap_mode): STA/MQTT-dependent services */

    /* HTTP(S) server. Branches on tls_store_has_cert(): HTTPS on :443
     * + :80 redirect when enrolled, plain :80 fallback otherwise. Runs in
     * AP mode too (served on the AP netif, AP_IP). */
    http_server_start();

    /* In AP mode (sticky AP-only or the unprovisioned first-boot portal) also
     * start the captive-portal DNS responder so a joining phone auto-opens the
     * /wifi page (the DHCP opt-114 captive URI is already advertised by
     * ap_configure_netif). The STA recovery path starts this itself when the
     * SoftAP fallback fires; here we cover the boot-time AP. */
    if (ap_mode) {
        http_softap_portal_start();
        /* Captive-portal DNS responder: resolve every lookup to the AP IP so a
         * joining phone's OS connectivity check hits our captive 404 handler
         * and the /wifi portal auto-opens (no typed URL). Paired with the
         * DHCP option-6 DNS advertisement in wifi_mgr ap_configure_netif. */
        dns_hijack_start(AP_IP);
    }

    /* (oled_init() now runs early, up by the audio init, so the boot screen
     * comes up alongside the boot jingle instead of after the network block.) */

    /* WiFi onboarding QR: in AP mode with a display present, paint a WiFi-join
     * QR (the AP's effective SSID + password — random for an unprovisioned
     * board, or the configured/default creds in AP-only mode). A phone that
     * scans it joins the AP and the captive portal opens /wifi. The OLED task
     * holds this persistently (until the next boot) once the panel is up; we
     * only need to hand it the credentials here. */
    if (onboard_display) {
        char qs[WIFI_STORE_SSID_CAP], qp[WIFI_STORE_PASS_CAP];
        if (wifi_mgr_get_ap_creds(qs, sizeof(qs), qp, sizeof(qp)))
            oled_show_wifi_qr(qs, qp);
    }

#if CONFIG_CHYTRA_BUDKA_BOOT_BUTTON_RESET
    /* BOOT-button (GPIO0) factory-reset monitor — started in all modes
     * (incl. safe mode) so physical recovery always works. Low priority,
     * small stack: it just polls a GPIO every 200 ms. */
    xTaskCreate(boot_button_task, "bootbtn", 3072, NULL, 1, NULL);
#endif

    /* Subscribe the main task to TWDT. The default idle-only TWDT
     * doesn't catch a task hang because the idle task keeps yielding
     * as long as anything calls vTaskDelay anywhere in the system —
     * which means our audio pump can deadlock and TWDT stays happy.
     * Adding the main task means: if this while-loop ever stops
     * calling esp_task_wdt_reset() for CONFIG_ESP_TASK_WDT_TIMEOUT_S
     * seconds (currently 30 s), TWDT panics + coredump + reboot.
     *
     * Loud-reboot is strictly better than a silent stuck device that
     * still answers ping but does no work — the symptom we hit on
     * .104 yesterday. */
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "esp_task_wdt_add failed — main loop not WDT-protected");
    } else {
        ESP_LOGI(TAG, "main loop subscribed to TWDT (timeout %d s)", CONFIG_ESP_TASK_WDT_TIMEOUT_S);
    }

    /* Drop supervisor priority to 1 so WiFi (23), LWIP (18), and esp-mqtt
     * cleanly preempt the 1 Hz tick on CPU0, and so the audio task on
     * CPU1 (prio 10) is unambiguously higher-priority than anything in
     * this loop. Done AFTER esp_task_wdt_add — TWDT subscription doesn't
     * depend on priority but ordering keeps the intent obvious. */
    vTaskPrioritySet(NULL, 1);

    /* Spawn the dedicated hot-path tasks. audio_task pumps PDM frames
     * on CPU1 prio 10; camera_worker drains capture requests on CPU1
     * prio 5. See core_assignment.h for the full task → core map.
     * Skipped in crash-loop safe mode (control-plane-only boot). */
    if (!safe_mode) {
        audio_task_start();
        camera_worker_start();
        /* SD retention/migration on its own low-prio, NON-WDT task (see
         * sd_maintenance_task) — isolated from cam_wrk + the supervisor loop so
         * a slow/dying card can't stall captures or telemetry. */
        xTaskCreate(sd_maintenance_task, "sd_maint", 4096, NULL, /*prio*/ 1, NULL);
        /* Optional BLE meter scan — no-op unless built with
         * CONFIG_CHYTRA_BUDKA_BLE *and* ble_enabled=true. WiFi has RF priority
         * (set inside ble_start). Skipped in safe mode (control-plane only). */
        ble_start();
    } else {
        ESP_LOGW(TAG, "audio + camera_worker: skipped (safe mode)");
    }

    /* Initial mode evaluation. */
    s_next_mode_check_us = esp_timer_get_time();
    s_next_telemetry_us = esp_timer_get_time();

    while (true) {
#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
        /* Debug hatch: GET /debug/hang?ms=N sets s_hang_main_until_us.
         * Busy-spin BEFORE the wdt reset so the watchdog actually fires
         * and we get to exercise the panic + coredump + reboot pipeline
         * end-to-end. */
        int64_t hang_until = s_hang_main_until_us.load();
        if (hang_until > esp_timer_get_time()) {
            ESP_LOGW(TAG,
                     "DEBUG: hanging main loop until TWDT fires "
                     "(%lld ms remaining)",
                     (hang_until - esp_timer_get_time()) / 1000);
            while (esp_timer_get_time() < hang_until) {
                /* no yield, no esp_task_wdt_reset — that's the whole point */
            }
        }
#endif

        /* Feed the task watchdog. Any deadlock or stuck I²C / I²S / HTTP
         * call below this line will block the reset for >timeout_s and
         * force a clean reboot rather than a silent hang. */
        esp_task_wdt_reset();

        int64_t now = esp_timer_get_time();

        /* Clear the consecutive-crash counter on a pure UPTIME milestone (180 s),
         * BEFORE the ap_mode early-continue below. A board that stays up this long
         * without crashing has broken any crash loop, regardless of provisioning or
         * broker reachability. This MUST run ahead of the ap_mode `continue` —
         * otherwise an unprovisioned / AP-only board that tripped crash-loop safe
         * mode never reaches the clear and can't self-heal without a physical power
         * cycle (RTC_NOINIT survives soft/RTS resets). Network-independent by design;
         * the OTA mark-valid timer below stays MQTT-gated and separate. */
        static int64_t s_crash_clear_due_us = 0;
        static bool s_crash_cleared = false;
        if (!s_crash_cleared) {
            if (s_crash_clear_due_us == 0) {
                s_crash_clear_due_us = now + MARK_VALID_DELAY_US;
            } else if (now >= s_crash_clear_due_us) {
                diag_clear_crash_count();
                s_crash_cleared = true;
            }
        }

        /* (SD retention + flat-root migration run on the dedicated sd_maint
         * task — NOT here. Keeping that off the WDT-fed supervisor loop means a
         * slow/dying card's multi-second unlinks can't stall telemetry/mode-eval
         * or trip a reboot loop.) */

        /* AP mode (sticky AP-only or unprovisioned portal): no station, so skip
         * the telemetry / net-watchdog / candidate / SoftAP-recovery / OTA /
         * enroll logic below. BUT still drive the power ladder — profile_tick() moves
         * Boot→Triggered on a no-battery bench (or honours mode_override), then
         * apply_power_state() calls audio_begin() so the mic actually pumps.
         * Without this the FSM stays in Boot forever in AP mode and audio_task
         * sits idle → the mic looks dead (frames_captured=0, selftest "no data")
         * even though the HW is fine. The camera worker + HTTP server run in
         * their own tasks, so /capture, /stream.mjpg, /mic.wav and the live
         * preview all work over the AP for positioning the box during onboarding. */
        if (ap_mode) {
            profile_tick();
            apply_power_state();
            /* WiFi-onboarding (bench): periodically echo the AP creds to the
             * LOCAL serial console (~every 30 s). In AP mode MQTT + GlitchTip
             * are both down, so this NEVER leaves the box — it's an operator
             * fallback for when the on-screen QR won't scan, and the
             * serial-free HIL reads it here to join the SoftAP (the per-boot
             * random AP password is otherwise unknowable to an automated tool).
             * Only fires when a display drove the random-password/QR path. */
            if (onboard_display) {
                static int64_t s_ap_creds_log_us = 0;
                if (now >= s_ap_creds_log_us) {
                    s_ap_creds_log_us = now + 30LL * 1000000;
                    char aps[WIFI_STORE_SSID_CAP], app[WIFI_STORE_PASS_CAP];
                    if (wifi_mgr_get_ap_creds(aps, sizeof(aps), app, sizeof(app)))
                        ESP_LOGW(TAG, "onboarding AP creds (local console only): "
                                      "ssid=%s pass=%s", aps, app);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Run self-test once, ~5 s after first MQTT connect, so that
         * HA discovery has been picked up and the diag entity exists. */
        static bool s_first_selftest_done = false;
        static int64_t s_selftest_due_us = 0;
        if (mqtt_is_connected()) {
            if (s_selftest_due_us == 0) {
                s_selftest_due_us = now + 5LL * 1000 * 1000;
            } else if (now >= s_selftest_due_us) {
                selftest_run_and_publish(nullptr, 0);
                if (!s_first_selftest_done) {
                    /* Publish boot diag (reset reason + coredump status)
                     * once, as soon as MQTT is up and HA discovery has had
                     * a chance to land. Retained for late subscribers. */
                    diag_publish_boot();
                    s_first_selftest_done = true;
                }
                /* Re-run periodically (every 5 min) so a sensor that dies
                 * AFTER boot — bad B2B contact, a browned-out mic, a wedged
                 * I²C slave — shows up as degraded -> GlitchTip without
                 * needing a reboot. The selftest escalation is edge-
                 * triggered, so a steady degraded state reports just once. */
                s_selftest_due_us = now + 300LL * 1000 * 1000;
            }
        }

        /* Network watchdog — recover from a wedged network stack. esp-mqtt
         * auto-reconnects, but when outbound TCP connects all fail (socket
         * exhaustion under load — seen on the bench during heavy OTA + upload
         * traffic: MQTT *and* GlitchTip both stuck on transport-connect, only
         * a reboot cleared it) the reconnect loops forever with no escape. If
         * MQTT stays down past the threshold, self-reboot. Timer resets
         * whenever MQTT is up, so it only fires on a CONTINUOUS outage;
         * diag_pre_boot_fail_set() folds repeated reboots into the safe-mode +
         * ramped-delay path so a real broker outage ramps down, not tight-loops. */
#if CONFIG_CHYTRA_BUDKA_NET_WATCHDOG_S > 0
        static int64_t s_mqtt_down_since_us = 0;
        /* Only count when the network stack is WEDGED — WiFi STA up but
         * MQTT down (the socket-exhaustion case this watchdog was built
         * for). When the STA itself is down, hold the timer: rebooting
         * can't conjure a missing AP, and the SoftAP ladder below owns
         * wifi-down recovery. Without this gate the watchdog would reboot
         * at the same threshold the SoftAP trigger uses and the fallback
         * would never come up. */
        if (mqtt_is_connected() || !wifi_mgr_is_connected()) {
            s_mqtt_down_since_us = 0;
            /* A real MQTT session proves the stack is NOT wedged — clear the
             * escalation so the next genuine wedge reboots fast again. */
            if (mqtt_is_connected() && diag_netwdt_count() > 0)
                diag_netwdt_reset();
        } else {
            /* Escalating threshold: base << min(count,4). The first wedged-stack
             * reboot still fires at the base interval, but if MQTT is STILL down
             * after the reboot (i.e. it's a broker outage a reboot can't fix),
             * each successive interval doubles (cap 16×). That turns a multi-hour
             * broker outage from ~one reboot per base-interval (a battery-burning
             * storm) into a handful, while still recovering a truly wedged stack
             * promptly. */
            uint32_t esc = diag_netwdt_count();
            if (esc > 4) esc = 4;
            int64_t threshold_us =
                ((int64_t)CONFIG_CHYTRA_BUDKA_NET_WATCHDOG_S << esc) * 1000000LL;
            if (s_mqtt_down_since_us == 0) {
                s_mqtt_down_since_us = now;
            } else if (now - s_mqtt_down_since_us >= threshold_us) {
                ESP_LOGE(TAG,
                         "net watchdog: MQTT down %lld s (threshold %dx) — "
                         "rebooting to recover (reboot #%" PRIu32 ")",
                         (long long)((now - s_mqtt_down_since_us) / 1000000LL),
                         1 << esc, diag_netwdt_count() + 1);
                diag_netwdt_inc();
                diag_pre_boot_fail_set();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }
#endif

        /* ── WiFi credential verify-before-commit (anti-brick core) ──
         * A staged candidate (from cmd/wifi or the SoftAP form) is on
         * trial this boot. Promote it to known-good ONLY once the control
         * plane is proven — IP + MQTT connected and held stable for
         * WIFI_CAND_VERIFY_S — the same "can still receive a corrective
         * OTA" invariant the OTA mark-valid uses. If it never gets there
         * within WIFI_CAND_VERIFY_TIMEOUT_S, auto-revert to known-good and
         * reboot, so a wrong SSID/password can't strand the OTA-only board.
         * Decoupled from the 180 s OTA window so the operator isn't made to
         * wait. NOT counted as a crash (no diag_pre_boot_fail_set) — the
         * RTC try-counter handles the candidate ladder separately. */
        {
            static bool    s_cand_resolved   = false;
            static int64_t s_cand_first_us   = 0;  /* first loop pass with candidate */
            static int64_t s_cand_mqtt_ok_us = 0;  /* when MQTT came up + stayed */
            if (!s_cand_resolved && wifi_store_has_candidate()) {
                if (s_cand_first_us == 0) s_cand_first_us = now;
                /* Initial onboarding (no known-good to fall back to): reverting
                 * a candidate would drop the box into the unprovisioned AP
                 * portal — worse than committing creds that demonstrably
                 * associate. So as soon as WiFi has an IP, promote on
                 * association alone, WITHOUT waiting for MQTT (TLS likely needs
                 * re-enrollment after the factory reset that left no known-good,
                 * so MQTT can lag minutes / never under a broken enroll path).
                 * When a known-good DOES exist (e.g. a field box changing WiFi),
                 * fall through to the strict MQTT-verify below so a wrong or
                 * control-plane-unreachable network still reverts safely. */
                if (!wifi_store_has_known_good() && wifi_mgr_is_connected()) {
                    if (wifi_store_promote_candidate() == ESP_OK) {
                        diag_wifi_try_clear();
                        ESP_LOGW(TAG, "WiFi candidate committed on association "
                                      "(initial onboarding, no known-good "
                                      "fallback) — TLS/MQTT will follow");
                    }
                    s_cand_resolved = true;
                } else if (mqtt_is_connected()) {
                    if (s_cand_mqtt_ok_us == 0) s_cand_mqtt_ok_us = now;
                    if (now - s_cand_mqtt_ok_us >=
                        (int64_t)CONFIG_CHYTRA_BUDKA_WIFI_CAND_VERIFY_S * 1000000LL) {
                        if (wifi_store_promote_candidate() == ESP_OK) {
                            diag_wifi_try_clear();
                            mqtt_publish_wifi_status("promoted");
                            ESP_LOGI(TAG, "WiFi candidate verified + promoted to known-good");
                        }
                        s_cand_resolved = true;
                    }
                } else {
                    s_cand_mqtt_ok_us = 0;  /* MQTT dropped — restart stability timer */
                    if (now - s_cand_first_us >=
                        (int64_t)CONFIG_CHYTRA_BUDKA_WIFI_CAND_VERIFY_TIMEOUT_S * 1000000LL) {
                        ESP_LOGE(TAG,
                                 "WiFi candidate failed to reach MQTT in %d s — "
                                 "reverting to known-good + rebooting",
                                 CONFIG_CHYTRA_BUDKA_WIFI_CAND_VERIFY_TIMEOUT_S);
                        wifi_store_revert_candidate();
                        diag_wifi_try_clear();
                        vTaskDelay(pdMS_TO_TICKS(200));
                        esp_restart();
                    }
                }
            }
        }

        /* ── Endpoint candidate verify-before-commit (net_store) ──
         * Mirrors the WiFi candidate ladder above, for cmd/endpoint broker
         * changes: promote the staged record to known-good only after MQTT
         * has held for NET_CAND_VERIFY_S on the NEW broker; revert + reboot
         * back to known-good/default if it never connects within
         * NET_CAND_VERIFY_TIMEOUT_S. Anti-brick guarantee for re-pointing
         * an OTA-only field board at a new broker. */
        {
            static bool    s_net_resolved   = false;
            static int64_t s_net_first_us   = 0;
            static int64_t s_net_mqtt_ok_us = 0;
            if (!s_net_resolved && net_store_has_candidate()) {
                if (s_net_first_us == 0) s_net_first_us = now;
                if (mqtt_is_connected()) {
                    if (s_net_mqtt_ok_us == 0) s_net_mqtt_ok_us = now;
                    if (now - s_net_mqtt_ok_us >=
                        (int64_t)CONFIG_CHYTRA_BUDKA_NET_CAND_VERIFY_S * 1000000LL) {
                        if (net_store_promote_candidate() == ESP_OK) {
                            mqtt_publish_net_state();
                            ESP_LOGI(TAG, "endpoint candidate verified + "
                                          "promoted to known-good");
                        }
                        s_net_resolved = true;
                    }
                } else {
                    s_net_mqtt_ok_us = 0;  /* MQTT dropped — restart stability timer */
                    if (now - s_net_first_us >=
                        (int64_t)CONFIG_CHYTRA_BUDKA_NET_CAND_VERIFY_TIMEOUT_S * 1000000LL) {
                        ESP_LOGE(TAG,
                                 "endpoint candidate failed to reach MQTT in %d s — "
                                 "reverting + rebooting",
                                 CONFIG_CHYTRA_BUDKA_NET_CAND_VERIFY_TIMEOUT_S);
                        net_store_revert_candidate();
                        vTaskDelay(pdMS_TO_TICKS(200));
                        esp_restart();
                    }
                }
            }
        }

        /* ── Deferred TLS enrollment / renewal retry ──
         * The boot enroll (tls_boot_enroll_if_needed) gives MQTT+SNTP only a
         * 15 s window. On a weak-signal link MQTT can take a minute+ to punch
         * through (DHCP alone observed at ~48 s @ -94 dBm), so the boot enroll
         * defers — and with no retry the box would stay on plain HTTP forever,
         * never reaching HTTPS. Fires when MQTT+SNTP are up AND either there's
         * no cert (first enrollment) OR the cert is within its renewal window
         * (proactive renew before expiry — without this the cert expires and
         * HTTPS is lost permanently). The async task reboots on success so the
         * clean boot's quick-path serves the new cert. Bounded TWO ways: a
         * cooldown (attempts can't stack — the enroll round-trip is < the
         * cooldown) AND a per-boot attempt cap so a persistently-failing
         * signer can't respawn a 12 KB enroll task forever (heap churn →
         * OOM-panic → false crash-loop). After the cap we stay on the current
         * transport (plain HTTP, or the existing still-valid cert) and let the
         * next boot try again. */
        {
            static int64_t s_enroll_retry_us = 0;
            static int      s_enroll_attempts = 0;
            constexpr int   ENROLL_MAX_ATTEMPTS = 6;  /* ~12 min @ 120 s cooldown */
            const bool need_enroll = !tls_store_has_cert() ||
                                     tls_enroll_cert_due_for_renewal() ||
                                     tls_enroll_san_drifted() ||
                                     tls_enroll_needs_client_auth() ||
                                     tls_store_has_pending_key();
            /* Legacy MQTT enrollment needs a broker session; the HTTPS
             * transport only needs an IP — critically, that keeps the
             * retry alive while an mtls broker candidate is still failing
             * (enroll fixes the cert, the candidate ladder then carries
             * the flip). */
            char enroll_ip[20];
            const bool enroll_link =
                mqtt_is_connected() ||
                (tls_enroll_https_configured() &&
                 wifi_mgr_get_ip_str(enroll_ip, sizeof(enroll_ip)));
            if (need_enroll && enroll_link && time(NULL) > CB_CLOCK_SYNCED_EPOCH &&
                s_enroll_attempts < ENROLL_MAX_ATTEMPTS) {
                if (s_enroll_retry_us == 0 || now >= s_enroll_retry_us) {
                    s_enroll_attempts++;
                    ESP_LOGI(TAG,
                             "MQTT+SNTP up, cert %s — running deferred enroll "
                             "(attempt %d/%d)",
                             tls_store_has_cert() ? "needs renewal/reissue" : "absent",
                             s_enroll_attempts, ENROLL_MAX_ATTEMPTS);
                    if (tls_enroll_retry_async(/*timeout_ms*/ 30000) != ESP_OK) {
                        /* Spawn failed (OOM) — don't burn the cooldown; retry
                         * next tick and don't count it as an attempt. */
                        s_enroll_attempts--;
                    } else {
                        /* Cooldown > the 30 s enroll timeout so attempts can't
                         * stack; on success the task reboots before this fires. */
                        s_enroll_retry_us = now + 120LL * 1000 * 1000;
                    }
                }
            } else if (need_enroll && s_enroll_attempts >= ENROLL_MAX_ATTEMPTS) {
                static bool s_enroll_gaveup_logged = false;
                if (!s_enroll_gaveup_logged) {
                    s_enroll_gaveup_logged = true;
                    ESP_LOGW(TAG,
                             "deferred enroll: %d attempts failed — giving up "
                             "this boot (staying on current transport; next "
                             "boot retries)",
                             ENROLL_MAX_ATTEMPTS);
                }
            }
        }

        /* ── WiFi SoftAP recovery fallback ──
         * If the station can't connect for WIFI_SOFTAP_TRIGGER_S straight
         * (and no candidate is on trial — that uses the faster revert
         * path), bring up a WPA2 SoftAP + the /wifi config portal so an
         * operator can re-provision a sealed field box with no physical
         * access. Time-bounded: after WIFI_SOFTAP_MAX_S the board reboots
         * to retry the station. If the home AP returns while the fallback
         * is up, tear it down and resume normal operation. */
#if CONFIG_CHYTRA_BUDKA_WIFI_SOFTAP_TRIGGER_S > 0
        {
            static int64_t s_sta_down_since_us = 0;
            static int64_t s_softap_up_since_us = 0;
            if (wifi_mgr_softap_active()) {
                if (wifi_mgr_is_connected()) {
                    ESP_LOGI(TAG, "STA reconnected — stopping SoftAP fallback");
                    http_softap_portal_stop();
                    wifi_mgr_stop_softap();
                    s_softap_up_since_us = 0;
                    s_sta_down_since_us = 0;
                } else {
                    if (s_softap_up_since_us == 0) s_softap_up_since_us = now;
                    else if (now - s_softap_up_since_us >=
                             (int64_t)CONFIG_CHYTRA_BUDKA_WIFI_SOFTAP_MAX_S * 1000000LL) {
                        ESP_LOGW(TAG,
                                 "SoftAP window (%d s) expired — rebooting to retry STA",
                                 CONFIG_CHYTRA_BUDKA_WIFI_SOFTAP_MAX_S);
                        vTaskDelay(pdMS_TO_TICKS(200));
                        esp_restart();
                    }
                }
            } else if (wifi_mgr_is_connected() || wifi_store_has_candidate()) {
                s_sta_down_since_us = 0;
            } else {
                if (s_sta_down_since_us == 0) {
                    s_sta_down_since_us = now;
                } else if (now - s_sta_down_since_us >=
                           (int64_t)CONFIG_CHYTRA_BUDKA_WIFI_SOFTAP_TRIGGER_S * 1000000LL) {
                    ESP_LOGW(TAG, "STA down %d s — launching SoftAP recovery portal",
                             CONFIG_CHYTRA_BUDKA_WIFI_SOFTAP_TRIGGER_S);
                    if (wifi_mgr_start_softap() == ESP_OK) {
                        http_softap_portal_start();
                        s_softap_up_since_us = now;
                    } else {
                        s_sta_down_since_us = 0; /* retry the trigger later */
                    }
                }
            }
        }
#endif

        /* After 3 minutes of clean runtime, confirm a pending OTA image so
         * the bootloader stops being ready to roll back.
         *
         * Gated on hot-path task health: if audio_task or cam_worker
         * failed xTaskCreatePinnedToCore at boot (heap fragmentation,
         * stack alloc OOM), don't mark the image valid — the next
         * reset will roll back to the previous slot. A field unit
         * with no audio + no captures is functionally dead; better
         * to roll back than to pin a broken image forever. */
        static int64_t s_succeed_due_us = 0;
        static bool s_succeed_done = false;
        if (!s_succeed_done) {
            if (s_succeed_due_us == 0) {
                s_succeed_due_us = now + MARK_VALID_DELAY_US;
            } else if (now >= s_succeed_due_us) {
                /* Commit the image once the CONTROL PLANE is confirmed:
                 * MQTT connectivity proves we can still RECEIVE A
                 * CORRECTIVE OTA, which is the one invariant that must
                 * hold for an unreflashable field board. We deliberately
                 * do NOT gate on camera/audio health here (it used to):
                 * a dead sensor — HW fault, bad B2B contact, or a
                 * transient xTaskCreate OOM at boot — must not roll back
                 * an otherwise-good, recoverable image, which would
                 * discard a fix for a hardware problem and is the wrong
                 * trade for an OTA-only unit. Degraded sensors are
                 * surfaced via the selftest -> GlitchTip path instead.
                 * This also lets a crash-loop safe-mode boot (camera/audio
                 * intentionally not started) commit + stay recoverable.
                 * If MQTT isn't up yet, DEFER rather than refuse forever;
                 * a permanently-unreachable image is caught by the net
                 * watchdog above, which reboots and lets the bootloader
                 * roll the pending image back. */
                if (!mqtt_is_connected()) {
                    s_succeed_due_us = now + MARK_VALID_RETRY_US;
                } else {
                    diag_mark_ota_valid();
                    /* Install the ESP_LOG -> GlitchTip hook now that the
                     * boot has been stable for 180 s and the control
                     * plane is confirmed. Delaying past boot_succeeded
                     * matches the original "bisecting the boot crash"
                     * concern (init-time vprintf hook was suspected to
                     * deadlock) while keeping runtime ESP_LOGE shipped in
                     * steady state. The hook itself is now race-safe —
                     * s_send_in_flight guards reentrancy from
                     * mbedtls/lwIP tasks during TLS handshakes. */
                    glitchtip_install_log_hook();
                    /* Clear stale OTA status from the previous boot so
                     * `state/ota` doesn't stay pinned at the last cycle's
                     * terminal value ("done"/"error") forever. */
                    extern void mqtt_pub_retained(const char *topic,
                                                  const char *value);
                    char topic[128];
                    snprintf(topic, sizeof(topic), "%s/state/ota", mqtt_topic_base());
                    mqtt_pub_retained(topic, "");
                    s_succeed_done = true;
                }
            }
        }

        /* ── 1 Hz: re-evaluate mode + handle MQTT commands ── */
        if (now >= s_next_mode_check_us) {
            s_next_mode_check_us = now + 1000 * 1000;
            profile_tick();
            /* Re-apply audio-window + WiFi-PS every tick (window can flip with
             * no mode change). Cheap: cache reads + one localtime_r. */
            apply_power_state();
            /* PIR motion: iterate every armed instance. Each has its
             * own hold-down timer + active flag so two PIRs on the
             * same board don't share state. Instance 0 keeps the
             * "pir" trigger string for HA automations that already
             * key on it; instances 1+ get "pir_<n>". */
            bool pir_gate = app_config_get_bool("pir_enabled");
            for (int pi = 0; pi < pir_active_count(); pi++) {
                if (pir_motion_consume_nth(pi) && pir_gate) {
                    uint32_t count = pir_motion_count_nth(pi);
                    ESP_LOGI(TAG, "motion[%d] detected (count=%" PRIu32 ")",
                             pi, count);
                    /* Hibernate: extend the PIR active window so a motion burst
                     * keeps the unit awake (no-op outside hibernate). */
                    ds_note_activity();
                    if (pi == 0) status_led_pir_pulse();
                    if (!s_motion_active[pi]) {
                        s_motion_active[pi] = true;
                        mqtt_publish_motion_nth(pi, true);
                        oled_flash(OLED_FLASH_MOTION);   /* bench OLED: 2 short blinks (rising edge) */
                    }
                    s_motion_off_us[pi] = now + MOTION_HOLD_MS * 1000;
                    mqtt_publish_motion_count_nth(pi, count);
                    /* ds_pir_photo_allowed(): always true outside hibernate;
                     * inside hibernate, honours the ds_pir_photo knob. */
                    if (camera_ready() && app_config_get_bool("cam_enabled") &&
                        ds_pir_photo_allowed()) {
                        char trig[32];
                        if (pi == 0) {
                            snprintf(trig, sizeof(trig), "pir");
                        } else {
                            snprintf(trig, sizeof(trig), "pir_%d", pi);
                        }
                        camera_request_event(trig);
                    }
                }
                if (s_motion_active[pi] && now >= s_motion_off_us[pi]) {
                    s_motion_active[pi] = false;
                    mqtt_publish_motion_nth(pi, false);
                }
            }

            /* Sonar proximity trigger (sonar_trig_cm > 0): the poll task
             * latched a far→near edge; same capture path + gating as PIR.
             * event/photo carries trigger:"sonar" for HA automations. */
            if (sonar_trigger_consume()) {
                ESP_LOGI(TAG, "sonar proximity trigger (count=%" PRIu32 ")",
                         sonar_trigger_count());
                if (camera_ready() && app_config_get_bool("cam_enabled"))
                    camera_request_event("sonar");
            }

            /* Bench OLED VAD-burst blink — poll the burst counter (~100 ms loop,
             * so it tracks the trigger LED closely): a VAD burst = three quick.
             * Photo flashes fire from camera_capture_event() itself (in sync
             * with the jingle, real captures only). PIR is handled above (two
             * short). The burst counter mirrors the raw VAD detector and keeps
             * ticking on ambient sound even when vad_enabled is OFF (it stays
             * live for RMS telemetry) — so gate the blink on vad_enabled, but
             * still advance last_burst so re-enabling doesn't catch-up-blink.
             * Primed on the first pass so a pre-existing count doesn't blink on
             * boot. No-op when no panel is present. */
            {
                static bool oled_ev_primed = false;
                static uint32_t last_burst;
                uint32_t burst = audio_burst_count();
                if (!oled_ev_primed) { last_burst = burst; oled_ev_primed = true; }
                if (burst != last_burst) {
                    last_burst = burst;
                    if (app_config_get_bool("vad_enabled")) oled_flash(OLED_FLASH_VAD);
                }
            }

            /* Reed switches: iterate every armed instance. Instance 0
             * keeps the singleton-era trigger string ("reed_open") so
             * any HA automation hooked on it survives the multi-
             * instance migration; instances 1+ get "reed_<n>_open".
             *
             * Open transition (closed→open, magnet pulled away) fires
             * a camera capture: operator opening the nestbox for
             * inspection / cleaning gets a frame stamped at the
             * moment the lid moves. Close transitions don't capture
             * — the inside is dark right after the lid drops (no IR),
             * and PIR / VAD inside the box already answers "is anyone
             * in there". Gated on cam_enabled like every other auto-
             * trigger. */
            for (int ri = 0; ri < reed_active_count(); ri++) {
                if (!reed_event_consume_nth(ri)) continue;
                ds_note_activity();   /* hibernate: a reed edge extends the wake window */
                bool closed = reed_is_closed_nth(ri);
                uint32_t count = reed_event_count_nth(ri);
                ESP_LOGI(TAG, "reed[%d]: %s (count=%" PRIu32 ")",
                         ri, closed ? "CLOSED" : "OPEN", count);
                mqtt_publish_reed_nth(ri, closed);
                mqtt_publish_reed_count_nth(ri, count);
                if (!closed && camera_ready() && app_config_get_bool("cam_enabled")) {
                    /* trig buffer sized for "reed_<idx>_open" — gcc's
                     * format-truncation analysis is pessimistic about
                     * %d width, so 32 chars leaves comfortable headroom
                     * past the realistic upper bound (idx<10). */
                    char trig[32];
                    if (ri == 0) {
                        snprintf(trig, sizeof(trig), "reed_open");
                    } else {
                        snprintf(trig, sizeof(trig), "reed_%d_open", ri);
                    }
                    camera_request_event(trig);
                }
            }
            if (mqtt_photo_requested() && camera_ready() && app_config_get_bool("cam_enabled")) {
                camera_request_event("mqtt");
            }
            /* Timelapse: tlapse_min minutes between shots. 0 = off.
             * Cadence resets whenever the operator changes the knob so a
             * "60 → 5" switch doesn't make them wait up to an hour for the
             * new schedule to kick in. First shot lands one interval after
             * boot (or after the knob is enabled).
             *
             * camera_request_event() is async (enqueues on the worker
             * queue) so we advance the schedule unconditionally — if the
             * queue is full the request is logged in camera.c and the
             * slot is silently lost. For a 1-frame-per-hour cadence the
             * worst case is one missed shot per overflow, which is far
             * better than the old behavior of permanently shifting the
             * schedule by one full retry tick. */
            {
                static int32_t s_last_interval_min = -1;
                static int64_t s_next_timelapse_us = 0;
                int32_t interval_min = app_config_get_int("tlapse_min");
                if (interval_min != s_last_interval_min) {
                    s_last_interval_min = interval_min;
                    if (interval_min > 0) {
                        s_next_timelapse_us =
                            now + (int64_t)interval_min * 60LL * 1000000LL;
                        ESP_LOGI(TAG, "timelapse: next shot in %" PRId32 " min",
                                 interval_min);
                    } else {
                        s_next_timelapse_us = 0;
                        ESP_LOGI(TAG, "timelapse: disabled");
                    }
                }
                if (interval_min > 0 && now >= s_next_timelapse_us &&
                    camera_ready() && app_config_get_bool("cam_enabled")) {
                    camera_request_event("timelapse");
                    s_next_timelapse_us =
                        now + (int64_t)interval_min * 60LL * 1000000LL;
                }
            }
            if (audio_vad_capture_consume() && camera_ready() &&
                app_config_get_bool("cam_enabled")) {
                camera_request_event("vad");
            }
            if (mqtt_snapshot_requested()) {
                publish_full_telemetry();
                ds_note_telemetry_published();
                telemetry_period_for_profile();
                continue;  // skip period-based publish below
            }
        }

        /* ── periodic telemetry (profile-aware period) ── */
        if (now >= s_next_telemetry_us) {
            telemetry_period_for_profile();
            publish_full_telemetry();
            ds_note_telemetry_published();
        }

        /* Hibernate heartbeat: the periodic block above uses the ds_sleep_s
         * cadence (≫ the wake window), so drive exactly one real (connected)
         * telemetry publish per wake here. cb_ds's re-sleep gate waits for it. */
        if (ds_heartbeat_pending() && mqtt_is_connected()) {
            publish_full_telemetry();
            ds_note_telemetry_published();
        }

        /* ── Hibernate: re-sleep decision (no-op unless profile==hibernate).
         * Runs at the loop tail, after telemetry/PIR/OTA. When the wake window
         * is satisfied it arms the wake sources and deep-sleeps (never returns).
         * Not reached in ap_mode (early-continue above). ── */
        ds_maybe_sleep();

        /* Supervisor cadence: 10 Hz. Audio runs on its own task pinned
         * to CPU1; camera captures are enqueued onto the worker queue
         * and drained asynchronously. The only thing this loop does is
         * the 1 Hz mode/PIR/reed/timelapse tick + telemetry — 100 ms
         * sleep is comfortable below that and keeps app_main from
         * busy-spinning CPU0 between ticks. In deep-save (Safe + light sleep)
         * stretch to 500 ms so CPU0 idles long enough to light-sleep between
         * the ~400 ms WiFi DTIM wakes; the 1 Hz tick just runs with ≤500 ms
         * jitter, harmless for a survival posture. */
        vTaskDelay(pdMS_TO_TICKS(app_profile_sleeps() ? 500 : 100));
    }
}
