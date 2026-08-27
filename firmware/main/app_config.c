/* app_config.c — schema-driven, NVS-backed runtime config.
 *
 * Each schema entry has a stable string key (≤ 14 chars to fit in NVS),
 * a type, default, optional min/max for numbers, a friendly HA name and
 * icon. Cached values live in a parallel runtime array indexed by entry.
 *
 * NVS namespace: "cb_cfg".
 *
 * Topics (per-device, rooted at device_id()):
 *   <device_id>/state/cfg/<key>   retained, current value as text
 *   <device_id>/cmd/cfg/<key>     subscribed; payload = new value text
 *   <device_id>/cmd/reboot        subscribed; payload ignored
 *
 * Discovery (per entry):
 *   homeassistant/<comp>/<device_id>/cfg_<key>/config
 *   <comp> = number | switch ; reboot is published as a one-shot button. */
#include "app_config.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "device_id.h"
#include "i18n.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mqtt.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "text_util.h"

static const char *TAG = "cfg";

#define NS  "cb_cfg"

typedef enum { T_FLOAT, T_INT, T_BOOL } cfg_type_t;

/* ---- Pin function map ----------------------------------------------------
 *
 * Each user-accessible XIAO ESP32-S3 breakout (D0..D7 = GPIO 1, 2, 3, 4, 5,
 * 6, 43, 44) has an NVS knob `pin_dN_fn` that names what the pin does.
 * Modules look themselves up via `app_config_pin_for(<fn>)` at init time —
 * NO live re-arming. Any pin remap takes effect at the next reboot.
 *
 * Adding a new function: extend the PIN_FN_* enum + PIN_FN_LABELS table
 * + (optionally) singleton/pair constraints below. Discovery + dashboard
 * pick the new label up automatically; the consuming module is on its own
 * to call app_config_pin_for() at init.
 *
 * Singleton functions (max 1× across the pin map): ir_led, capture_led,
 * uart_tx, uart_rx, button, oled_pwr, buzzer. Pair functions (must appear with their partner or not
 * at all): i2c0_sda↔i2c0_scl, i2c1_sda↔i2c1_scl, uart_tx↔uart_rx. Setter
 * cross-validates the resulting map and refuses writes that would break
 * either rule. */
typedef enum {
    PIN_FN_NONE = 0,
    PIN_FN_REED,
    PIN_FN_PIR,
    PIN_FN_IR_LED,
    PIN_FN_CAPTURE_LED,
    PIN_FN_I2C0_SDA,
    PIN_FN_I2C0_SCL,
    PIN_FN_I2C1_SDA,
    PIN_FN_I2C1_SCL,
    PIN_FN_UART_TX,
    PIN_FN_UART_RX,
    PIN_FN_BUTTON,        /* external push-button → cycles the OLED page (and the
                          * same hold-to-factory-reset as the onboard BOOT button);
                          * appended at the end so existing NVS pin values keep
                          * their meaning. */
    PIN_FN_OLED_PWR,      /* GPIO → MOSFET gating the OLED's VCC; oled_enabled=OFF
                          * cuts it (true power-down). Appended at the end so
                          * existing NVS pin values keep their meaning. */
    PIN_FN_BUZZER,        /* GPIO → LEDC square-wave beeper (active piezo /
                          * passive transducer / line-in). speaker.c looks
                          * itself up here at init. Appended at the end so
                          * existing NVS pin values keep their meaning. */
    PIN_FN_PCM,           /* GPIO → I2S PDM TX (1-bit sigma-delta) for real
                          * sample playback (pcm.c). Feed via an RC low-pass +
                          * amp = DIY 1-bit DAC. Appended at the end. */
    PIN_FN_SONAR,         /* Grove Ultrasonic Ranger SIG — single-wire
                          * trigger+echo, any pad works (sonar.c; gated by
                          * sonar_enabled). Appended at the end. */
    PIN_FN_SOIL,          /* Grove Soil Moisture SIG — analog, needs an ADC1
                          * pad (D0..D5 = GPIO1..6; the setter refuses the
                          * rest). soil.c; gated by soil_enabled. Appended
                          * at the end. */
    PIN_FN__COUNT,
} pin_fn_t;

static const char *const PIN_FN_LABELS[PIN_FN__COUNT] = {
    [PIN_FN_NONE]        = "none",
    [PIN_FN_REED]        = "reed",
    [PIN_FN_PIR]         = "pir",
    [PIN_FN_IR_LED]      = "ir_led",
    [PIN_FN_CAPTURE_LED] = "capture_led",
    [PIN_FN_I2C0_SDA]    = "i2c0_sda",
    [PIN_FN_I2C0_SCL]    = "i2c0_scl",
    [PIN_FN_I2C1_SDA]    = "i2c1_sda",
    [PIN_FN_I2C1_SCL]    = "i2c1_scl",
    [PIN_FN_UART_TX]     = "uart_tx",
    [PIN_FN_UART_RX]     = "uart_rx",
    [PIN_FN_BUTTON]      = "button",
    [PIN_FN_OLED_PWR]    = "oled_pwr",
    [PIN_FN_BUZZER]      = "buzzer",
    [PIN_FN_PCM]         = "pcm",
    [PIN_FN_SONAR]       = "sonar",
    [PIN_FN_SOIL]        = "soil",
};

/* Pin-slot key names map to actual GPIO numbers. PIN_SLOT_KEYS index = slot
 * D-number; PIN_SLOT_GPIO is the matching ESP32-S3 GPIO so a module that
 * gets back "pin slot 2 → use ir_led" can resolve it to GPIO3. */
static const char *const PIN_SLOT_KEYS[8] = {
    "pin_d0_fn", "pin_d1_fn", "pin_d2_fn", "pin_d3_fn",
    "pin_d4_fn", "pin_d5_fn", "pin_d6_fn", "pin_d7_fn",
};
static const int PIN_SLOT_GPIO[8] = {
    1, 2, 3, 4, 5, 6, 43, 44,
};
#define PIN_SLOT_COUNT 8

/* Singleton functions — at most one pin slot may carry this function.
 * The I²C SDA/SCL lines are singletons too: a bus has exactly one of each,
 * so a second i2c0_sda (etc.) is always a misconfig (and would silently
 * orphan the essential bus0 gauge/sensors). Paired with pin_fn_pair_of so
 * the setter enforces "exactly one sda + one scl, together". */
static bool pin_fn_is_singleton(pin_fn_t fn) {
    switch (fn) {
        case PIN_FN_IR_LED:
        case PIN_FN_CAPTURE_LED:
        case PIN_FN_I2C0_SDA:
        case PIN_FN_I2C0_SCL:
        case PIN_FN_I2C1_SDA:
        case PIN_FN_I2C1_SCL:
        case PIN_FN_UART_TX:
        case PIN_FN_UART_RX:
        case PIN_FN_BUTTON:
        case PIN_FN_OLED_PWR:
        case PIN_FN_BUZZER:
        case PIN_FN_PCM:
        case PIN_FN_SONAR:
        case PIN_FN_SOIL:
            return true;
        default:
            return false;
    }
}

/* Pair functions — if one side is mapped, the other must also be (and
 * vice versa) for the function to be usable. Returns the paired function,
 * or PIN_FN__COUNT if `fn` is not part of a pair. */
static pin_fn_t pin_fn_pair_of(pin_fn_t fn) {
    switch (fn) {
        case PIN_FN_I2C0_SDA: return PIN_FN_I2C0_SCL;
        case PIN_FN_I2C0_SCL: return PIN_FN_I2C0_SDA;
        case PIN_FN_I2C1_SDA: return PIN_FN_I2C1_SCL;
        case PIN_FN_I2C1_SCL: return PIN_FN_I2C1_SDA;
        case PIN_FN_UART_TX:  return PIN_FN_UART_RX;
        case PIN_FN_UART_RX:  return PIN_FN_UART_TX;
        default:              return PIN_FN__COUNT;
    }
}

static int pin_fn_from_label(const char *label) {
    for (int i = 0; i < PIN_FN__COUNT; i++) {
        if (strcmp(label, PIN_FN_LABELS[i]) == 0) return i;
    }
    return -1;
}

static int pin_slot_index(const char *key) {
    for (int i = 0; i < PIN_SLOT_COUNT; i++) {
        if (strcmp(key, PIN_SLOT_KEYS[i]) == 0) return i;
    }
    return -1;
}

typedef struct {
    const char *key;       /* short, lowercase, snake_case */
    cfg_type_t  type;
    float       min, max, step;  /* numbers only */
    const char *unit;            /* HA unit_of_meas, optional */
    const char *name;            /* HA friendly name */
    const char *icon;            /* mdi: */
    union {
        float    f;
        int32_t  i;
        bool     b;
    } def;
} cfg_entry_t;

/* ---- SCHEMA ----------------------------------------------------------- */
/* Add new keys here. Keep `key` ≤ 15 ASCII chars for NVS — the hard
 * limit is `NVS_KEY_NAME_MAX_SIZE - 1 = 15` (IDF nvs.h). Anything longer
 * makes every `nvs_set_*` return `ESP_ERR_NVS_KEY_TOO_LONG`, the setter
 * leaves the cache untouched, the retained state echo is not published,
 * and the HA toggle silently snaps back. There's a runtime guard at the
 * bottom of `app_config_init` that logs `ESP_LOGE` for any over-limit
 * entry — caught on the first dev boot before it ships. */
