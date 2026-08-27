/* pir.c — AM312 PIR motion sensor driver.
 *
 * Polling sample-confirm model (same shape as reed.c). A previous
 * implementation used GPIO ISR + per-edge software debounce + a
 * periodic re-probe task to recover from AM312 warm-up. That worked
 * for clean field harnesses but blew up on the bench:
 *
 *   Bench backtrace fingered `pir_isr` reached from `_xt_lowint1`
 *   while `ppTask` was inside `wifi_malloc(1700)` → multi_heap's
 *   `vPortEnterCritical`. Long enough heap-critical sections + a
 *   floating-ish PIR line generating phantom edges = `int_wdt`
 *   on the next ISR entry. Disabling PIR via NVS stopped the
 *   crash loop instantly, confirming the path.
 *
 * Polling sidesteps the whole ISR-storm risk. We sample the line
 * every PIR_POLL_PERIOD_MS, accept a LOW→HIGH transition only when
 * PIR_DEBOUNCE_SAMPLES consecutive samples disagree with the cached
 * state, and bump the motion counter on each confirmed edge.
 * Response latency is roughly POLL_PERIOD * DEBOUNCE_SAMPLES
 * (~40 ms with defaults) — well below the AM312's 2 s hardware
 * blanking, so we never miss a real trigger.
 *
 * Multi-instance: the polling task iterates an array of up to
 * PIR_MAX_INSTANCES PIRs discovered from the pin function map at
 * boot. Each has its own debounced state + counter + pending flag
 * + stuck-high tracking. Instance 0 keeps the singleton-shaped MQTT
 * topic (state/motion, state/motion_count) and "Motion" HA entity
 * for backward compat; instances 1+ get `_N` suffixes.
 *
 * Deep-sleep wake is exposed via `pir_arm_deep_sleep_wakeup()`
 * (RTC GPIO EXT0). Currently arms instance 0 only — multi-PIR wake
 * isn't a thing today (deep sleep itself isn't wired in yet). */
#include "pir.h"

#include <stdatomic.h>

#include "app_config.h"
#include "app_main_exports.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"   /* RTC-IO path for light-sleep wake + read (EXT1) */
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cb_time.h"
#include "mqtt.h"

static const char *TAG = "pir";

#define PIR_POLL_PERIOD_MS   20
/* In deep-save (Safe + light sleep) poll slowly so the 20 ms cadence doesn't
 * wake CPU0 out of light sleep 50×/s. ~1 s motion latency (2 samples) is fine in
 * a survival posture; the PIR holds HIGH for ~2 s so an event isn't missed. */
#define PIR_POLL_SAVE_MS     500
#define PIR_DEBOUNCE_SAMPLES 2     /* 2 × 20 ms = 40 ms — fast vs AM312's 2 s hold */

/* Once we observe this many *real* (debounced) motion events in a
 * single boot, promote to "sensor likely present" so selftest stops
 * masking us behind "pir:false". A handful filters the corner case
 * where the AM312 fires a single warmup glitch through the debounce
 * window without committing to "sensor present" forever. */
#define PIR_PROMOTE_EDGE_THRESHOLD 3

/* AM312 datasheet caps the output-HIGH hold time at 2 s. Anything past
 * 5 min is unambiguously the sensor's output transistor failing
 * (saturated stuck-on) — well beyond any plausible motion duration or
 * EMI artifact. */
#define PIR_STUCK_HIGH_THRESHOLD_US (5LL * 60 * 1000 * 1000)

typedef struct {
    int                       pin;
    bool                      rtc;            /* pin driven as RTC-IO (EXT1 wake + rtc_gpio read) */
    atomic_bool               motion_pending;
    atomic_bool               likely_present;
    atomic_uint_fast32_t      motion_count;
    atomic_uint_fast32_t      last_motion_ms;
    atomic_uint_fast64_t      high_at_us;     /* 0 = line currently LOW */
    /* poll-local state */
    bool                      current;
    int                       differ;
} pir_instance_t;

static pir_instance_t s_inst[PIR_MAX_INSTANCES];
static atomic_int     s_active_count = ATOMIC_VAR_INIT(0);
static atomic_bool    s_ready        = ATOMIC_VAR_INIT(false);

/* DIAGNOSTIC counters (instance 0): how many times the poll task actually ran,
 * and how many of those reads saw the line HIGH. Read via /debug/pir after an
 * UNPOLLED motion window in Safe+light-sleep to separate the failure modes:
 *   poll_iters barely advancing → the poll task is starved (tickless idle isn't
 *     scheduling it) — a sleep/scheduling problem.
 *   poll_iters advances but high_reads stays 0 → the poll runs but never sees a
 *     HIGH — sensor not asserting / pin genuinely LOW.
 *   high_reads advances but motion_count doesn't → debounce/logic. */
