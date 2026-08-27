// cb_ds.c — hibernate (deep-sleep duty cycle). See cb_ds.h for the contract.

#include "cb_ds.h"
#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_main_exports.h"
#include "diag.h"
#include "mqtt.h"
#include "oled.h"
#include "ota.h"
#include "pir.h"
#include "reed.h"
#include "wifi_mgr.h"

static const char *TAG = "cb_ds";

/* ---- RTC slow-memory state (survives deep sleep; cleared only on power-on).
 * Own magic word — NOT diag.c's — so the OTA that introduces this module does
 * not bump diag's RTC_MAGIC and wipe the fleet crash counters. ---- */
#define DS_RTC_MAGIC 0xCBD50001u
static RTC_NOINIT_ATTR uint32_t s_ds_magic;
static RTC_NOINIT_ATTR uint32_t s_ds_wake_count;     // DS wakes since cold boot
static RTC_NOINIT_ATTR uint64_t s_ds_epoch_at_sleep; // best-known UNIX time pre-sleep (0=never)
static RTC_NOINIT_ATTR uint64_t s_ds_accum_sleep_s;  // Σ programmed sleep since last SNTP sync
static RTC_NOINIT_ATTR uint64_t s_ds_last_ota_epoch; // wall-clock of last OTA check (0=never)

/* ---- per-wake (RAM) state, reset every boot ---- */
typedef enum { DS_WAKE_COLD = 0, DS_WAKE_TIMER, DS_WAKE_PIR } ds_wake_t;
static ds_wake_t s_wake_reason = DS_WAKE_COLD;
static int64_t   s_window_start_us = 0;
static int64_t   s_last_motion_us = 0;   // 0 = no motion this window
static bool      s_published = false;    // a telemetry publish happened this window
static bool      s_ota_checked = false;  // OTA already checked this window
static bool      s_ble_warned = false;

static const char *wake_reason_str(void) {
    switch (s_wake_reason) {
        case DS_WAKE_TIMER: return "timer";
        case DS_WAKE_PIR:   return "pir";
        default:            return "cold";
    }
}

/* Best-effort wall clock: trust SNTP once synced, else reconstruct from the
 * pre-sleep epoch + accumulated programmed sleep. Used only for the coarse
 * "OTA due" decision — never for photo/EXIF timestamps. */
static uint64_t ds_now_epoch(void) {
    time_t now = time(NULL);
    if (now > CB_CLOCK_SYNCED_EPOCH) return (uint64_t)now;  // SNTP-synced
    return s_ds_epoch_at_sleep + s_ds_accum_sleep_s;
}

void ds_capture_wake(void) {
    /* Bitmask API (esp_sleep_get_wakeup_cause is deprecated in IDF v6). */
    uint64_t causes = esp_sleep_get_wakeup_causes();
    esp_reset_reason_t reset = esp_reset_reason();
    const uint64_t pir_bits = (1ULL << ESP_SLEEP_WAKEUP_EXT0) |
                              (1ULL << ESP_SLEEP_WAKEUP_EXT1) |
                              (1ULL << ESP_SLEEP_WAKEUP_GPIO);

    if (s_ds_magic != DS_RTC_MAGIC) {
        /* Cold boot / first run after this firmware: initialise RTC state. */
        s_ds_magic = DS_RTC_MAGIC;
        s_ds_wake_count = 0;
        s_ds_epoch_at_sleep = 0;
        s_ds_accum_sleep_s = 0;
        s_ds_last_ota_epoch = 0;
        s_wake_reason = DS_WAKE_COLD;
    } else if (reset == ESP_RST_DEEPSLEEP) {
        /* A genuine wake out of our deep sleep. Prefer PIR (the actionable
         * event) if both a motion edge and the timer coincided. */
        if (causes & pir_bits)
            s_wake_reason = DS_WAKE_PIR;
        else
            s_wake_reason = DS_WAKE_TIMER;  // timer or unknown DS cause
        s_ds_wake_count++;
        /* A completed sleep→wake cycle proves a non-crashing run. The 180 s
         * uptime crash-clear in main.cpp never elapses inside a short hibernate
         * window, so clear here. */
        diag_clear_crash_count();
    } else {
        /* Reached boot some other way while DS state exists (power-on, OTA
         * esp_restart, panic, …): treat as a cold window — full boot path. */
        s_wake_reason = DS_WAKE_COLD;
    }

    s_window_start_us = esp_timer_get_time();
    s_last_motion_us  = (s_wake_reason == DS_WAKE_PIR) ? s_window_start_us : 0;
    s_published   = false;
    s_ota_checked = false;
    s_ble_warned  = false;

    ESP_LOGI(TAG, "wake reason=%s wake_count=%" PRIu32 " (causes=0x%llx reset=%d)",
             wake_reason_str(), s_ds_wake_count, (unsigned long long)causes, (int)reset);
}

void ds_note_telemetry_published(void) { s_published = true; }