static const cfg_entry_t SCHEMA[] = {
    /* Master VAD enable. When OFF, audio_pump_triggered keeps reading
     * frames (for /mic.wav + RMS telemetry) but never fires the burst
     * branch — no MQTT triggered event, no photo, no audio relay
     * stream. Threshold-based "set vad_thr_dbfs really high" hack is
     * inferior because schema clamps to -20 dBFS and loud noises still
     * leak through. This flag is the clean off-switch. */
    { "vad_enabled",  T_BOOL, 0, 0, 0, NULL,
      "VAD enabled", "mdi:waveform",
      .def = { .b = true } },
    { "vad_thr_dbfs", T_FLOAT, -80, -20, 1, "dBFS",
      "VAD threshold", "mdi:waveform",
      .def = { .f = -45.0f } },
    { "vad_burst_ms", T_INT, 1000, 120000, 1000, "ms",
      "VAD burst duration", "mdi:timer-sand",
      .def = { .i = 30000 } },
    { "vad_rearm_ms", T_INT, 0, 60000, 500, "ms",
      "VAD rearm time", "mdi:timer-refresh",
      .def = { .i = 5000 } },
    /* Power profile — the single power-management selector. One ladder
     * (most→least power) replaces the old mode_override + pm_lightsleep +
     * a deep-sleep bool. Each tier bundles activity + sleep depth + cadence;
     * `auto` walks the SOC ladder (never self-hibernates unless soc_hib_en>0).
     * Encoded as int enum (matches POWER_PROFILE_LABELS / HA select options):
     *   0 = auto       (SOC ladder picks max…sentinel)
     *   1 = max        (stream + capture, no sleep)        — most power
     *   2 = active     (VAD burst + motion capture, no sleep)
     *   3 = eco        (motion capture, audio off, light-sleep)
     *   4 = sentinel   (sensors only, audio off, light-sleep)
     *   5 = hibernate  (deep-sleep duty cycle, UNREACHABLE) — least power
     * Sleeping tiers (eco/sentinel/hibernate) force audio OFF — the mic's
     * continuous I2S holds cb_pm's NO_LIGHT_SLEEP lock, so sleep can't
     * engage while audio runs. Field default 0 (auto). */
    { "power_profile", T_INT, 0, 5, 1, NULL,
      "Power profile", "mdi:cog-transfer",
      .def = { .i = 0 } },
    /* Per-tier telemetry cadence (read LIVE in telemetry_period_for_profile):
     *   max               → tlm_max_s (60 s, the chatty live tier)
     *   active / eco       → tlm_mid_s (300 s)
     *   sentinel           → tlm_low_s (900 s, survival heartbeat)
     *   hibernate          → ds_sleep_s (one publish per wake)
     * NVS keys ≤9 chars. */
    { "tlm_max_s",    T_INT, 10, 600, 5, "s",
      "Telemetry period (max)", "mdi:timer-outline",
      .def = { .i = 60 } },
    { "tlm_mid_s",    T_INT, 30, 3600, 30, "s",
      "Telemetry period (active/eco)", "mdi:timer-outline",
      .def = { .i = 300 } },
    { "tlm_low_s",    T_INT, 60, 7200, 30, "s",
      "Telemetry period (sentinel)", "mdi:timer-outline",
      .def = { .i = 900 } },
    /* Audio active-hours window (local wall clock). Gates audio_begin/end in
     * the audio-running modes (Continuous/Triggered) so a solar unit listens
     * only during bird-active hours and goes silent (real power saving — the
     * only lever that moves the flat ~1.1 W baseline; see POWER.md) overnight.
     *   on_h == off_h  => window DISABLED → always-on audio (the default, so a
     *                     fielded unit's behaviour is unchanged after OTA).
     *   on_h <  off_h  => same-day window  (e.g. 5..21).
     *   on_h >  off_h  => wraps midnight   (e.g. 22..6, nocturnal).
     * Read LIVE each 1 Hz tick in main.cpp apply_power_state() — no apply hook.
     * Safe mode never runs audio regardless of the window. */
    { "audio_on_h",   T_INT, 0, 23, 1, "h",
      "Audio active from (hour)", "mdi:weather-sunset-up",
      .def = { .i = 0 } },
    { "audio_off_h",  T_INT, 0, 23, 1, "h",
      "Audio active until (hour)", "mdi:weather-sunset-down",
      .def = { .i = 0 } },
    /* Light-sleep is no longer a standalone knob — it's a pure function of the
     * power_profile tier (eco/sentinel engage it; max/active never; hibernate is
     * deep sleep). apply_power_state() calls cb_pm_set_lightsleep(eco||sentinel).
     * This removes the old "only works in Safe" coupling and the crash-loop risk
     * (light-sleep + audio): sleeping tiers force audio OFF by definition. */
    /* WiFi STA listen interval — how many AP beacon intervals the modem may doze
     * before waking to check for buffered downlink. Applied to wcfg.sta.listen_interval
     * in wifi_mgr; only takes EFFECT under WIFI_PS_MAX_MODEM, i.e. Safe mode (other
     * modes use MIN_MODEM/NONE and ignore it). Higher = deeper doze = lower power but
     * higher downlink latency — and this is an OTA-only fleet, so too high delays
     * cmd/ota + cmd/cfg delivery. Default 3 = IDF default = today's behaviour (the
     * wifi log line "li: 3"). Applied at connect, so a change takes effect on the next
     * reconnect/reboot, not live. The lone un-built lever flagged in POWER.md
     * for pushing a solar unit below ~0.40 W; bump per-unit only after measuring the
     * latency trade. NVS key 14 chars. */
    { "wifi_listen_iv", T_INT, 1, 10, 1, NULL,
      "WiFi listen interval", "mdi:wifi-cog",
      .def = { .i = 3 } },
    /* Battery-SOC % enter-thresholds for the `auto` power ladder. Read LIVE in
     * main.cpp profile_tick() (next 1 Hz tick), so an operator can retune battery
     * behaviour in the field without a reflash. Only ENTER thresholds are exposed;
     * profile_tick() derives each leave-threshold by subtracting the boundary's
     * default hysteresis gap (max −10, act −7, eco −6), so the FSM's tested
     * hysteresis is preserved when knobs sit at defaults. Keep en values ordered
     * (max_en > act_en > eco_en > hib_en). */
    { "soc_max_en",   T_INT, 0, 100, 1, "%",
      "SOC enter Max", "mdi:battery-high",
      .def = { .i = 65 } },
    { "soc_act_en",   T_INT, 0, 100, 1, "%",
      "SOC enter Active", "mdi:battery-medium",
      .def = { .i = 45 } },
    { "soc_eco_en",   T_INT, 0, 100, 1, "%",
      "SOC enter Eco", "mdi:battery-low",
      .def = { .i = 28 } },
    /* Survival floor: when >0, `auto` may descend below Sentinel into Hibernate
     * (deep sleep, UNREACHABLE) at this SOC. 0 = auto NEVER self-hibernates —
     * hibernate stays a deliberate manual power_profile choice. */
    { "soc_hib_en",   T_INT, 0, 100, 1, "%",
      "SOC enter Hibernate (0=off)", "mdi:battery-alert",
      .def = { .i = 0 } },
    /* ---- Hibernate (deep-sleep duty cycle) — active only when the resolved
     * profile is `hibernate`. All read LIVE by cb_ds at the loop tail. ---- */
    /* Periodic RTC-timer wake interval (the check-in cadence). Also the
     * effective telemetry cadence in hibernate (one publish per wake). */
    { "ds_sleep_s",   T_INT, 60, 86400, 60, "s",
      "Hibernate wake interval", "mdi:timer-sand",
      .def = { .i = 900 } },
    /* Max awake-window for a TIMER wake before forced re-sleep. Floor 20 s
     * acknowledges the ~8–15 s boot→WiFi→MQTT cost. */
    { "ds_wake_s",    T_INT, 20, 600, 5, "s",
      "Hibernate wake window", "mdi:timer-outline",
      .def = { .i = 45 } },
    /* On a PIR wake, stay awake until this long after the LAST motion edge
     * (burst coverage), then re-sleep. Re-extended by each fresh edge. */
    { "ds_pir_win_s", T_INT, 10, 300, 5, "s",
      "Hibernate PIR active window", "mdi:motion-sensor",
      .def = { .i = 60 } },
    /* Snap a photo on a PIR wake (camera-present boards only). */
    { "ds_pir_photo", T_BOOL, 0, 0, 0, NULL,
      "Hibernate photo on PIR", "mdi:camera",
      .def = { .b = true } },
    /* Check OTA on a wake only if this many HOURS elapsed since the last
     * check (RTC-persisted). 0 = never check OTA while hibernating. */
    { "ds_ota_every", T_INT, 0, 168, 1, "h",
      "Hibernate OTA check every", "mdi:cloud-download",
      .def = { .i = 24 } },
    /* Post-publish MQTT-drain cap before deep sleep — exits early when the
     * client outbox empties; this is the upper bound. */
    { "ds_settle_ms", T_INT, 200, 10000, 100, "ms",
      "Hibernate MQTT settle", "mdi:timer-cog",
      .def = { .i = 1500 } },
    /* OTA battery safety gate: OTA is allowed only when SOC ≥ this. 0 disables
     * the gate. Bypassed on external/USB power (battery_on_external_power()), so
     * the mains/USB field board always OTAs. Guards against a brownout mid-flash
     * on a battery unit. Read by ota.c on both the background poll + DS on-wake
     * check. */
    { "ota_min_soc",  T_INT, 0, 100, 5, "%",
      "OTA min battery SOC", "mdi:battery-charging-50",
      .def = { .i = 50 } },
    { "cam_enabled",  T_BOOL, 0, 0, 0, NULL,
      "Camera enabled", "mdi:camera",
      .def = { .b = true } },
    /* Bench OLED on/off — a manual power switch for the status panel. OFF
     * blanks the SSD1306 (charge-pump + display off) and stops the refresh
     * task flushing (also frees the shared bus0 the required SHT41 uses); if a
     * GPIO is mapped to "oled_pwr" in the pin map it also cuts the panel's VCC
     * (true power-down via a MOSFET). Read LIVE by oled_task — no apply hook.
     * Field board has no panel → pure no-op. Not a meaningful field watt lever
     * (~20 mA, <1 % of the ~1.1 W baseline); a bench convenience. Default ON so
     * fielded units' behaviour is unchanged after OTA. */
    { "oled_enabled", T_BOOL, 0, 0, 0, NULL,
      "OLED display", "mdi:monitor",
      .def = { .b = true } },
    /* Timelapse interval in minutes. 0 = off (default). Main loop ticks
     * once per second, schedules the next shot N minutes after the
     * previous one, and fires camera_capture_event("timelapse") which
     * publishes through the same path as PIR/VAD/cmd_photo (so the HA
     * archiver picks it up automatically). Live — changing the value
     * resets the cadence. Range covers 1 min up to 24 h. NVS key is
     * `tlapse_min` (≤14 chars, NVS limit); HA slug stays "timelapse
     * interval" via the friendly name. */
    { "tlapse_min", T_INT, 0, 1440, 1, "min",
      "Timelapse interval", "mdi:camera-timer",
      .def = { .i = 0 } },
    { "pir_enabled",  T_BOOL, 0, 0, 0, NULL,
      "PIR enabled", "mdi:motion-sensor",
      .def = { .b = true } },
    /* Reed switch on REED_PIN (D0) — optional door/lid contact. Default
     * OFF: at boot the GPIO is left in its post-bootloader state and no
     * polling task is started. Live-tunable via MQTT — flipping ON
     * arms the GPIO + spawns the poll task + publishes initial
     * state/reed; flipping OFF asks the task to exit on its next 20 ms
     * tick. No reboot required. See reed_apply_config in reed.c. */
    { "reed_enabled", T_BOOL, 0, 0, 0, NULL,
      "Reed switch enabled", "mdi:magnet",
      .def = { .b = false } },
    /* Total debounce window. The poll task samples GPIO every 20 ms and
     * counts how many consecutive samples disagree with the current
     * debounced state; transition is accepted only after
     * `reed_db_ms / 20` such samples. Lower = snappier (down to
     * 20 ms ≈ 1 sample); higher = more tolerant of EMI on long lid
     * harnesses (different installs differ). Default 100 ms killed the
     * field unit's spurious 39-count storm. Live — change takes effect
     * on the next sample, no reboot. NVS key short-form (10 chars) —
     * the more readable `reed_debounce_ms` was 16 chars and silently
     * failed every nvs_set on the HA toggle. */
    { "reed_db_ms", T_INT, 20, 2000, 20, "ms",
      "Reed debounce", "mdi:timer-sand",
      .def = { .i = 100 } },
    /* Grove Ultrasonic Ranger on the "sonar" pin function (sonar.c).
     * Default OFF — like reed_enabled, the flag is the operator's "it
     * is physically wired" statement (an unplugged SIG just times out,
     * but there's no point running the poll task). Live-toggle via
     * sonar_apply_config; the poll period is read live each cycle.
     * The knob is the STEADY-STATE (MQTT/HA) cadence only — while the
     * OLED ENV page is on-screen the task self-boosts to ~2 Hz for a
     * live panel readout (oled_env_page_visible), so there's no need
     * to keep this short for debugging. */
    { "sonar_enabled", T_BOOL, 0, 0, 0, NULL,
      "Ultrasonic ranger enabled", "mdi:signal-distance-variant",
      .def = { .b = false } },
    { "sonar_poll_s", T_INT, 1, 3600, 1, "s",
      "Ultrasonic poll period", "mdi:timer-outline",
      .def = { .i = 60 } },
    /* Proximity photo trigger: a valid reading dropping below this
     * fires camera_request_event("sonar") — same capture path/gating
     * as PIR, and event/photo carries trigger:"sonar" for HA. 0 = off.
     * Hysteresis on the far side (+10 %, min 10 cm) so a target parked
     * AT the threshold triggers once, not per sample. While armed the
     * task samples continuously at ~2 Hz (detection latency must not
     * hang off sonar_poll_s, which then only caps MQTT). */
    { "sonar_trig_cm", T_INT, 0, 400, 1, "cm",
      "Ultrasonic photo trigger (0=off)", "mdi:camera-burst",
      .def = { .i = 0 } },
    /* "Nothing in range" split. With no real target these modules emit
     * a fixed-width artifact pulse that decodes as a constant fake
     * distance (~60–80 cm observed on the bench at 3V3) — point the
     * sensor into free space, read the artifact off the ENV page, set
     * this just below it. Valid readings at/above then report as
     * clear: OLED "inf", MQTT the 999 sentinel. 0 = off (raw readings
     * pass through). */
    { "sonar_clear_cm", T_INT, 0, 500, 1, "cm",
      "Ultrasonic clear threshold (0=off)", "mdi:signal-distance-variant",
      .def = { .i = 0 } },
    /* Grove Soil Moisture on the "soil" pin function (soil.c). Analog
     * probe — no presence detection is possible on a floating ADC pin,
     * so the enable IS the wiring statement. Own poll task like the
     * sonar, with the same OLED ENV-page ~2 Hz self-boost — this knob
     * is the steady-state MQTT/HA cadence only. dry/wet are the
     * two-point calibration in raw mV: dry air ≈ 0–100 mV, saturated
     * ≈ 1500–2500 mV at 3V3 (short the probe prongs, read the mV off
     * the ENV page / soil_mv entity, copy into soil_wet_mv — live). */
    { "soil_enabled", T_BOOL, 0, 0, 0, NULL,
      "Soil moisture enabled", "mdi:sprout",
      .def = { .b = false } },
    { "soil_poll_s", T_INT, 1, 3600, 1, "s",
      "Soil poll period", "mdi:timer-outline",
      .def = { .i = 60 } },
    { "soil_dry_mv", T_INT, 0, 3300, 10, "mV",
      "Soil calibration (dry)", "mdi:water-minus",
      .def = { .i = 50 } },
    { "soil_wet_mv", T_INT, 0, 3300, 10, "mV",
      "Soil calibration (wet)", "mdi:water-plus",
      .def = { .i = 1800 } },
    { "cam_rotate_180", T_BOOL, 0, 0, 0, NULL,
      "Camera 180° rotation", "mdi:rotate-360",
      .def = { .b = false } },
    /* Sensor profile knobs. Two independent profiles: "cam_*" for still
     * captures (camera_capture_event) and "mjpg_*" for the live MJPEG
     * stream. MJPEG handler swaps to stream profile on entry and restores
     * capture profile on every exit path. No mid-stream toggling — if
     * VAD/PIR fires during a stream that capture comes out at stream
     * quality, and event/photo JSON publishes the actual (framesize,
     * quality) so dashboards can correlate.
     *
     * framesize maps directly to the esp_camera framesize_t enum AS
     * BUNDLED IN THIS BUILD (managed_components/.../driver/include/sensor.h).
     * NOTE: that enum has extra 128X128 (idx 2) and 320X320 (idx 7) entries
     * vs the "classic" enum, so every index >=10 is +3 from the old tables.
     * The useful 4:3 / 16:9 range here:
     *   10=VGA  640x480  (4:3)
     *   11=SVGA 800x600  (4:3)
     *   12=XGA  1024x768 (4:3)
     *   13=HD   1280x720 (16:9)   <- NOT 4:3
     *   14=SXGA 1280x1024(5:4)
     *   15=UXGA 1600x1200(4:3)    <- largest 4:3 the OV3660 does well
     * (values 0-9 are sub-VGA; verified against the field "captured
     * 1280x720" log, which is exactly idx 13 = HD.)
     * Cap is 15 so the still + stream profiles can both be set to a
     * matching 4:3 (e.g. stills 15 UXGA + stream 12 XGA).
     *
     * quality is libjpeg-style "lower = better, larger". Sensor accepts
     * 1-63 but <4 is unstable on OV3660 and >32 looks worse than just
     * dropping resolution. */
    /* Defaults below come from the 2026-05-26 HIL sweep — OV3660 visual
     * quality is nearly flat across jpeg_quality 8..28. Stills are UXGA
     * (1600×1200, idx 15) for the largest 4:3 the OV3660 does cleanly;
     * at that resolution q=8 frames (~150-300 KB) routinely overflow the
     * 160 KB MQTT image buffer (mqtt.c MQTT_OUT_BUFFER_SIZE) → rejected
     * shots. Since the sweep showed quality is flat in this range, q=12
     * keeps the full UXGA frame comfortably under 160 KB with no visible
     * quality loss vs q=8. For MJPEG we picked 1024×768 (XGA, idx 12,
     * 4:3 — matches the stills aspect) q=28 as the Pareto winner of
     * sqrt(sharp) × fps under a 5 Mbps stream budget. See
     * firmware/tests/hil/test_jpeg_sweep.py and the README note for the
     * full methodology. */
    { "cam_framesize",  T_INT, 0, 15, 1, NULL,
      "Camera framesize (stills)", "mdi:image-size-select-large",
      .def = { .i = 15 } },    /* 1600×1200 (UXGA, 4:3) */
    { "cam_quality",    T_INT, 4, 32, 1, NULL,
      "Camera JPEG quality (stills)", "mdi:quality-high",
      .def = { .i = 12 } },    /* fits UXGA frame under 160 KB MQTT cap; quality flat vs q=8 */
    { "mjpg_framesize", T_INT, 0, 15, 1, NULL,
      "MJPEG framesize (stream)", "mdi:video-image",
      .def = { .i = 12 } },    /* 1024×768 (XGA, 4:3 — matches stills 4:3) */
    { "mjpg_quality",   T_INT, 4, 32, 1, NULL,
      "MJPEG JPEG quality (stream)", "mdi:quality-medium",
      .def = { .i = 28 } },    /* high q number = small frame, good fps */
    /* Operator-tunable OV3660 image levels — applied live via
     * camera_apply_tuning() on cmd/cfg (no reflash). Ranges match the
     * esp32-camera sensor API; defaults = the former hard-coded neutral
     * values, so a board that never touches these behaves exactly as before. */
    { "cam_brightness", T_INT, -2, 2, 1, NULL,
      "Camera brightness", "mdi:brightness-6",
      .def = { .i = 0 } },
    { "cam_contrast",   T_INT, -2, 2, 1, NULL,
      "Camera contrast", "mdi:contrast-box",
      .def = { .i = 0 } },
    { "cam_saturation", T_INT, -2, 2, 1, NULL,
      "Camera saturation", "mdi:palette",
      .def = { .i = 0 } },
    { "cam_sharpness",  T_INT, -2, 2, 1, NULL,
      "Camera sharpness", "mdi:image-filter-center-focus",
      .def = { .i = 0 } },
    { "cam_ae_level",   T_INT, -2, 2, 1, NULL,
      "Camera exposure compensation", "mdi:brightness-percent",
      .def = { .i = 0 } },
    { "cam_wb_mode",    T_INT, 0, 4, 1, NULL,
      "Camera white balance (0 auto / 1 sun / 2 cloud / 3 office / 4 home)",
      "mdi:white-balance-sunny",
      .def = { .i = 0 } },
    { "cam_special_fx", T_INT, 0, 6, 1, NULL,
      "Camera effect (0 none / 2 grayscale / 6 sepia)",
      "mdi:image-filter-black-white",
      .def = { .i = 0 } },
    /* Default 4 = 32x. The OV3660 factory table ships ~15.5x; camera_apply_tuning
     * now writes the REAL ceiling (the esp32-camera helper was broken — see
     * camera.c), so this knob finally bites. 32x gives the AGC headroom to climb
     * through dusk so ir_agc_thresh (8x) is reachable and the gain reads as a
     * usable light signal; IR fires before it gets noisy, so field image quality
     * is unaffected. Lower it (e.g. 3 = 16x) if dusk frames look too grainy. */
    { "cam_gainceil",   T_INT, 0, 6, 1, NULL,
      "Camera gain ceiling (0=2x .. 6=128x; higher = brighter+noisier at dusk)",
      "mdi:camera-iris",
      .def = { .i = 4 } },
    /* IR illuminator master enable + ambient-light gating.
     *
     * Behavior in camera_capture_event:
     *   if (!ir_led_enabled)                      → never light up
     *   else if (sensor AGC gain >= ir_agc_thresh) → light up (it's dark)
     *   else                                       → skip IR (daylight)
     *
     * The "dark" decision uses the sensor's own AGC value: the OV3660 reports
     * the real analog gain MULTIPLIER (1 = full saturation / bright sunlight,
     * climbing to the cam_gainceil ceiling — default 32x — in the dark). 8 is an
     * initial guess for "indoor dusk" — tune on real night/day data.
     * ir_agc_thresh=0 = force IR every shot (always-on debug). ir_agc_thresh=99 =
     * effectively force-off (gain never gets that high). NB: this only works now
     * that cam_gainceil is written correctly — the old broken ceiling pinned gain
     * at 1, so the threshold was unreachable and IR never auto-fired.
     *
     * Disabling ir_led_enabled is the explicit "never light" knob for
     * battery-saving / quiet operation; preferable to setting thresh=99
     * because the log shows the operator's intent clearly. */
    { "ir_led_enabled", T_BOOL, 0, 0, 0, NULL,
      "IR LED enabled", "mdi:led-on",
      .def = { .b = true } },
    { "ir_agc_thresh", T_INT, 0, 99, 1, NULL,
      "IR AGC threshold (dark)", "mdi:weather-night",
      .def = { .i = 8 } },
    /* NVS key short-form (10 chars); the descriptive `capture_led_enabled`
     * was 19 chars (NVS limit is 15) and silently failed nvs_set on every
     * HA toggle — the entity flipped back to ON regardless of the click. */
    { "cap_led_en", T_BOOL, 0, 0, 0, NULL,
      "Capture LED enabled", "mdi:led-on",
      .def = { .b = true } },
    /* Short tick on the "buzzer" pin function at each capture. Off by default
     * (opt-in; silent unless a pad is also mapped to "buzzer"). */
    { "cap_beep_en", T_BOOL, 0, 0, 0, NULL,
      "Capture beep enabled", "mdi:volume-high",
      .def = { .b = false } },
    /* OTA pulls from a single shared endpoint (ota.example.com). Bench boards
     * routinely run dirty builds newer than what's on the server, so the
     * OTA poller will gladly downgrade them to whatever stale tag is
     * staged for the field — observed: test board crash-looped on a
     * months-old release after a single OTA cycle. So the DEFAULT is
     * build-scoped: OFF on bench/debug images (CONFIG_..._DEBUG_ENDPOINTS,
     * the bench↔field discriminator) so a factory reset — e.g. a BOOT-button
     * hold, or a button-pin-fn mapped to a grounded pin — can't silently
     * re-enable the auto-downgrade; ON for field images so they still take
     * production OTAs. An operator can always flip it per-board in NVS/HA. */
    { "ota_enabled",  T_BOOL, 0, 0, 0, NULL,
      "OTA updates", "mdi:cloud-download",
#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
      .def = { .b = false } },
#else
      .def = { .b = true } },