static atomic_uint_fast32_t s_poll_iters = 0;
static atomic_uint_fast32_t s_high_reads = 0;
/* Count of poll iterations whose preceding sleep was woken by EXT1 (the RTC
 * controller sensing the PIR pad HIGH). >0 in Safe+LS motion ⇒ the pad DOES go
 * HIGH (HW wake fires) and the bug is in the read path; 0 ⇒ the pad never goes
 * HIGH. Purely electrical — needs no external load on the pin. */
static atomic_uint_fast32_t s_ext1_wakes = 0;

/* RTC EXT1 wake mask armed at init (0 if no RTC-capable PIR pin). Exposed via
 * pir_ds_wake_armed() so the hibernate path can verify motion-wake is live. */
static uint64_t s_ext1_mask = 0;

bool pir_ds_wake_armed(void) { return s_ext1_mask != 0; }

uint64_t pir_rtc_pin_mask(void) { return s_ext1_mask; }

static void pir_task(void *arg) {
    (void)arg;
    int n_active = atomic_load(&s_active_count);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(app_profile_sleeps() ? PIR_POLL_SAVE_MS
                                                       : PIR_POLL_PERIOD_MS));
        atomic_fetch_add(&s_poll_iters, 1);
        if (esp_sleep_get_wakeup_causes() & (1ULL << ESP_SLEEP_WAKEUP_EXT1))
            atomic_fetch_add(&s_ext1_wakes, 1);
        for (int i = 0; i < n_active; i++) {
            pir_instance_t *p = &s_inst[i];
            /* RTC-IO pins read via the RTC mux (alive in light-sleep); digital
             * fallback uses the normal path. Un-hold first: automatic light-sleep
             * may latch the pad to its pre-sleep (idle LOW) level — which would make
             * BOTH this read AND the EXT1 wake see a stale LOW even while the AM312
             * drives HIGH. hold_dis releases the latch so we read the LIVE pad. */
            if (p->rtc) rtc_gpio_hold_dis((gpio_num_t)p->pin);
            bool sample = ((p->rtc ? rtc_gpio_get_level((gpio_num_t)p->pin)
                                   : (uint32_t)gpio_get_level((gpio_num_t)p->pin)) == 1);
            if (i == 0 && sample) atomic_fetch_add(&s_high_reads, 1);
            if (sample == p->current) {
                p->differ = 0;
                continue;
            }
            if (++p->differ < PIR_DEBOUNCE_SAMPLES)
                continue;
            p->current = sample;
            p->differ  = 0;
            if (sample) {
                int64_t now_us = esp_timer_get_time();
                atomic_store(&p->high_at_us, (uint64_t)now_us);
                atomic_store(&p->last_motion_ms, (uint32_t)(now_us / 1000));
                uint32_t count = atomic_fetch_add(&p->motion_count, 1) + 1;
                atomic_store(&p->motion_pending, true);
                if (count >= PIR_PROMOTE_EDGE_THRESHOLD) {
                    atomic_store(&p->likely_present, true);
                }
            } else {
                atomic_store(&p->high_at_us, 0);
            }
        }
    }
}

/* One-time probe per pin — a pull-up + pull-down sweep tells us how the line
 * behaves when only the internal pulls act on it. THREE outcomes are actually
 * distinguishable, and collapsing them into "floating vs driven" is what made
 * a bare board with nothing wired to the pin report "sensor present":
 *
 *   pull-up=1, pull-dn=0   line follows whichever pull is active — nothing is
 *                          driving it → no sensor.
 *   pull-up=0, pull-dn=0   something SINKS the line against the internal
 *                          pull-up. An idle AM312 does exactly this (its idle
 *                          state is LOW — see WIRING.md) → sensor present.
 *   pull-up=1, pull-dn=1   something HOLDS the line high against the internal
 *                          pull-down. That is either a PIR asserting motion or
 *                          still in warm-up, OR board-level leakage on an
 *                          unwired pad. Those two are electrically identical at
 *                          this instant, so we must NOT claim a sensor.
 *
 * Only the confident LOW case seeds likely_present. The ambiguous HIGH case
 * stays unconfirmed and is resolved at runtime by PIR_PROMOTE_EDGE_THRESHOLD
 * real debounced edges — a wired sensor produces them, a leaky bare pad does
 * not. That costs a wired-but-currently-asserting sensor a "pir:false" for the
 * first few events, which is the right trade: an honest "not confirmed" beats a
 * green light that a mis-wire cannot turn red. */