bool ds_heartbeat_pending(void) {
    return app_profile_is_hibernate() && !s_published;
}

int ds_seconds_to_sleep(void) {
    if (!app_profile_is_hibernate()) return -1;
    bool pir = (s_last_motion_us != 0);
    int64_t ref = pir ? s_last_motion_us : s_window_start_us;
    int budget_s = pir ? app_config_get_int("ds_pir_win_s")
                       : app_config_get_int("ds_wake_s");
    int elapsed = (int)((esp_timer_get_time() - ref) / 1000000);
    int rem = budget_s - elapsed;
    return rem < 0 ? 0 : rem;
}

void ds_note_activity(void) {
    if (app_profile_is_hibernate()) s_last_motion_us = esp_timer_get_time();
}

bool ds_pir_photo_allowed(void) {
    return !app_profile_is_hibernate() || app_config_get_bool("ds_pir_photo");
}

static bool ds_ota_due(void) {
    int every_h = app_config_get_int("ds_ota_every");
    if (every_h <= 0) return false;
    uint64_t every_s = (uint64_t)every_h * 3600ULL;
    if (s_ds_last_ota_epoch == 0) return true;  // never checked
    uint64_t now = ds_now_epoch();
    if (now > s_ds_last_ota_epoch && (now - s_ds_last_ota_epoch) >= every_s)
        return true;
    /* Clock untrustworthy / unchanged? Fall back to accumulated sleep time so a
     * long-offline unit still eventually checks. */
    return s_ds_accum_sleep_s >= every_s;
}

/* Drain the MQTT client outbox before cutting power, bounded by ds_settle_ms.
 * Exits early when the outbox empties or the link drops. */
static void ds_drain_mqtt(void) {
    int cap_ms = app_config_get_int("ds_settle_ms");
    int64_t deadline = esp_timer_get_time() + (int64_t)cap_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (!mqtt_is_connected() || mqtt_outbox_empty()) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // final TCP send slack
}

/* Returns the GPIO (≥0) of an enabled, mapped, RTC-capable trigger pin, else -1.
 * Polarity (active-HIGH vs active-LOW) is decided by the caller from the pin's
 * idle level — so a button wired either way works. */
static int ds_wake_pin(const char *fn, bool enabled) {
    if (!enabled) return -1;
    int p = app_config_pin_for_first(fn);
    if (p < 0) return -1;
    if (!rtc_gpio_is_valid_gpio((gpio_num_t)p)) {
        ESP_LOGW(TAG, "hibernate: %s on GPIO%d is not RTC-capable — can't wake "
                      "deep sleep (use D0..D5 = GPIO1..6)", fn, p);
        return -1;
    }
    return p;
}