#endif
    /* Codec switch: when ON, audio.cpp routes captured PCM through
     * cb::FlacEncoder and POSTs as audio/flac instead of audio/L16.
     * Default OFF — opt-in until libFLAC is vendored on FW (see
     * cb_core/README-flac.md) and field-validated. */
    { "flac_enabled", T_BOOL, 0, 0, 0, NULL,
      "FLAC encoding", "mdi:file-music",
      .def = { .b = false } },
    /* Optional BLE meter reader (NimBLE) — only takes effect if the firmware
     * was built with CONFIG_CHYTRA_BUDKA_BLE (bench overlay). Default OFF: WiFi
     * is the box's job, BLE is opt-in. Toggling ON starts a low-duty passive
     * scan live; see firmware/BLE.md. */
    { "ble_enabled",  T_BOOL, 0, 0, 0, NULL,
      "BLE meter scan", "mdi:bluetooth",
      .def = { .b = false } },
    /* Onboard status LED is off by default for field/battery units: avoid
     * wasting power and blinking into the nest. Enable on the bench. With
     * debug patterns disabled, only capture/OTA force the LED visibly on. */
    { "status_led_en", T_BOOL, 0, 0, 0, NULL,
      "Status LED enabled", "mdi:led-outline",
      .def = { .b = false } },
    { "status_led_dbg", T_BOOL, 0, 0, 0, NULL,
      "Status LED debug patterns", "mdi:led-on",
      .def = { .b = true } },
    /* SD-card retention. The capture path writes into /sdcard/YYYY-MM-DD/…;
     * with autoprune ON (default) a near-full card sheds its OLDEST whole
     * day-buckets (legacy loose root files first) after each capture until
     * free space climbs back over sd_min_free + 5 %. Default ON because the
     * alternative — a silently full card where every capture's SD write fails
     * — loses photos with no operator signal (CONTRIBUTING.md: robustness
     * over breadth).
     * sd_keep_days adds an age cap (0 = off). Read live by camera.c; no
     * reboot. NVS keys: sd_autoprune(12) / sd_min_free(11) / sd_keep_days(12),
     * all ≤ 15. */
    { "sd_autoprune", T_BOOL, 0, 0, 0, NULL,
      "SD autoprune", "mdi:broom",
      .def = { .b = true } },
    { "sd_min_free", T_INT, 1, 50, 1, "%",
      "SD min free", "mdi:harddisk",
      .def = { .i = 10 } },
    { "sd_keep_days", T_INT, 0, 3650, 1, "d",
      "SD keep days", "mdi:calendar-clock",
      .def = { .i = 0 } },
    /* UART servo bus — for the 16-channel UART servo controller the
     * user wants to experiment with. Baud rate covers everything
     * from cheap 9600-baud RC servo boards to Feetech/Dynamixel
     * 1 Mbit STS-series. Default 1000000 matches Feetech-style
     * smart servos which is the user's likely first target. The
     * module is a no-op when no slot in the pin map is set to
     * uart_tx or uart_rx (default rev3.2 layout has neither). */
    { "uart_baud", T_INT, 9600, 1000000, 100, NULL,
      "UART baud", "mdi:serial-port",
      .def = { .i = 1000000 } },
    /* Pin function map — see pin_fn_t / PIN_FN_LABELS for the full
     * function inventory. Stored as enum int in NVS but emitted as HA
     * select with named options (see app_config_publish_discovery
     * pin_*_fn branch). Any change requires a reboot to take effect —
     * modules read their pin via app_config_pin_for() at init, no poll.
     *
     * ── Allocation principle ─────────────────────────────────────────
     * RTC-capability is the scarce resource: only the EXT1 deep-sleep
     * wake sources (pir/reed/button) need an RTC GPIO (ESP32-S3: 0..21).
     * The D-header splits cleanly: D0..D5 = GPIO1..6 (RTC-capable),
     * D6/D7 = GPIO43/44 (NOT RTC). The OPTIMAL layout anchors the
     * ESSENTIAL-but-never-waking primary I²C bus0 (MAX17048 + SHT41 +
     * BMP388 + the universal board's BQ25798 charger) on the throwaway
     * non-RTC pads D6/D7, freeing all six RTC pins for wake sources. The
     * setter's RTC guard enforces the wake-fn-on-RTC half (see "Pin
     * function map cross-validation").
     *
     * ── DEFAULT = optimal D6/D7 (Phase 2) ────────────────────────────
     * The compile default ships the optimal layout: bus0 on D6/D7, D4 =
     * button, D5 = spare, all wake sources on RTC pads. A freshly flashed /
     * NVS-erased board (incl. the bench after a HIL factory reset) comes up
     * correctly on its D6/D7 wiring — which is what makes the HIL selftest
     * (requires sht41) pass. A fielded unit that has its pin map in NVS
     * keeps whatever it holds (e.g. legacy D4/D5); a unit that relied on the
     * OLD default relocates bus0 to D6/D7 on OTA and loses its gauge/sensors
     * until re-pinned — recoverable in one config push (set d4=i2c0_sda,
     * d5=i2c0_scl, d6/d7=i2c1_*), accepted for the single field unit. */
    { "pin_d0_fn", T_INT, 0, PIN_FN__COUNT - 1, 1, NULL,
      "Pin D0 function (GPIO1, RTC)", "mdi:gpio",
      .def = { .i = PIN_FN_REED } },
    { "pin_d1_fn", T_INT, 0, PIN_FN__COUNT - 1, 1, NULL,
      "Pin D1 function (GPIO2, RTC)", "mdi:gpio",
      .def = { .i = PIN_FN_PIR } },
    { "pin_d2_fn", T_INT, 0, PIN_FN__COUNT - 1, 1, NULL,
      "Pin D2 function (GPIO3, RTC)", "mdi:gpio",
      .def = { .i = PIN_FN_IR_LED } },
    { "pin_d3_fn", T_INT, 0, PIN_FN__COUNT - 1, 1, NULL,
      "Pin D3 function (GPIO4, RTC)", "mdi:gpio",
      .def = { .i = PIN_FN_CAPTURE_LED } },
    /* D4/D5 (RTC) freed by moving bus0 to D6/D7: D4 = button (the 3rd wake
     * source), D5 = buzzer (LEDC square-wave → passive speaker on the bench).
     * (Legacy/field boards keep i2c0 here via NVS.) */
    { "pin_d4_fn", T_INT, 0, PIN_FN__COUNT - 1, 1, NULL,
      "Pin D4 function (GPIO5, RTC)", "mdi:gpio",
      .def = { .i = PIN_FN_BUTTON } },
    { "pin_d5_fn", T_INT, 0, PIN_FN__COUNT - 1, 1, NULL,
      "Pin D5 function (GPIO6, RTC)", "mdi:gpio",
      .def = { .i = PIN_FN_BUZZER } },
    /* D6/D7 (GPIO43/44, non-RTC) = primary I²C bus0 — the right home for a
     * bus that never wakes. GPIO43/44 also carry the ROM UART0 boot log, but
     * the console is on USB-CDC so I²C here is fine (the bus tolerates the
     * brief boot toggle). bus1 (clone-quarantine / 2nd SHT41) is no longer in
     * the default; assign it to spare slots if needed. */
    { "pin_d6_fn", T_INT, 0, PIN_FN__COUNT - 1, 1, NULL,
      "Pin D6 function (GPIO43)", "mdi:gpio",
      .def = { .i = PIN_FN_I2C0_SDA } },
    { "pin_d7_fn", T_INT, 0, PIN_FN__COUNT - 1, 1, NULL,
      "Pin D7 function (GPIO44)", "mdi:gpio",
      .def = { .i = PIN_FN_I2C0_SCL } },
    /* Web UI / HA friendly-name language. T_INT enum over i18n_lang_t
     * (0=cs, 1=en). Rendered as a named <select> in /config; on change,
     * apply_side_effects flips the live language + re-publishes HA discovery. */
    { "ui_lang", T_INT, 0, LANG_COUNT - 1, 1, NULL,
      "UI language", "mdi:translate",
      .def = { .i = LANG_EN } },
    /* Buzzer output mode (speaker.c, "buzzer" pin function). Read live at
     * playback. OFF = active/self-drive piezo → pad held full-on, fixed pitch
     * (melodies = rhythm); ON = tone → LEDC frequency follows the note, so a
     * passive piezo / speaker / line-in plays real chiptune. */
    { "spkr_tone", T_BOOL, 0, 0, 0, NULL,
      "Tone mode (passive speaker)", "mdi:music-note",
      .def = { .b = false } },
};
#define N_ENTRIES (sizeof(SCHEMA) / sizeof(SCHEMA[0]))