typedef enum {
    PIR_LINE_FLOATING,    /* follows the internal pull — nothing attached */
    PIR_LINE_DRIVEN_LOW,  /* actively sunk — an idle AM312 */
    PIR_LINE_HELD_HIGH,   /* held high — asserting sensor OR unwired leakage */
} pir_line_t;

static const char *pir_line_str(pir_line_t st) {
    switch (st) {
        case PIR_LINE_DRIVEN_LOW:
            return "driven LOW (sensor present)";
        case PIR_LINE_HELD_HIGH:
            return "held HIGH (UNCONFIRMED — an asserting/warming sensor, or nothing wired)";
        default:
            return "floating (no sensor)";
    }
}

static pir_line_t pir_probe_line(int pin) {
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    cb_delay_ms(5);
    int level_up = gpio_get_level((gpio_num_t)pin);
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLDOWN_ONLY);
    cb_delay_ms(5);
    int level_dn = gpio_get_level((gpio_num_t)pin);

    pir_line_t st;
    if (level_up == 1 && level_dn == 0)
        st = PIR_LINE_FLOATING;
    else if (level_up == 0 && level_dn == 0)
        st = PIR_LINE_DRIVEN_LOW;
    else
        /* (0,1) is an inverted follow — electrically nonsensical. Lump it in
         * with the ambiguous case rather than invent a fourth verdict. */
        st = PIR_LINE_HELD_HIGH;

    ESP_LOGI(TAG, "probe GPIO%d: pull-up=%d pull-dn=%d → %s",
             pin, level_up, level_dn, pir_line_str(st));
    return st;
}