static void ds_enter_deep_sleep(void) {
    int sleep_s = app_config_get_int("ds_sleep_s");

    ESP_LOGW(TAG, "hibernate: sleeping %d s (wake=%s, count=%" PRIu32 ")",
             sleep_s, wake_reason_str(), s_ds_wake_count);

    /* Decide wake sources up front (also feeds the retained marker). PIR only
     * when pir_enabled (fixes "PIR woke it even when off" — pir_init() arms EXT1
     * unconditionally, which we override below). reed when reed_enabled; the
     * button is a manual wake, armed whenever a button pin is mapped. */
    bool pir_on  = app_config_get_bool("pir_enabled");
    int reed_pin = ds_wake_pin("reed",   app_config_get_bool("reed_enabled"));
    int btn_pin  = ds_wake_pin("button", true);

    /* EXT1 fires on ONE polarity for the whole mask, so split candidates by
     * their CURRENT idle level: idle-LOW pins wake on HIGH (ANY_HIGH, e.g. the
     * PIR, which idles low + pulses high); idle-HIGH pins wake on LOW (ANY_LOW,
     * e.g. a button wired idle-3V3 → GND-on-press). PIR is always idle-low. */
    uint64_t hi = pir_on ? pir_rtc_pin_mask() : 0;   /* wake on HIGH */
    uint64_t lo = 0;                                 /* wake on LOW  */
    int cand[2] = { reed_pin, btn_pin };
    for (int i = 0; i < 2; i++) {
        if (cand[i] < 0) continue;
        if (gpio_get_level((gpio_num_t)cand[i]) == 0) hi |= (1ULL << cand[i]);
        else                                          lo |= (1ULL << cand[i]);
    }

    /* 1) static OLED frame the panel holds through the whole sleep. */
    oled_show_deepsleep(sleep_s);

    /* 2) retained sleeping marker so HA tells intentional sleep from a fault. */
    mqtt_publish_ds_state(true, sleep_s, s_ds_wake_count, wake_reason_str(),
                          /*reed_wake=*/reed_pin >= 0);

    /* 3) let the QoS-1 marker + telemetry drain before power is cut. */
    ds_drain_mqtt();

    /* 4) persist RTC state. epoch base refreshes only when SNTP is trustworthy;
     * accum tracks programmed sleep for the offline OTA-due fallback. */
    time_t now = time(NULL);
    if (now > CB_CLOCK_SYNCED_EPOCH) s_ds_epoch_at_sleep = (uint64_t)now;
    s_ds_accum_sleep_s += (uint64_t)sleep_s;

    /* 5) arm wake sources: timer (always) + EXT1 on the qualifying trigger pins.
     * Prefer the active-HIGH group (PIR's convention); fall back to active-LOW
     * only when no active-HIGH pin qualifies (the PIR-off manual-button case) —
     * the two polarities can't share one EXT1. */
    uint64_t mask;
    esp_sleep_ext1_wakeup_mode_t mode;
    if (hi) {
        mask = hi; mode = ESP_EXT1_WAKEUP_ANY_HIGH;
        if (lo)
            ESP_LOGW(TAG, "hibernate: active-LOW pins 0x%llx not armed (can't mix "
                          "with active-HIGH wake; disable PIR to use them)",
                     (unsigned long long)lo);
    } else {
        mask = lo; mode = ESP_EXT1_WAKEUP_ANY_LOW;
    }
    /* cb_ds is AUTHORITATIVE over EXT1. esp_sleep_enable_ext1_wakeup_io() is
     * ADDITIVE (IDF v5.3+/v6) — it ORs pins in, it does NOT replace. pir_init()
     * arms the PIR pin in EXT1 at boot regardless of pir_enabled, so unless we
     * clear it that residual mask lingers and keeps waking us with PIR OFF (seen
     * in the field-test: reason="pir", wake_count climbing while disabled, as
     * soon as any other pin — the button — also armed and took the mask!=0 path).
     * Wipe EXT1 unconditionally, THEN arm exactly our computed mask. */
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    if (mask) {
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        esp_err_t we = esp_sleep_enable_ext1_wakeup_io(mask, mode);
        if (we != ESP_OK)
            ESP_LOGW(TAG, "hibernate: ext1 wake arm failed: %s", esp_err_to_name(we));
    }
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_s * 1000000ULL);
    ESP_LOGI(TAG, "hibernate: timer %ds + ext1 mask 0x%llx %s (pir%c reed%c btn%c)",
             sleep_s, (unsigned long long)mask,
             mask ? (mode == ESP_EXT1_WAKEUP_ANY_HIGH ? "ANY_HIGH" : "ANY_LOW") : "off",
             pir_on && pir_rtc_pin_mask() ? '+' : '-',
             reed_pin >= 0 ? '+' : '-', btn_pin >= 0 ? '+' : '-');

    /* 6) go. Never returns; the next boot re-enters via ds_capture_wake(). */
    esp_deep_sleep_start();
}

void ds_maybe_sleep(void) {
    if (!app_profile_is_hibernate()) return;

    /* OTA-when-due: a short wake window won't hit the background ota_task's
     * minutes-long cadence, so check synchronously here (once per window). The
     * check reboots into PENDING_VERIFY on an update; otherwise it returns and
     * we proceed to sleep. */
    if (!s_ota_checked && ds_ota_due() && wifi_mgr_is_connected() &&
        mqtt_is_connected() && !ota_img_pending_verify()) {
        s_ota_checked = true;
        s_ds_last_ota_epoch = ds_now_epoch();
        s_ds_accum_sleep_s = 0;  // reset the offline accumulator on a real check
        ESP_LOGI(TAG, "hibernate: OTA check due — running synchronously");
        ota_check_now_blocking();  // may esp_restart()
    }

    /* Never sleep while a fresh image is unverified (would roll back next wake)
     * or an OTA is downloading. */
    if (ota_img_pending_verify() || ota_progress_pct() >= 0) return;

    /* BLE + deep sleep is the known RAM/scan hazard — refuse until BLE is off. */
    if (app_config_get_bool("ble_enabled")) {
        if (!s_ble_warned) {
            s_ble_warned = true;
            ESP_LOGW(TAG, "hibernate: ble_enabled=ON blocks deep sleep — set "
                          "ble_enabled=OFF to hibernate");
        }
        return;
    }

    /* Window budget: a PIR wake (or any motion this window) stays up until
     * ds_pir_win_s after the last edge; a timer wake until ds_wake_s. */
    int64_t now = esp_timer_get_time();
    bool pir = (s_last_motion_us != 0);
    int64_t ref = pir ? s_last_motion_us : s_window_start_us;
    int64_t budget_us =
        (int64_t)(pir ? app_config_get_int("ds_pir_win_s")
                      : app_config_get_int("ds_wake_s")) * 1000000LL;
    if ((now - ref) < budget_us) return;  // still inside the active window

    /* Make sure a heartbeat went out while the link is up (don't strand HA with
     * no data) — but never block forever offline: if MQTT isn't up, the budget
     * timeout above already authorised the sleep. */
    if (mqtt_is_connected() && !s_published) return;

    ds_enter_deep_sleep();
}