/* ---- runtime cache --------------------------------------------------- */
typedef union {
    float   f;
    int32_t i;
    bool    b;
} cfg_val_t;

static cfg_val_t s_cache[N_ENTRIES];
static bool      s_inited = false;

/* mqtt.c provides s_client + connection state; we re-publish via its
 * helpers. To avoid circular linkage, this file calls extern fns. */
extern void mqtt_pub_retained(const char *topic, const char *value);
extern void mqtt_pub(const char *topic, const char *value);
extern bool mqtt_is_connected(void);
extern void mqtt_pub_discovery_raw(const char *topic, const char *payload,
                                   int payload_len);

/* ---- helpers --------------------------------------------------------- */
static int find_entry(const char *key) {
    for (size_t i = 0; i < N_ENTRIES; i++) {
        if (strcmp(SCHEMA[i].key, key) == 0) return (int)i;
    }
    return -1;
}

/* Reject GPIO numbers that would brick the board (strap pins, internal
 * flash/PSRAM, USB JTAG) or collide with other peripherals on this PCB
 * (camera DVP, SD slot, I²C, PDM mic, PIR). Realistically only the
 * unused breakouts are valid — D0..D3 on the XIAO header (GPIO 1, 2,
 * 3, 4). Two of those are already taken on the standard layout (PIR
 * on 2, default IR on 4); runtime overrides exist to let the bench
 * rig pick 3 or the OG 1.
 *
 * Used by every NVS GPIO setter that takes a user-supplied pin number
 * (ir_led_pin, capture_led_pin, ...), and by camera.c at boot to fall
 * back to the compile-time default when stale NVS holds a bad pin.
 * Returns ESP_OK if the pin is safe, ESP_ERR_INVALID_ARG otherwise.
 * The cmd handler should never silently accept a footgun value just
 * because it's in the schema min/max range. */
/* Reject a GPIO that the rev3.2 PCB or the ESP32-S3 silicon itself
 * locks down — strap pins, USB-JTAG D+/D-, SPI flash + octal PSRAM
 * region, fixed camera DVP signals, SD-MMC, PDM mic, status LED.
 *
 * Note: PIR/REED/I²C-bus pins are NOT in this list any more (they were
 * in the old `app_config_validate_safe_gpio` before the pin-function-
 * map refactor retired ir_led_pin/capture_led_pin). Those pads are
 * runtime-assignable via pin_d?_fn now — the singleton/pair invariant
 * checks in the setter (search "Pin function map cross-validation"
 * below) replace per-pin reservation for them.
 *
 * Used only by app_config_init's PIN_SLOT_GPIO sanity check below —
 * the slot GPIOs are compile-time fixed (PCB-pad map), so this is a
 * developer-error tripwire for a future "I added D8=GPIO0" mistake,
 * not a runtime concern. */
static bool pin_is_hardware_reserved(int pin) {
    if (pin == 0 || pin == 45 || pin == 46) return true;  /* strap */
    if (pin == 19 || pin == 20) return true;              /* USB D±   */
    if (pin >= 26 && pin <= 37) return true;              /* flash + octal PSRAM */
    if (pin == SD_CLK_PIN || pin == SD_CMD_PIN || pin == SD_DAT0_PIN)
        return true;
    if (pin == I2S_PDM_CLK_PIN || pin == I2S_PDM_DATA_PIN) return true;
    if (pin == STATUS_LED_PIN) return true;
    if (pin == CAM_PIN_XCLK || pin == CAM_PIN_SIOD || pin == CAM_PIN_SIOC ||
        pin == CAM_PIN_D7 || pin == CAM_PIN_D6 || pin == CAM_PIN_D5 ||
        pin == CAM_PIN_D4 || pin == CAM_PIN_D3 || pin == CAM_PIN_D2 ||
        pin == CAM_PIN_D1 || pin == CAM_PIN_D0 ||
        pin == CAM_PIN_VSYNC || pin == CAM_PIN_HREF || pin == CAM_PIN_PCLK)
        return true;
    return false;
}

/* Label table for power_profile. Index 0..5 maps to the schema's int
 * encoding (0=auto, 1=max, 2=active, 3=eco, 4=sentinel, 5=hibernate). Kept in
 * one place so HA select discovery options, state echo, and cmd parser all stay
 * in lock-step — adding a future tier means one line edit here and the discovery
 * payload picks it up automatically. */
static const char *const POWER_PROFILE_LABELS[] = {
    "auto", "max", "active", "eco", "sentinel", "hibernate",
};
#define POWER_PROFILE_N (sizeof(POWER_PROFILE_LABELS) / sizeof(POWER_PROFILE_LABELS[0]))

static void format_value(int idx, char *out, size_t out_sz) {
    const cfg_entry_t *e = &SCHEMA[idx];
    /* power_profile: emit as a label so HA's select entity gets a value
     * matching its `options` list. The number-typed schema entry stays
     * (NVS still stores int, range-checked 0..5 at load time); only
     * the wire representation differs. */
    if (e->type == T_INT && strcmp(e->key, "power_profile") == 0) {
        int32_t v = s_cache[idx].i;
        if (v >= 0 && v < (int32_t)POWER_PROFILE_N) {
            snprintf(out, out_sz, "%s", POWER_PROFILE_LABELS[v]);
        } else {
            /* Out-of-range NVS made it past load_from_nvs clamp (shouldn't
             * happen) — fall back to the raw int so the operator can spot
             * the bug in the state echo instead of an empty string. */
            snprintf(out, out_sz, "%" PRId32, v);
        }
        return;
    }
    /* Pin function map (pin_dN_fn): same label-emit trick as mode_override
     * — HA's select entity expects the option string, not the enum int. */
    if (e->type == T_INT && pin_slot_index(e->key) >= 0) {
        int32_t v = s_cache[idx].i;
        if (v >= 0 && v < PIN_FN__COUNT) {
            snprintf(out, out_sz, "%s", PIN_FN_LABELS[v]);
        } else {
            snprintf(out, out_sz, "%" PRId32, v);
        }
        return;
    }
    switch (e->type) {
        case T_FLOAT: snprintf(out, out_sz, "%.2f", s_cache[idx].f); break;
        case T_INT:   snprintf(out, out_sz, "%" PRId32, s_cache[idx].i); break;
        case T_BOOL:  snprintf(out, out_sz, "%s", s_cache[idx].b ? "ON" : "OFF"); break;
    }
}

static void publish_state(int idx) {
    char topic[96], val[24];
    snprintf(topic, sizeof(topic), "%s/state/cfg/%s",
             mqtt_topic_base(), SCHEMA[idx].key);
    format_value(idx, val, sizeof(val));
    mqtt_pub_retained(topic, val);
}

static void load_from_nvs(int idx, nvs_handle_t h) {
    const cfg_entry_t *e = &SCHEMA[idx];
    esp_err_t err = ESP_FAIL;
    switch (e->type) {
        case T_FLOAT: {
            uint32_t blob;
            err = nvs_get_u32(h, e->key, &blob);
            if (err == ESP_OK) {
                memcpy(&s_cache[idx].f, &blob, sizeof(float));
                /* Defense against stale NVS persisting through a schema
                 * range shrink (e.g. vad_burst_ms max lowered 120000→60000
                 * in a later firmware). Setter validates on the way in
                 * but the value is already there from a previous build —
                 * clamp + warn so HA discovery (min/max) and the slider
                 * agree with cache. NaN/inf snuck-in values also caught
                 * here: any non-finite fails both range comparisons. */
                if (!(s_cache[idx].f >= e->min && s_cache[idx].f <= e->max)) {
                    ESP_LOGW(TAG, "load %s: %.3f out of schema [%g..%g] — using default %.3f",
                             e->key, (double)s_cache[idx].f, (double)e->min,
                             (double)e->max, (double)e->def.f);
                    s_cache[idx].f = e->def.f;
                }
            } else {
                if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
                    ESP_LOGW(TAG, "load %s: NVS type mismatch — schema "
                                  "changed type, falling to default", e->key);
                }
                s_cache[idx].f = e->def.f;
            }
            break;
        }
        case T_INT:
            err = nvs_get_i32(h, e->key, &s_cache[idx].i);
            if (err == ESP_OK) {
                if (s_cache[idx].i < (int32_t)e->min || s_cache[idx].i > (int32_t)e->max) {
                    ESP_LOGW(TAG, "load %s: %" PRId32 " out of schema [%d..%d] — using default %" PRId32,
                             e->key, s_cache[idx].i, (int)e->min, (int)e->max, e->def.i);
                    s_cache[idx].i = e->def.i;
                }
            } else {
                /* Distinguish "schema type changed under us" from "key
                 * never written" — the former is a real schema-evolution
                 * footgun (cache fell to default and operator's setting
                 * vanished), the latter is normal first-boot behavior. */
                if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
                    ESP_LOGW(TAG, "load %s: NVS type mismatch — schema "
                                  "changed type, falling to default",
                             e->key);
                }
                s_cache[idx].i = e->def.i;
            }
            break;
        case T_BOOL: {
            uint8_t v;
            err = nvs_get_u8(h, e->key, &v);
            if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
                ESP_LOGW(TAG, "load %s: NVS type mismatch — schema "
                              "changed type, falling to default", e->key);
            }
            s_cache[idx].b = (err == ESP_OK) ? !!v : e->def.b;
            break;
        }
    }
}