esp_err_t pir_init(void) {
    if (atomic_load(&s_ready)) return ESP_OK;

    int pins[PIR_MAX_INSTANCES];
    size_t n_pins = app_config_pins_for("pir", pins, PIR_MAX_INSTANCES);
    if (n_pins == 0) {
        ESP_LOGI(TAG, "no GPIO mapped to 'pir' in pin map — module idle");
        return ESP_OK;
    }

    for (size_t i = 0; i < n_pins; i++) {
        s_inst[i].pin = pins[i];
        s_inst[i].current = false;
        s_inst[i].differ = 0;
        atomic_store(&s_inst[i].motion_pending, false);
        atomic_store(&s_inst[i].motion_count, 0);
        atomic_store(&s_inst[i].last_motion_ms, 0);
        atomic_store(&s_inst[i].high_at_us, 0);

        pir_line_t line = pir_probe_line(pins[i]);
        /* ONLY a confidently-sunk line means "sensor" — see pir_probe_line. */
        atomic_store(&s_inst[i].likely_present, line == PIR_LINE_DRIVEN_LOW);

        /* RTC-IO path (the one that actually survives S3 light-sleep). The digital
         * GPIO input reads a dead 0 the moment automatic light-sleep is on — even
         * during an awake read — because the ESD-reset errata workaround
         * (ESP_SLEEP_GPIO_RESET_WORKAROUND → PM_SLP_DISABLE_GPIO, force-select'd,
         * not disableable) isolates the digital IO in sleep, and gpio_wakeup /
         * gpio_sleep_set_* did NOT fix it (measured 0/20 HIGH while moving). So drive
         * the pin through the RTC mux instead: read via rtc_gpio_get_level and wake
         * via the RTC controller (EXT1, armed after the loop) — both in the always-on
         * RTC domain, untouched by the digital-sleep isolate. Falls back to digital if
         * the pin isn't RTC-capable (then it just won't detect in light-sleep). */
        bool rtc = rtc_gpio_is_valid_gpio((gpio_num_t)pins[i]);
        if (rtc) {
            esp_err_t e1 = rtc_gpio_init((gpio_num_t)pins[i]);
            esp_err_t e2 = rtc_gpio_set_direction((gpio_num_t)pins[i], RTC_GPIO_MODE_INPUT_ONLY);
            /* PULL-UP, not pull-down: the AM312's motion HIGH is weak/marginal and a
             * pull-down opposes it (proven — a clean rail-to-rail button on the same
             * pin path detects fine in Safe+LS, the weak AM312 doesn't). The boot
             * probe shows the AM312 actively SINKS when idle (held LOW against an
             * internal pull-up), so a pull-up keeps idle LOW yet pulls the weak motion
             * HIGH up toward 3.3 V so it clears the input/EXT1 threshold from deep
             * sleep. Trade-off: if the sensor is unpowered/floating the pull-up reads
             * HIGH (could nuisance-wake) — acceptable for a driven AM312. */
            rtc_gpio_pulldown_dis((gpio_num_t)pins[i]);
            esp_err_t e3 = rtc_gpio_pullup_en((gpio_num_t)pins[i]);
            if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) {
                ESP_LOGW(TAG, "rtc_gpio setup GPIO%d (%s/%s/%s) — digital fallback (no LS detect)",
                         pins[i], esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3));
                rtc_gpio_deinit((gpio_num_t)pins[i]);
                rtc = false;
            }
        }
        s_inst[i].rtc = rtc;
        if (!rtc) {
            gpio_config_t cfg = {
                .pin_bit_mask = 1ULL << pins[i],
                .mode         = GPIO_MODE_INPUT,
                .pull_up_en   = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_ENABLE,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            esp_err_t err = gpio_config(&cfg);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "gpio_config GPIO%d: %s — instance %u disabled",
                         pins[i], esp_err_to_name(err), (unsigned)i);
                continue;
            }
        }
        /* Seed the debounced state from the line as it actually reads under the
         * FINAL pull configuration. Hard-coding `false` here manufactured a
         * LOW→HIGH transition on the first poll of any line that was already
         * HIGH: on a bare board that fired a phantom motion event — and, with
         * ds_pir_photo on, a photo — within seconds of every boot, and started
         * the stuck-high timer that later surfaced as pir_wedged. */
        bool initial = ((s_inst[i].rtc ? rtc_gpio_get_level((gpio_num_t)pins[i])
                                       : (uint32_t)gpio_get_level((gpio_num_t)pins[i])) == 1);
        s_inst[i].current = initial;
        /* Keep the documented invariant (high_at_us == 0 ⟺ line is LOW) true
         * from boot, so stuck-high tracking measures from the right instant. */
        atomic_store(&s_inst[i].high_at_us,
                     initial ? (uint_fast64_t)esp_timer_get_time() : 0);

        ESP_LOGI(TAG, "pir[%u] armed on GPIO%d (%s, poll %d/%d ms × %d) — line: %s, initial: %s",
                 (unsigned)i, pins[i], rtc ? "RTC-IO + EXT1 wake" : "digital",
                 PIR_POLL_PERIOD_MS, PIR_POLL_SAVE_MS, PIR_DEBOUNCE_SAMPLES,
                 pir_line_str(line), initial ? "HIGH" : "LOW");
    }
    atomic_store(&s_active_count, (int)n_pins);

    /* Arm EXT1 RTC wake from the RTC-capable PIR pins. EXT1 is the RTC-controller
     * wake source that works from automatic light-sleep (and deep sleep) through the
     * always-on RTC domain — the path that survives the S3 digital-GPIO light-sleep
     * isolate. Keep RTC_PERIPH powered so the RTC input + pull-down stay alive across
     * sleep (a few µA). ANY_HIGH: AM312 idles LOW (pull-down), pulses HIGH ~2 s on
     * motion → wakes the SoC; while the line stays HIGH the chip can't re-sleep, so
     * the ~500 ms Safe poll (rtc_gpio_get_level) reliably catches it + debounces. */
    uint64_t ext1_mask = 0;
    for (size_t i = 0; i < n_pins; i++)
        if (s_inst[i].rtc) ext1_mask |= (1ULL << s_inst[i].pin);
    s_ext1_mask = ext1_mask;  /* publish for pir_ds_wake_armed() */
    if (ext1_mask) {
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        esp_err_t we = esp_sleep_enable_ext1_wakeup_io(ext1_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
        if (we != ESP_OK)
            ESP_LOGW(TAG, "esp_sleep_enable_ext1_wakeup_io(0x%llx): %s — PIR won't wake light-sleep",
                     (unsigned long long)ext1_mask, esp_err_to_name(we));
        else
            ESP_LOGI(TAG, "PIR EXT1 light-sleep wake armed (mask 0x%llx, ANY_HIGH)",
                     (unsigned long long)ext1_mask);
    }

    BaseType_t ok = xTaskCreatePinnedToCore(pir_task, "pir", 2560, NULL,
                                            tskIDLE_PRIORITY + 1, NULL,
                                            /*core*/ 0);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_ready, true);
    return ESP_OK;
}

bool pir_ready(void) {
    if (!atomic_load(&s_ready)) return false;
    /* Existing semantics: pir_ready() is "at least one instance is
     * likely_present". Backward compat for selftest etc. */
    int n = atomic_load(&s_active_count);
    for (int i = 0; i < n; i++) {
        if (atomic_load(&s_inst[i].likely_present)) return true;
    }
    return false;
}