/* audio.cpp will re-init Vad when these keys change. */
extern void audio_apply_config(void);
/* reed.c arms/disarms the polling task live so the operator's HA toggle
 * doesn't need a reboot to take effect. */
extern void reed_apply_config(void);
/* pir.c flushes motion=false on live OFF toggle so the dashboard
 * doesn't show residual ON for up to 10 s of MOTION_HOLD_MS window. */
extern void pir_apply_config(void);
/* camera.c re-applies vflip/hmirror without reinit. */
extern void camera_apply_orientation(void);
/* camera.c re-applies the tunable OV3660 image levels (brightness/contrast/
 * saturation/sharpness/ae_level/wb_mode/special_fx/gainceiling) live. */
extern void camera_apply_tuning(void);
/* ble.c starts/stops the BLE scan on a live ble_enabled toggle (no-op stub
 * when CONFIG_CHYTRA_BUDKA_BLE is off). */
extern void ble_apply_config(void);
/* sonar.c / soil.c arm/disarm their Grove sensor live. The paired
 * mqtt_publish_discovery() re-run registers the sensors' HA entities the
 * moment they're enabled (their discovery is gated on the enable flag so
 * boards without the hardware don't grow permanently-unknown entities). */
extern void sonar_apply_config(void);
extern void soil_apply_config(void);
extern void mqtt_publish_discovery(void);
/* camera.c profile-apply functions (capture = stills; stream = MJPEG). */
extern esp_err_t camera_apply_capture_profile(void);
extern esp_err_t camera_apply_stream_profile(void);
/* http_server.c exposes whether the MJPEG handler is actively running.
 * Tying the apply gate to "is the handler running" instead of a cached
 * profile flag avoids leaks where the flag survived past handler exit
 * (observed during HIL sweeps — cam_* echoes were published but the
 * sensor stayed in stale stream profile). */
extern bool http_server_mjpg_is_active(void);

static void apply_side_effects(int idx) {
    const char *k = SCHEMA[idx].key;
    if (strncmp(k, "vad_", 4) == 0) audio_apply_config();
    else if (strcmp(k, "reed_enabled") == 0) reed_apply_config();
    else if (strcmp(k, "pir_enabled") == 0) pir_apply_config();
    else if (strcmp(k, "cam_rotate_180") == 0) camera_apply_orientation();
    else if (strcmp(k, "cam_brightness") == 0 || strcmp(k, "cam_contrast") == 0 ||
             strcmp(k, "cam_saturation") == 0 || strcmp(k, "cam_sharpness") == 0 ||
             strcmp(k, "cam_ae_level") == 0 || strcmp(k, "cam_wb_mode") == 0 ||
             strcmp(k, "cam_special_fx") == 0 || strcmp(k, "cam_gainceil") == 0)
        camera_apply_tuning();
    else if (strcmp(k, "ble_enabled") == 0) ble_apply_config();
    else if (strcmp(k, "sonar_enabled") == 0) {
        sonar_apply_config();
        mqtt_publish_discovery();
    }
    else if (strcmp(k, "soil_enabled") == 0) {
        soil_apply_config();
        mqtt_publish_discovery();
    }
    /* soc_* thresholds need no apply hook — main.cpp mode_tick() reads them
     * live each tick. */
    else if (strcmp(k, "ui_lang") == 0) {
        i18n_set_lang((i18n_lang_t)s_cache[idx].i);
        /* HA entity friendly_names come from schema_name() → re-emit discovery
         * so the dashboard relabels in the new language (idempotent; uniq_id +
         * option values unchanged, so entity_ids and state survive). */
        app_config_publish_discovery();
    }
    else if (strcmp(k, "cam_framesize") == 0 || strcmp(k, "cam_quality") == 0) {
        /* MJPEG running → defer; the handler's exit-time
         * camera_apply_capture_profile() re-reads NVS. Otherwise apply
         * now so a still capture immediately reflects the new size/q. */
        if (!http_server_mjpg_is_active()) camera_apply_capture_profile();
    }
    else if (strcmp(k, "mjpg_framesize") == 0 || strcmp(k, "mjpg_quality") == 0) {
        /* Mirror: only re-apply if a stream is actually open right now.
         * Otherwise the new values will land at next /stream.mjpg open. */
        if (http_server_mjpg_is_active()) camera_apply_stream_profile();
    }
    else if (pin_slot_index(k) >= 0) {
        /* Pin function remap: no live re-bind, modules cache their pin
         * at boot-time only. Operator reboots when ready. */
        ESP_LOGW(TAG, "%s changed — reboot required for the new pin "
                      "function map to take effect", k);
    }
}

/* ---- public API ------------------------------------------------------ */
esp_err_t app_config_init(void) {
    if (s_inited) return ESP_OK;
    /* Boot-time guard against silently-broken NVS keys. NVS rejects any
     * key longer than NVS_KEY_NAME_MAX_SIZE - 1 (= 15) with
     * ESP_ERR_NVS_KEY_TOO_LONG. The setter chain handles that as a
     * regular write error, but the symptom in the field is invisible:
     * cache stays at the previous (default) value, retained state echo
     * isn't published, and the HA toggle silently snaps back. We've been
     * burned twice (capture_led_enabled=19ch, reed_debounce_ms=16ch)
     * before this check existed. Loud ESP_LOGE so a dev catches it on
     * the first serial boot of a new key; no abort because we don't
     * want to brick a field unit if somehow an over-limit key slips
     * through to OTA. */
    bool any_bad = false;
    for (size_t i = 0; i < N_ENTRIES; i++) {
        size_t klen = strlen(SCHEMA[i].key);
        if (klen > NVS_KEY_NAME_MAX_SIZE - 1) {
            ESP_LOGE(TAG, "schema bug: key '%s' is %u chars, NVS max is %d "
                          "— every nvs_set will fail, rename in SCHEMA[]",
                     SCHEMA[i].key, (unsigned)klen,
                     (int)(NVS_KEY_NAME_MAX_SIZE - 1));
            any_bad = true;
        }
    }
    if (any_bad) {
        ESP_LOGE(TAG, "one or more NVS keys are unwritable — see above");
    }

    /* Slot-table sanity: every PIN_SLOT_GPIO must be a pin the ESP32-S3
     * actually lets you drive freely. A future rev that adds D8 = GPIO0
     * (strap) or =GPIO27 (flash) would silently brick on first boot
     * otherwise. LOGE but don't abort — same policy as the NVS key
     * check above: surface in serial, never wedge a field unit. */
    for (int slot = 0; slot < PIN_SLOT_COUNT; slot++) {
        int gpio = PIN_SLOT_GPIO[slot];
        if (pin_is_hardware_reserved(gpio)) {
            ESP_LOGE(TAG, "slot table bug: %s -> GPIO%d is hardware-reserved "
                          "(strap/USB-JTAG/flash/PSRAM/camera/SD/PDM/status-LED) "
                          "— driving it via pin_d?_fn will misbehave or brick; "
                          "review PIN_SLOT_GPIO[]",
                     PIN_SLOT_KEYS[slot], gpio);
        }
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s — using defaults", esp_err_to_name(err));
        for (size_t i = 0; i < N_ENTRIES; i++) {
            const cfg_entry_t *e = &SCHEMA[i];
            switch (e->type) {
                case T_FLOAT: s_cache[i].f = e->def.f; break;
                case T_INT:   s_cache[i].i = e->def.i; break;
                case T_BOOL:  s_cache[i].b = e->def.b; break;
            }
        }
    } else {
        for (size_t i = 0; i < N_ENTRIES; i++) load_from_nvs((int)i, h);
        nvs_close(h);
    }
    s_inited = true;
    /* Seed the live UI language from the persisted knob so the first page
     * render (and HA discovery) matches NVS, not just the LANG_CS default. */
    i18n_set_lang((i18n_lang_t)app_config_get_int("ui_lang"));
    for (size_t i = 0; i < N_ENTRIES; i++) {
        char buf[24];
        format_value((int)i, buf, sizeof(buf));
        ESP_LOGI(TAG, "  %s = %s", SCHEMA[i].key, buf);
    }
    /* ── Pin-map audit (advisory) ─────────────────────────────────────
     * The setter refuses NEW bad assignments, but legacy NVS written by a
     * pre-guard firmware can still hold a wake source on a non-RTC pad, or
     * an under-configured bus0. Surface both in the boot log so a "won't
     * wake" / "no gauge" board is one grep away, not a multi-hour mystery.
     * Advisory only — never mutates NVS (silent self-edits violate
     * least-surprise; the operator/setter fixes it). */
    {
        int i2c0_sda = 0, i2c0_scl = 0;
        for (int s = 0; s < PIN_SLOT_COUNT; s++) {
            int oi = find_entry(PIN_SLOT_KEYS[s]);
            if (oi < 0) continue;
            pin_fn_t fn = (pin_fn_t)s_cache[oi].i;
            if (fn == PIN_FN_I2C0_SDA) i2c0_sda++;
            if (fn == PIN_FN_I2C0_SCL) i2c0_scl++;
            if ((fn == PIN_FN_PIR || fn == PIN_FN_REED || fn == PIN_FN_BUTTON) &&
                !rtc_gpio_is_valid_gpio((gpio_num_t)PIN_SLOT_GPIO[s])) {
                ESP_LOGW(TAG, "pin-map: %s=%s on GPIO%d is NOT RTC-capable — "
                              "this wake source can't wake deep sleep "
                              "(move to D0..D5 = GPIO1..6, or reassign)",
                         PIN_SLOT_KEYS[s], PIN_FN_LABELS[fn], PIN_SLOT_GPIO[s]);
            }
        }
        if (i2c0_sda != 1 || i2c0_scl != 1)
            ESP_LOGE(TAG, "pin-map: primary I2C bus0 mis-configured (%d sda, "
                          "%d scl — need exactly 1 each); the gauge/SHT41/"
                          "charger bus will not come up", i2c0_sda, i2c0_scl);
    }
    return ESP_OK;
}

esp_err_t app_config_reset_defaults(void) {
    /* Wipe the cb_cfg namespace and restore every cached value to its
     * schema default. Recovers a wedged config (e.g. mode_override=3 Safe,
     * which disables the audio path so it can't be undone over a degraded
     * link) without a reflash. Callers typically reboot afterward so
     * pin-map + apply-on-change side effects re-run cleanly from a known
     * state. WiFi creds + TLS certs live in their own namespaces and are
     * NOT touched here (see wifi_store / tls_store). */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reset_defaults: nvs erase: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < N_ENTRIES; i++) {
        const cfg_entry_t *e = &SCHEMA[i];
        switch (e->type) {
            case T_FLOAT: s_cache[i].f = e->def.f; break;
            case T_INT:   s_cache[i].i = e->def.i; break;
            case T_BOOL:  s_cache[i].b = e->def.b; break;
        }
    }
    ESP_LOGW(TAG, "config reset to schema defaults (cb_cfg erased)");
    /* Reflect the reset in HA even if the caller doesn't reboot. */
    app_config_publish_state_all();
    return ESP_OK;
}

/* Human-readable resolution for an esp_camera framesize index (only the
 * useful >= VGA range; sub-VGA indices are junk on the OV3660). NULL for
 * indices we don't surface in the dropdown. */
static const char *framesize_label(int v) {
    switch (v) {
        case 10: return "VGA 640×480 (4:3)";
        case 11: return "SVGA 800×600 (4:3)";
        case 12: return "XGA 1024×768 (4:3)";
        case 13: return "HD 1280×720 (16:9)";
        case 14: return "SXGA 1280×1024 (5:4)";
        case 15: return "UXGA 1600×1200 (4:3)";
        default: return NULL;
    }
}

/* Map a schema key to its translatable friendly-name ID. Keyed (not index-
 * aligned with SCHEMA[]) so it can't silently desync from schema order; a key
 * absent here falls back to the English SCHEMA[].name, so adding a schema entry
 * without a translation degrades gracefully. */
static const struct {
    const char *key;
    i18n_str_t  id;
} SCHEMA_NAME[] = {
    {"vad_enabled", STR_CFG_VAD_ENABLED}, {"vad_thr_dbfs", STR_CFG_VAD_THR},
    {"vad_burst_ms", STR_CFG_VAD_BURST}, {"vad_rearm_ms", STR_CFG_VAD_REARM},
    /* power_profile + the renamed tlm_/soc_/ds_ keys fall back to their
     * English SCHEMA[].name (Czech labels for the important ones live in
     * the HA strategy dashboard). */
    {"cam_enabled", STR_CFG_CAM_ENABLED},
    {"tlapse_min", STR_CFG_TLAPSE}, {"pir_enabled", STR_CFG_PIR_ENABLED},
    {"reed_enabled", STR_CFG_REED_ENABLED}, {"reed_db_ms", STR_CFG_REED_DB},
    {"cam_rotate_180", STR_CFG_CAM_ROTATE}, {"cam_framesize", STR_CFG_CAM_FRAMESIZE},
    {"cam_quality", STR_CFG_CAM_QUALITY}, {"mjpg_framesize", STR_CFG_MJPG_FRAMESIZE},
    {"mjpg_quality", STR_CFG_MJPG_QUALITY}, {"ir_led_enabled", STR_CFG_IR_LED_ENABLED},
    {"ir_agc_thresh", STR_CFG_IR_AGC}, {"cap_led_en", STR_CFG_CAP_LED},
    {"ota_enabled", STR_CFG_OTA_ENABLED}, {"flac_enabled", STR_CFG_FLAC_ENABLED},
    {"status_led_en", STR_CFG_STATUS_LED_EN}, {"status_led_dbg", STR_CFG_STATUS_LED_DBG},
    {"sd_autoprune", STR_CFG_SD_AUTOPRUNE}, {"sd_min_free", STR_CFG_SD_MIN_FREE},
    {"sd_keep_days", STR_CFG_SD_KEEP_DAYS},
    {"uart_baud", STR_CFG_UART_BAUD}, {"pin_d0_fn", STR_CFG_PIN_D0},
    {"pin_d1_fn", STR_CFG_PIN_D1}, {"pin_d2_fn", STR_CFG_PIN_D2},
    {"pin_d3_fn", STR_CFG_PIN_D3}, {"pin_d4_fn", STR_CFG_PIN_D4},
    {"pin_d5_fn", STR_CFG_PIN_D5}, {"pin_d6_fn", STR_CFG_PIN_D6},
    {"pin_d7_fn", STR_CFG_PIN_D7}, {"ui_lang", STR_CFG_UI_LANG},
};
/* Translated friendly name for schema entry idx (English SCHEMA[].name fallback).
 * Used by both the local /config form row and HA discovery, so a single
 * ui_lang flip relabels both surfaces. */
static const char *schema_name(int idx) {
    const char *key = SCHEMA[idx].key;
    for (size_t j = 0; j < sizeof(SCHEMA_NAME) / sizeof(SCHEMA_NAME[0]); j++)
        if (strcmp(SCHEMA_NAME[j].key, key) == 0) return tr(SCHEMA_NAME[j].id);
    return SCHEMA[idx].name;
}

size_t app_config_count(void) { return N_ENTRIES; }

const char *app_config_key_at(size_t i) {
    return (i < N_ENTRIES) ? SCHEMA[i].key : NULL;
}

bool app_config_form_row(size_t i, char *buf, size_t cap) {
    if (i >= N_ENTRIES || !buf || cap == 0) return false;
    const cfg_entry_t *e = &SCHEMA[i];
    char cur[24];
    format_value((int)i, cur, sizeof(cur));
    /* Build with text_append (truncation-safe): the old `n += snprintf(buf+n,
     * cap-n, …)` accumulator underflows `cap-n` to ~4 GB and scribbles past
     * `buf` the moment any single append would truncate. Worst-case row (the
     * pin-function <select>) fits 512 B today, but a longer label / extra pin
     * function would have flipped it to a stack smash. text_append clamps. */
    char  *p    = buf;
    size_t left = cap;
    text_append(&p, &left, "<p><label>%s<br>", schema_name((int)i));
    if (e->type == T_BOOL) {
        text_append(&p, &left,
                    "<select name=\"%s\"><option%s>ON</option>"
                    "<option%s>OFF</option></select>",
                    e->key, strcmp(cur, "ON") == 0 ? " selected" : "",
                    strcmp(cur, "OFF") == 0 ? " selected" : "");
    } else if (strcmp(e->key, "power_profile") == 0) {
        text_append(&p, &left, "<select name=\"%s\">", e->key);
        for (size_t k = 0; k < POWER_PROFILE_N; k++)
            text_append(&p, &left, "<option%s>%s</option>",
                        strcmp(cur, POWER_PROFILE_LABELS[k]) == 0 ? " selected" : "",
                        POWER_PROFILE_LABELS[k]);
        text_append(&p, &left, "</select>");
    } else if (strcmp(e->key, "ui_lang") == 0) {
        /* Named language dropdown; option value is the i18n_lang_t int so the
         * form submits the enum and app_config_set_from_string stores it. */
        int curv = (int)strtol(cur, NULL, 10);
        text_append(&p, &left, "<select name=\"%s\">", e->key);
        for (int k = 0; k < LANG_COUNT; k++)
            text_append(&p, &left, "<option value=%d%s>%s</option>", k,
                        k == curv ? " selected" : "", I18N_LANG_NAMES[k]);
        text_append(&p, &left, "</select>");
    } else if (pin_slot_index(e->key) >= 0) {
        text_append(&p, &left, "<select name=\"%s\">", e->key);
        for (int fn = 0; fn < PIN_FN__COUNT; fn++)
            text_append(&p, &left, "<option%s>%s</option>",
                        strcmp(cur, PIN_FN_LABELS[fn]) == 0 ? " selected" : "",
                        PIN_FN_LABELS[fn]);
        text_append(&p, &left, "</select>");
    } else if (strcmp(e->key, "cam_framesize") == 0 ||
               strcmp(e->key, "mjpg_framesize") == 0) {
        /* Labelled resolution dropdown — the raw 0..15 index is meaningless. */
        int curv = (int)strtol(cur, NULL, 10);
        bool in_range = (curv >= 10 && curv <= 15);
        text_append(&p, &left, "<select name=\"%s\">", e->key);
        for (int v = 10; v <= 15; v++)
            text_append(&p, &left, "<option value=%d%s>%s</option>", v,
                        v == curv ? " selected" : "", framesize_label(v));
        if (!in_range) /* keep an out-of-range current value selectable */
            text_append(&p, &left,
                        "<option value=%d selected>index %d</option>", curv, curv);
        text_append(&p, &left, "</select>");
    } else {
        char mn[16], mx[16], st[16];
        if (e->type == T_FLOAT) {
            snprintf(mn, sizeof(mn), "%.2f", (double)e->min);
            snprintf(mx, sizeof(mx), "%.2f", (double)e->max);
            snprintf(st, sizeof(st), "%.2f", (double)e->step);
        } else {
            snprintf(mn, sizeof(mn), "%d", (int)e->min);
            snprintf(mx, sizeof(mx), "%d", (int)e->max);
            snprintf(st, sizeof(st), "%d", (int)e->step);
        }
        /* JPEG quality is "lower = better + bigger" — non-obvious, so hint it. */
        const char *hint = "";
        if (strcmp(e->key, "cam_quality") == 0 || strcmp(e->key, "mjpg_quality") == 0)
            hint = tr(STR_HINT_QUALITY);
        text_append(&p, &left,
                    "<input type=number name=\"%s\" value=\"%s\" min=%s max=%s "
                    "step=%s> %s%s",
                    e->key, cur, mn, mx, st, e->unit ? e->unit : "", hint);
    }
    text_append(&p, &left, "</label></p>");
    return true;
}

/* ---- Pin function map helpers --------------------------------------------
 *
 * Both helpers walk the 8 pin slot knobs in fixed D0..D7 order, look up
 * each cached enum int, and match the requested function label. NVS
 * load already clamped any out-of-range value to the schema default at
 * boot (see load_from_nvs) — these helpers don't re-validate. Cheap
 * enough to call at every module init; modules generally cache the
 * result for the lifetime of the boot since the map can't change
 * without a reboot. */
size_t app_config_pins_for(const char *fn_label, int *out, size_t out_max) {
    int want = pin_fn_from_label(fn_label);
    if (want < 0) return 0;
    atomic_thread_fence(memory_order_acquire);
    size_t found = 0;
    for (int slot = 0; slot < PIN_SLOT_COUNT; slot++) {
        int idx = find_entry(PIN_SLOT_KEYS[slot]);
        if (idx < 0) continue;
        if (s_cache[idx].i == want) {
            if (out && found < out_max) {
                out[found] = PIN_SLOT_GPIO[slot];
            }
            found++;
        }
    }
    /* Cap the return to out_max when the caller provided a buffer — the
     * header contract promises "capped at out_max", and reed_init/pir_init
     * use the return as the loop bound for s_inst[REED_MAX_INSTANCES=4].
     * Without the cap, mapping 5+ slots to "reed" (8 slots exist) walks
     * past the array end. Log the truncation so the misconfig surfaces in
     * the boot log instead of silently losing the extra instances.
     *
     * Probe mode (out=NULL, out_max=0): keep returning the true count so
     * callers can size their buffer correctly. */
    if (out && found > out_max) {
        ESP_LOGW(TAG,
                 "fn '%s' mapped to %zu slots but caller buffer holds only %zu — "
                 "extra instances ignored; reduce pin_d?_fn mappings or raise the cap",
                 fn_label, found, out_max);
        found = out_max;
    }
    return found;
}

int app_config_pin_for_first(const char *fn_label) {
    int gpio;
    return (app_config_pins_for(fn_label, &gpio, 1) > 0) ? gpio : -1;
}

bool app_config_pin_slot_info(int slot, int *gpio_out, const char **fn_label_out) {
    if (slot < 0 || slot >= PIN_SLOT_COUNT) return false;
    if (gpio_out) *gpio_out = PIN_SLOT_GPIO[slot];
    if (fn_label_out) {
        atomic_thread_fence(memory_order_acquire);
        int idx = find_entry(PIN_SLOT_KEYS[slot]);
        int fn = (idx >= 0) ? s_cache[idx].i : 0;
        if (fn < 0 || fn >= PIN_FN__COUNT) fn = 0;
        *fn_label_out = PIN_FN_LABELS[fn];
    }
    return true;
}

int app_config_pin_slot_count(void) { return PIN_SLOT_COUNT; }

/* Acquire fence pairs with the release fence in
 * app_config_set_from_string so a cross-core reader observes any
 * write that happened-before the setter's fence. See the setter for
 * rationale (compiler-ordering barrier, internal SRAM is HW-coherent). */
float app_config_get_float(const char *key) {
    int i = find_entry(key);
    if (i < 0 || SCHEMA[i].type != T_FLOAT) return 0.0f;
    atomic_thread_fence(memory_order_acquire);
    return s_cache[i].f;
}
int32_t app_config_get_int(const char *key) {
    int i = find_entry(key);
    if (i < 0 || SCHEMA[i].type != T_INT) return 0;
    atomic_thread_fence(memory_order_acquire);
    return s_cache[i].i;
}
bool app_config_get_bool(const char *key) {
    int i = find_entry(key);
    if (i < 0 || SCHEMA[i].type != T_BOOL) return false;
    atomic_thread_fence(memory_order_acquire);
    return s_cache[i].b;
}