int pir_active_count(void) { return atomic_load(&s_active_count); }

/* Singleton wrappers (instance 0). */
bool pir_motion_consume(void)     { return pir_motion_consume_nth(0); }
uint32_t pir_motion_count(void)   { return pir_motion_count_nth(0); }
uint32_t pir_last_motion_ms(void) { return pir_last_motion_ms_nth(0); }

bool pir_motion_consume_nth(int idx) {
    if (idx < 0 || idx >= PIR_MAX_INSTANCES) return false;
    if (idx >= atomic_load(&s_active_count)) return false;
    return atomic_exchange(&s_inst[idx].motion_pending, false);
}
uint32_t pir_motion_count_nth(int idx) {
    if (idx < 0 || idx >= PIR_MAX_INSTANCES) return 0;
    if (idx >= atomic_load(&s_active_count)) return 0;
    return (uint32_t)atomic_load(&s_inst[idx].motion_count);
}
uint32_t pir_last_motion_ms_nth(int idx) {
    if (idx < 0 || idx >= PIR_MAX_INSTANCES) return 0;
    if (idx >= atomic_load(&s_active_count)) return 0;
    return (uint32_t)atomic_load(&s_inst[idx].last_motion_ms);
}

/* Raw line level read the same way the poll does (RTC mux for RTC-IO pins, else
 * digital) — so /debug/pir shows the true level even in Safe+light-sleep. -1 if
 * the instance isn't active. */
int pir_raw_level_nth(int idx) {
    if (idx < 0 || idx >= PIR_MAX_INSTANCES) return -1;
    if (idx >= atomic_load(&s_active_count)) return -1;
    pir_instance_t *p = &s_inst[idx];
    return p->rtc ? (int)rtc_gpio_get_level((gpio_num_t)p->pin)
                  : gpio_get_level((gpio_num_t)p->pin);
}

void pir_debug_poll_stats(uint32_t *poll_iters, uint32_t *high_reads, uint32_t *ext1_wakes) {
    if (poll_iters) *poll_iters = (uint32_t)atomic_load(&s_poll_iters);
    if (high_reads) *high_reads = (uint32_t)atomic_load(&s_high_reads);
    if (ext1_wakes) *ext1_wakes = (uint32_t)atomic_load(&s_ext1_wakes);
}

bool pir_wedged(void) {
    if (!atomic_load(&s_ready)) return false;
    /* Wedge = ANY instance has been HIGH > threshold. Selftest surfaces
     * this so a stuck sensor on a multi-PIR board is visible without
     * needing per-instance wedge reporting (rare enough not to worth
     * the entity sprawl right now). */
    int n = atomic_load(&s_active_count);
    int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
        /* Only a line we actually believe carries a sensor can be "wedged".
         * An unwired pad that sits permanently HIGH is not a stuck sensor, and
         * reporting it as one sends an operator hunting a hardware fault on a
         * board that has no PIR attached at all. */
        if (!atomic_load(&s_inst[i].likely_present)) continue;
        uint64_t high_at = atomic_load(&s_inst[i].high_at_us);
        if (high_at == 0) continue;
        if (now_us - (int64_t)high_at > PIR_STUCK_HIGH_THRESHOLD_US) {
            return true;
        }
    }
    return false;
}

esp_err_t pir_arm_deep_sleep_wakeup(void) {
    /* Arms instance 0's pin only. Multi-PIR deep-sleep wake would need
     * EXT1 (any-of bitmap) instead of EXT0 (single pin) — deferred
     * until deep sleep is actually wired into the mode FSM. */
    int n = atomic_load(&s_active_count);
    if (n == 0) return ESP_ERR_INVALID_STATE;
    return esp_sleep_enable_ext0_wakeup((gpio_num_t)s_inst[0].pin, 1);
}

void pir_apply_config(void) {
    /* main.cpp owns the consume gate (pir_motion_consume() &&
     * pir_enabled) so toggling is effectively instant from a
     * functional standpoint. The visible lag is residual
     * MOTION_HOLD_MS (10 s) holding s_motion_active from the last
     * fire just before the toggle. Force a motion=false publish here
     * so HA reflects the new state right away; main.cpp's timer-
     * driven false publish later is harmless retain=1 idempotency. */
    if (!app_config_get_bool("pir_enabled")) {
        ESP_LOGI(TAG, "pir_enabled flipped OFF live — flushing motion=false");
        mqtt_publish_motion(false);
        /* Refresh count for instance 0 only (singleton telemetry). */
        mqtt_publish_motion_count(pir_motion_count_nth(0));
    }
}