esp_err_t app_config_set_from_string(const char *key, const char *value) {
    int idx = find_entry(key);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    const cfg_entry_t *e = &SCHEMA[idx];

    /* Parse + range-check into a local. Don't touch the cache yet —
     * if NVS persist fails we don't want callers to see a phantom "OK"
     * with a value that won't survive reboot. */
    cfg_val_t parsed = {0};
    char *end = NULL;
    switch (e->type) {
        case T_FLOAT: {
            parsed.f = strtof(value, &end);
            /* `!(x >= min && x <= max)` rather than `x < min || x > max`
             * so NaN fails the range check — both comparisons against
             * NaN return false, which the bare `< || >` form accepts as
             * "in range". strtof returns NaN for "nan" / "NaN" and inf
             * for "inf" / "Infinity"; both would silently corrupt the
             * VAD threshold or any other float knob. */
            if (end == value || !(parsed.f >= e->min && parsed.f <= e->max)) {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        }
        case T_INT: {
            long v;
            /* power_profile accepts the named labels (HA select sends
             * "auto"/"max"/"active"/"eco"/"sentinel"/"hibernate") plus the
             * raw int for scripts that do `mosquitto_pub … -m 4`. Label
             * match takes precedence so a future user-renamed schema label
             * doesn't get misinterpreted as a number. */
            if (strcmp(e->key, "power_profile") == 0) {
                v = -1;
                for (size_t i = 0; i < POWER_PROFILE_N; i++) {
                    if (strcmp(value, POWER_PROFILE_LABELS[i]) == 0) {
                        v = (long)i;
                        break;
                    }
                }
                if (v < 0) {
                    v = strtol(value, &end, 10);
                    if (end == value) return ESP_ERR_INVALID_ARG;
                }
            } else if (pin_slot_index(e->key) >= 0) {
                /* pin_dN_fn: accept the function label ("reed",
                 * "uart_tx", …) from HA select; fall back to the raw
                 * enum int for scripts that want it numeric. Same
                 * pattern as power_profile. */
                int fn = pin_fn_from_label(value);
                if (fn >= 0) {
                    v = fn;
                } else {
                    v = strtol(value, &end, 10);
                    if (end == value) return ESP_ERR_INVALID_ARG;
                }
            } else {
                v = strtol(value, &end, 10);
                if (end == value) return ESP_ERR_INVALID_ARG;
            }
            if (v < (long)e->min || v > (long)e->max) {
                return ESP_ERR_INVALID_ARG;
            }
            parsed.i = (int32_t)v;

            /* Pin function map cross-validation.
             *
             * Build a "what would the full map look like if we accept
             * this write" view: every other slot keeps its cached
             * value, this slot takes `parsed.i`. Then enforce two
             * invariants:
             *
             *   1. Singleton functions (ir_led, capture_led, uart_tx,
             *      uart_rx) — at most one slot may carry them. A second
             *      assignment is a configuration mistake the operator
             *      almost certainly didn't intend; reject up-front.
             *
             *   2. Pair functions (i2c0_sda↔scl, i2c1_*↔, uart_*↔) —
             *      they must appear together or not at all. We reject
             *      a setter call only when the OPERATOR'S change would
             *      break the pair (was paired, now isn't). The reverse
             *      transition — operator MUST be able to remove the
             *      pair entirely by going through an intermediate
             *      half-broken state — is permitted, but logged WARN
             *      so the operator sees the half-paired state.
             *
             * Bonus warning for D6/D7: GPIO43/44 are also the chip's
             * UART1 boot console pins. Remapping them away from
             * i2c1_* loses USB-serial boot logs. Field unit doesn't
             * care, bench operator gets the warn. */
            int slot = pin_slot_index(e->key);
            if (slot >= 0) {
                pin_fn_t new_fn = (pin_fn_t)parsed.i;
                /* RTC-capability guard. The EXT1 deep-sleep wake sources
                 * (pir/reed/button) can only wake hibernate from an
                 * RTC-capable GPIO (ESP32-S3: GPIO0..21). Assigning one to
                 * a non-RTC pad (D6/D7 = GPIO43/44) used to succeed silently
                 * and only surfaced as a one-liner at sleep entry — the box
                 * then "wouldn't stay asleep" / "wouldn't wake on the door".
                 * Refuse up front. (Outputs/I²C/UART never wake, so they're
                 * free to live on the non-RTC pads.) */
                if ((new_fn == PIN_FN_PIR || new_fn == PIN_FN_REED ||
                     new_fn == PIN_FN_BUTTON) &&
                    !rtc_gpio_is_valid_gpio((gpio_num_t)PIN_SLOT_GPIO[slot])) {
                    ESP_LOGW(TAG, "set %s = %s: GPIO%d is not RTC-capable — a "
                                  "wake source here can't wake deep sleep; "
                                  "refusing (use D0..D5 = GPIO1..6)",
                             e->key, PIN_FN_LABELS[new_fn], PIN_SLOT_GPIO[slot]);
                    return ESP_ERR_INVALID_ARG;
                }
                /* ADC guard: the "soil" function is an analog input —
                 * only ADC1 pads qualify (ESP32-S3: GPIO1..10, i.e.
                 * D0..D5 on this header). D6/D7 = GPIO43/44 have no ADC
                 * at all; accepting them would arm a module that can
                 * never read. Refuse up front, same policy as the RTC
                 * wake-source guard above. */
                if (new_fn == PIN_FN_SOIL &&
                    !(PIN_SLOT_GPIO[slot] >= 1 && PIN_SLOT_GPIO[slot] <= 10)) {
                    ESP_LOGW(TAG, "set %s = %s: GPIO%d has no ADC1 channel — "
                                  "the analog soil probe can't be read here; "
                                  "refusing (use D0..D5 = GPIO1..6)",
                             e->key, PIN_FN_LABELS[new_fn], PIN_SLOT_GPIO[slot]);
                    return ESP_ERR_INVALID_ARG;
                }
                /* Singleton check across all 8 slots. */
                if (pin_fn_is_singleton(new_fn) && new_fn != PIN_FN_NONE) {
                    for (int j = 0; j < PIN_SLOT_COUNT; j++) {
                        if (j == slot) continue;
                        int oi = find_entry(PIN_SLOT_KEYS[j]);
                        if (oi >= 0 && s_cache[oi].i == (int32_t)new_fn) {
                            ESP_LOGW(TAG, "set %s = %s: function is "
                                          "singleton, already on slot %s "
                                          "(GPIO%d) — refusing",
                                     e->key, PIN_FN_LABELS[new_fn],
                                     PIN_SLOT_KEYS[j], PIN_SLOT_GPIO[j]);
                            return ESP_ERR_INVALID_ARG;
                        }
                    }
                }
                /* Pair check: count how many slots have new_fn's pair
                 * vs new_fn itself in the post-write state. We warn
                 * loudly when the change leaves a half-paired map. */
                pin_fn_t pair = pin_fn_pair_of(new_fn);
                if (pair != PIN_FN__COUNT) {
                    int new_count = 1;  /* counts THIS slot post-write */
                    int pair_count = 0;
                    for (int j = 0; j < PIN_SLOT_COUNT; j++) {
                        if (j == slot) continue;
                        int oi = find_entry(PIN_SLOT_KEYS[j]);
                        if (oi < 0) continue;
                        if (s_cache[oi].i == (int32_t)new_fn) new_count++;
                        if (s_cache[oi].i == (int32_t)pair)   pair_count++;
                    }
                    if (new_count != pair_count) {
                        ESP_LOGW(TAG, "set %s = %s: leaves map half-paired "
                                      "(%d %s vs %d %s) — the pair won't "
                                      "be functional until the partner is "
                                      "also reassigned",
                                 e->key, PIN_FN_LABELS[new_fn],
                                 new_count, PIN_FN_LABELS[new_fn],
                                 pair_count, PIN_FN_LABELS[pair]);
                        /* warn only; permit so the operator can do the
                         * second leg of the move */
                    }
                }
                /* D6/D7 boot-console caveat. GPIO43/44 carry the ROM UART0
                 * boot log; using them for I²C (the intended bus0/bus1 home)
                 * is fine — the bus tolerates the brief boot toggle and the
                 * console is on USB-CDC. Warn only for a NON-I²C peripheral,
                 * which loses the UART boot log for no real gain. */
                if ((slot == 6 || slot == 7) &&
                    new_fn != PIN_FN_I2C0_SDA && new_fn != PIN_FN_I2C0_SCL &&
                    new_fn != PIN_FN_I2C1_SDA && new_fn != PIN_FN_I2C1_SCL &&
                    new_fn != PIN_FN_NONE) {
                    ESP_LOGW(TAG, "set %s = %s: GPIO%d is the chip's UART0 "
                                  "boot-console pad — prefer it for I²C "
                                  "(bus0/bus1); a non-I²C peripheral here "
                                  "loses the bootloader UART log for no gain.",
                             e->key, PIN_FN_LABELS[new_fn],
                             PIN_SLOT_GPIO[slot]);
                    /* warn only; the operator asked for it */
                }
            }
            break;
        }
        case T_BOOL: {
            /* Strict whitelist for both directions. Previously the parse
             * was "ON/1/true → true, anything else → false", which meant
             * a typo'd "OFFF" or stdin-piped "OFF\n" silently flipped
             * the entity OFF without an error. Now garbage is rejected
             * so an operator's broken command surfaces in the cfg log
             * line ("set X: invalid payload") instead of doing the
             * wrong thing quietly. */
            bool truthy = (strcasecmp(value, "ON")   == 0 ||
                           strcmp(value, "1")        == 0 ||
                           strcasecmp(value, "true") == 0);
            bool falsy  = (strcasecmp(value, "OFF")   == 0 ||
                           strcmp(value, "0")         == 0 ||
                           strcasecmp(value, "false") == 0);
            if (!truthy && !falsy) {
                return ESP_ERR_INVALID_ARG;
            }
            parsed.b = truthy;
            break;
        }
    }

    /* Skip the write path entirely when the value is unchanged. IDF's
     * NVS does its own pre-write compare and won't actually rewrite a
     * flash entry whose bytes match, but the open/set/commit/close
     * cycle still costs a few ms + we'd needlessly re-run
     * apply_side_effects (audio_apply_config kicks a VAD reconfig req
     * which audio_task on CPU1 picks up on its next pump iteration —
     * harmless, but pointless when nothing changed). HA automations
     * that mirror the current state on a schedule (or that round-trip
     * through state→cmd via someone's misconfigured node-RED flow)
     * would otherwise saturate the cmd path. Re-publish state once so
     * a stale retained value (e.g. broker just restarted with a clean
     * store) gets a fresh echo. */
    bool unchanged = false;
    switch (e->type) {
        case T_FLOAT: unchanged = (s_cache[idx].f == parsed.f); break;
        case T_INT:   unchanged = (s_cache[idx].i == parsed.i); break;
        case T_BOOL:  unchanged = (s_cache[idx].b == parsed.b); break;
    }
    if (unchanged) {
        publish_state(idx);
        return ESP_OK;
    }

    /* Persist first; commit to cache only on success. */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set %s: nvs_open failed: %s",
                 e->key, esp_err_to_name(err));
        return err;
    }
    switch (e->type) {
        case T_FLOAT: {
            uint32_t blob;
            memcpy(&blob, &parsed.f, sizeof(float));
            err = nvs_set_u32(h, e->key, blob);
            break;
        }
        case T_INT:
            err = nvs_set_i32(h, e->key, parsed.i);
            break;
        case T_BOOL:
            err = nvs_set_u8(h, e->key, parsed.b ? 1 : 0);
            break;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set %s: nvs write failed: %s",
                 e->key, esp_err_to_name(err));
        return err;
    }

    /* Persist succeeded — now commit to in-memory cache and announce.
     * Apply BEFORE publish so the state/cfg/<key> echo is a real "ready"
     * signal: a HIL test that publishes cmd/cfg/cam_framesize then waits
     * for the echo can immediately trigger a capture, confident the
     * sensor is already at the new framesize (not mid-reconfigure).
     *
     * Release fence after the cache write pairs with the acquire fence
     * in the getters so a cross-core reader (audio_task on CPU1 reading
     * vad_thr_dbfs while MQTT task on CPU0 writes it) can never see a
     * stale value carried over by compiler register-caching across
     * unrelated function calls. ESP32-S3's internal SRAM is hardware-
     * coherent across cores, so this is a *compiler*-ordering barrier,
     * not a cache flush. */
    s_cache[idx] = parsed;
    atomic_thread_fence(memory_order_release);
    apply_side_effects(idx);
    publish_state(idx);

    char buf[24];
    format_value(idx, buf, sizeof(buf));
    ESP_LOGI(TAG, "set %s = %s", e->key, buf);
    return ESP_OK;
}

void app_config_publish_state_all(void) {
    if (!mqtt_is_connected()) return;
    for (size_t i = 0; i < N_ENTRIES; i++) publish_state((int)i);
}

void app_config_publish_discovery(void) {
    if (!mqtt_is_connected()) return;

    const char *id     = device_id();
    const char *base   = mqtt_topic_base();
    const char *avail  = mqtt_topic_availability();
    const char *devblk = mqtt_device_block();

    char topic[160], payload[700];

    /* One-shot migration cleanup: empty-retained payload to the discovery
     * topics of keys removed/renamed in the power-model redesign, so HA drops
     * the orphan entities (otherwise stale sliders/switches linger next to the
     * new ones). Harmless to keep republishing — the topic is gone after the
     * first empty payload. {component, old_key}. */
    static const struct { const char *comp; const char *key; } REMOVED[] = {
        { "select", "mode_override" }, { "number", "mode_override" },
        { "switch", "pm_lightsleep" },
        { "number", "tlm_cont_s" },     { "number", "tlm_trig_s" },
        { "number", "tlm_safe_s" },
        { "number", "soc_cont_enter" }, { "number", "soc_cont_leave" },
        { "number", "soc_safe_enter" }, { "number", "soc_safe_leave" },
    };
    for (size_t r = 0; r < sizeof(REMOVED) / sizeof(REMOVED[0]); r++) {
        snprintf(topic, sizeof(topic), "homeassistant/%s/%s/cfg_%s/config",
                 REMOVED[r].comp, id, REMOVED[r].key);
        mqtt_pub_discovery_raw(topic, "", 0);
    }

    for (size_t i = 0; i < N_ENTRIES; i++) {
        const cfg_entry_t *e = &SCHEMA[i];

        /* power_profile: a `select` entity (number slider was a UX trap —
         * operators had to remember the int→tier mapping). Options come
         * straight from POWER_PROFILE_LABELS so the dropdown, state echo,
         * and cmd parser never drift. NVS stores the int internally (T_INT);
         * format_value + the setter translate at the wire boundary. */
        if (strcmp(e->key, "power_profile") == 0) {
            /* Build the options array from the label table (JSON-escaped). */
            char options[128];
            int opt_pos = 0;
            options[opt_pos++] = '[';
            for (size_t k = 0; k < POWER_PROFILE_N; k++)
                opt_pos += snprintf(options + opt_pos, sizeof(options) - opt_pos,
                                    "%s\"%s\"", k == 0 ? "" : ",",
                                    POWER_PROFILE_LABELS[k]);
            options[opt_pos++] = ']';
            options[opt_pos] = 0;

            snprintf(topic, sizeof(topic),
                     "homeassistant/select/%s/cfg_power_profile/config", id);
            /* power_profile is the ONE config entity with NO availability topic
             * and a RETAINED command — deliberately, so a hibernating board can
             * be re-tasked while it's asleep: HA lets you pick a profile even
             * when the device shows offline (no avty_t), and publishes it
             * retained, so the broker hands it to the board the instant it wakes
             * and re-subscribes. profile_tick() (1 Hz) reads the new NVS value
             * and enter_profile()s out of hibernate on the next tick. (Trade-off:
             * HA is then authoritative — a retained pick re-applies on every
             * reconnect, overriding a local web-UI change until HA is changed.) */
            snprintf(payload, sizeof(payload),
                "{\"name\":\"%s\","
                "\"uniq_id\":\"%s_cfg_%s\","
                "\"stat_t\":\"%s/state/cfg/%s\","
                "\"cmd_t\":\"%s/cmd/cfg/%s\","
                "\"options\":%s,"
                "\"icon\":\"%s\","
                "\"ret\":true,"
                "%s,"
                "\"ent_cat\":\"config\"}",
                schema_name((int)i), id, e->key,
                base, e->key, base, e->key,
                options,
                e->icon ? e->icon : "mdi:cog-transfer",
                devblk);
            mqtt_pub_discovery_raw(topic, payload, (int)strlen(payload));
            continue;
        }

        /* Pin function map: emit each pin_dN_fn slot as a `select` with
         * the full PIN_FN_LABELS list. The whole inventory is exposed
         * as the options list — operator picks what each pin should
         * do. Validation lives in the setter (singleton + pair + D6/D7
         * warn); HA just sees a flat dropdown. */
        if (pin_slot_index(e->key) >= 0) {
            snprintf(topic, sizeof(topic),
                     "homeassistant/select/%s/cfg_%s/config", id, e->key);
            /* Build the options array with proper JSON escaping. With
             * 11 labels at ~10 chars each plus quoting + commas it's
             * about 130 chars, well within our payload buffer. */
            char options[256];
            int opt_pos = 0;
            options[opt_pos++] = '[';
            for (int fn = 0; fn < PIN_FN__COUNT; fn++) {
                opt_pos += snprintf(options + opt_pos, sizeof(options) - opt_pos,
                                    "%s\"%s\"", fn == 0 ? "" : ",",
                                    PIN_FN_LABELS[fn]);
            }
            options[opt_pos++] = ']';
            options[opt_pos] = 0;
            snprintf(payload, sizeof(payload),
                "{\"name\":\"%s\","
                "\"uniq_id\":\"%s_cfg_%s\","
                "\"stat_t\":\"%s/state/cfg/%s\","
                "\"cmd_t\":\"%s/cmd/cfg/%s\","
                "\"options\":%s,"
                "\"icon\":\"%s\","
                "\"avty_t\":\"%s\","
                "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
                "%s,"
                "\"ent_cat\":\"config\"}",
                schema_name((int)i), id, e->key,
                base, e->key, base, e->key,
                options,
                e->icon ? e->icon : "mdi:gpio",
                avail, devblk);
            mqtt_pub_discovery_raw(topic, payload, (int)strlen(payload));
            continue;
        }

        const char *comp = (e->type == T_BOOL) ? "switch" : "number";
        snprintf(topic, sizeof(topic),
                 "homeassistant/%s/%s/cfg_%s/config", comp, id, e->key);

        if (e->type == T_BOOL) {
            snprintf(payload, sizeof(payload),
                "{\"name\":\"%s\","
                "\"uniq_id\":\"%s_cfg_%s\","
                "\"stat_t\":\"%s/state/cfg/%s\","
                "\"cmd_t\":\"%s/cmd/cfg/%s\","
                "\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
                "\"icon\":\"%s\","
                "\"avty_t\":\"%s\","
                "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
                "%s,"
                "\"ent_cat\":\"config\"}",
                schema_name((int)i), id, e->key,
                base, e->key, base, e->key,
                e->icon ? e->icon : "mdi:cog",
                avail, devblk);
        } else {
            /* min/max/step are float-typed in the schema struct; pick the
             * format AND the argument cast together. Previous version
             * always passed the float value to whichever printf format —
             * `%d` reading a varargs-promoted double from the wrong stack
             * offset yields 0 on Xtensa, which made every T_INT number
             * entity publish `min:0 max:0 step:0`. HA refuses degenerate
             * ranges and silently drops the entity, breaking the dashboard
             * (vad_burst_ms, vad_rearm_ms, tlm_*_s, ir_led_pin,
             * ir_agc_thresh, capture_led_pin all affected). */
            char min_s[16], max_s[16], step_s[16];
            if (e->type == T_FLOAT) {
                snprintf(min_s,  sizeof(min_s),  "%.2f", (double)e->min);
                snprintf(max_s,  sizeof(max_s),  "%.2f", (double)e->max);
                snprintf(step_s, sizeof(step_s), "%.2f", (double)e->step);
            } else {
                snprintf(min_s,  sizeof(min_s),  "%d", (int)e->min);
                snprintf(max_s,  sizeof(max_s),  "%d", (int)e->max);
                snprintf(step_s, sizeof(step_s), "%d", (int)e->step);
            }
            snprintf(payload, sizeof(payload),
                "{\"name\":\"%s\","
                "\"uniq_id\":\"%s_cfg_%s\","
                "\"stat_t\":\"%s/state/cfg/%s\","
                "\"cmd_t\":\"%s/cmd/cfg/%s\","
                "\"min\":%s,\"max\":%s,\"step\":%s,"
                "\"unit_of_meas\":\"%s\","
                "\"icon\":\"%s\","
                "\"mode\":\"box\","
                "\"avty_t\":\"%s\","
                "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
                "%s,"
                "\"ent_cat\":\"config\"}",
                schema_name((int)i), id, e->key,
                base, e->key, base, e->key,
                min_s, max_s, step_s,
                e->unit ? e->unit : "",
                e->icon ? e->icon : "mdi:tune",
                avail, devblk);
        }
        mqtt_pub_discovery_raw(topic, payload, (int)strlen(payload));
    }

    /* Clear retained discovery + state for NVS keys renamed in past
     * firmware versions. NVS rejects keys >15 chars, so the old
     * `capture_led_enabled` (19 chars) and `reed_debounce_ms` (16
     * chars) never persisted writes — but the discovery topics *were*
     * published and stuck around in the broker's retained store,
     * leaving zombie HA entities that the operator could click but
     * couldn't change. Publishing an empty payload to the discovery
     * topic tells HA to remove the entity; publishing empty retained
     * to the state topic frees the slot for the next gc cycle. We
     * iterate even on every connect — cost is two MQTT publishes per
     * deprecated key per reconnect and the retained store stays clean
     * if a broker dump+restore reintroduces the orphans. */
    static const struct {
        const char *comp;   /* "switch" / "number" / "select" — must match the original */
        const char *key;    /* the old (now-removed) NVS key name */
    } DEPRECATED_DISC[] = {
        { "switch", "capture_led_enabled" },  /* → cap_led_en */
        { "number", "reed_debounce_ms"    },  /* → reed_db_ms */
        { "number", "spkr_sink"           },  /* → spkr_tone (int knob → bool switch) */
        { "switch", "pcm_enabled"         },  /* → pcm pin function (pin_d?_fn=pcm) */
        /* Burned-in still watermark — removed in 258bd56 (OV3660 has no
         * on-chip OSD; software decode→re-encode too slow — see jpeg_stamp.h).
         * The `891d31c-dirty` build published this switch; its retained
         * discovery config outlived the feature, leaving a dead "Burn
         * caption into still captures" toggle in HA. */
        { "switch", "cam_watermark"       },  /* feature removed, no replacement */
        /* Old LED pin knobs replaced by the pin function map (pin_dN_fn).
         * They went through a number→select migration first, so we
         * have to clear BOTH discovery flavours: the older `number`
         * topic was emptied during the select migration but the
         * `select` topic itself now needs the same treatment. */
        { "number", "ir_led_pin"          },  /* → pin_d?_fn=ir_led */
        { "select", "ir_led_pin"          },
        { "number", "capture_led_pin"     },  /* → pin_d?_fn=capture_led */
        { "select", "capture_led_pin"     },
    };
    for (size_t i = 0; i < sizeof(DEPRECATED_DISC) / sizeof(DEPRECATED_DISC[0]); i++) {
        snprintf(topic, sizeof(topic),
                 "homeassistant/%s/%s/cfg_%s/config",
                 DEPRECATED_DISC[i].comp, id, DEPRECATED_DISC[i].key);
        mqtt_pub_discovery_raw(topic, "", 0);
        snprintf(topic, sizeof(topic), "%s/state/cfg/%s",
                 base, DEPRECATED_DISC[i].key);
        mqtt_pub_retained(topic, "");
    }

    /* Action buttons — no schema entry, just discovery + cmd handling.
     * All four targets are already subscribed in mqtt.c
     * (cmd_reboot / cmd_photo / cmd_snapshot / cmd_ota) — discovery
     * only exposes them as HA button entities so the operator gets
     * one-click triggers in the dashboard.
     *
     * `photo` is the only "primary" action (no ent_cat) — the others
     * are diagnostic/config so they cluster under the device's
     * Diagnostic section instead of cluttering the main controls. */
    static const struct {
        const char *obj;
        const char *name;
        const char *cmd_suffix;
        const char *icon;
        const char *ent_cat;  /* NULL = no entity_category (primary control) */
    } ACTION_BUTTONS[] = {
        { "reboot",   "Reboot",            "reboot",   "mdi:restart",      "config"     },
        { "photo",    "Vyfotit",           "photo",    "mdi:camera",       NULL         },
        { "snapshot", "Aktualizovat stav", "snapshot", "mdi:database-sync","diagnostic" },
        { "ota",      "Zkontrolovat OTA",  "ota",      "mdi:cloud-download","diagnostic"},
    };
    for (size_t i = 0; i < sizeof(ACTION_BUTTONS) / sizeof(ACTION_BUTTONS[0]); i++) {
        const char *ec = ACTION_BUTTONS[i].ent_cat;
        snprintf(topic, sizeof(topic),
                 "homeassistant/button/%s/%s/config", id, ACTION_BUTTONS[i].obj);
        snprintf(payload, sizeof(payload),
            "{\"name\":\"%s\","
            "\"uniq_id\":\"%s_%s\","
            "\"cmd_t\":\"%s/cmd/%s\","
            "\"icon\":\"%s\","
            "\"avty_t\":\"%s\","
            "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
            "%s%s%s%s}",
            ACTION_BUTTONS[i].name,
            id, ACTION_BUTTONS[i].obj,
            base, ACTION_BUTTONS[i].cmd_suffix,
            ACTION_BUTTONS[i].icon,
            avail,
            devblk,
            ec ? ",\"ent_cat\":\"" : "",
            ec ? ec : "",
            ec ? "\"" : "");
        mqtt_pub_discovery_raw(topic, payload, (int)strlen(payload));
    }

    /* OTA status sensor — ota.c publishes one of
     * "checking"/"up-to-date"/"downloading"/"done"/"error" as retained
     * to <base>/state/ota. Operator visibility: shows whether the last
     * OTA round succeeded without tailing the log. */
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s/ota_status/config", id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"OTA\","
        "\"uniq_id\":\"%s_ota_status\","
        "\"stat_t\":\"%s/state/ota\","
        "\"icon\":\"mdi:cloud-sync\","
        "\"avty_t\":\"%s\","
        "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
        "%s,"
        "\"ent_cat\":\"diagnostic\"}",
        id, base, avail, devblk);
    mqtt_pub_discovery_raw(topic, payload, (int)strlen(payload));
}
