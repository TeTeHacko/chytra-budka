/* reed.c — magnetic reed switch on REED_PIN. See reed.h. */

#include "reed.h"

#include <inttypes.h>
#include <stdatomic.h>

#include "app_config.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt.h"

static const char *TAG = "reed";

/* Sample-and-confirm debounce.
 *
 * The original per-edge lockout (30 ms after any ISR edge) only filtered
 * the mechanical bounce of the reed contact itself (~hundreds of µs).
 * It did nothing about the line being driven by a weak internal pull-up
 * (~45 kΩ) over a long wire harness — EMI and capacitive coupling can
 * pull the line LOW for tens of ms at irregular intervals, generating
 * spurious ANYEDGE interrupts spaced >30 ms apart. Every one passed
 * the lockout and got counted as a transition.
 *
 * Approach now: drop the GPIO ISR entirely (not a wake source — deep-
 * sleep will use RTC GPIO directly when that ships). Poll the line
 * every REED_POLL_PERIOD_MS and accept a state change only after N
 * consecutive samples disagree with the current debounced state. N is
 * derived live from app_config_get_int("reed_db_ms") on every
 * poll, so the operator can tune per-install without rebooting —
 * different houses have different harness lengths, magnet quality,
 * and EMI floors.
 *
 * Multi-instance: the polling task iterates an array of up to
 * REED_MAX_INSTANCES reeds discovered from the pin function map at
 * boot. Each instance has its own debounced state + counter +
 * event-pending flag; the main loop consumes them by index. Instance
 * 0 keeps the singleton-shaped MQTT topics (state/reed,
 * state/reed_count) and "Door / Lid" HA entity so existing dashboards
 * survive an upgrade unchanged. Instances 1..N publish to
 * state/reed_<n> and discover as "Door / Lid <n>". */
#define REED_POLL_PERIOD_MS    20
#define REED_DEBOUNCE_MS_MIN   REED_POLL_PERIOD_MS   /* clamp; 1 sample */

typedef struct {
    int                       pin;
    atomic_bool               closed;
    atomic_bool               event_pending;
    atomic_uint_fast32_t      event_count;
    /* poll-local debounce state (only touched from reed_task) */
    bool                      current;
    int                       differ;
} reed_instance_t;

static reed_instance_t s_inst[REED_MAX_INSTANCES];
static atomic_int      s_active_count = ATOMIC_VAR_INIT(0);
static atomic_bool     s_ready        = ATOMIC_VAR_INIT(false);
/* Set by reed_apply_config(OFF) to ask the poll task to exit cleanly
 * on the next iteration. The task self-deletes; reed_apply_config does
 * not vTaskDelete it from outside (that bypasses the in-task cleanup
 * window and could leave instance state inconsistent with the GPIOs
 * if a sample was mid-confirmation). */
static atomic_bool     s_should_stop  = ATOMIC_VAR_INIT(false);

static void reed_task(void *arg) {
    (void)arg;
    int n_active = atomic_load(&s_active_count);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(REED_POLL_PERIOD_MS));
        /* Runtime tear-down: reed_apply_config(OFF) raises s_should_stop;
         * we drain it here so the task always exits between samples
         * (never mid-confirmation). s_ready falls in the same fence. */
        if (atomic_load(&s_should_stop)) {
            atomic_store(&s_ready, false);
            atomic_store(&s_should_stop, false);
            atomic_store(&s_active_count, 0);
            ESP_LOGI(TAG, "task exiting (reed_enabled=OFF)");
            vTaskDelete(NULL);
            return;
        }
        /* Live-tunable debounce (same value applied to all instances).
         * Different reeds on the same board are likely wired with
         * similar harnesses + magnets, so a single knob is fine for
         * v0; per-instance debounce can be a future per-pin config. */
        int32_t debounce_ms = app_config_get_int("reed_db_ms");
        if (debounce_ms < REED_DEBOUNCE_MS_MIN) debounce_ms = REED_DEBOUNCE_MS_MIN;
        int samples_needed = debounce_ms / REED_POLL_PERIOD_MS;
        if (samples_needed < 1) samples_needed = 1;

        for (int i = 0; i < n_active; i++) {
            reed_instance_t *r = &s_inst[i];
            /* Active-low contact: line LOW = switch closed (magnet present). */
            bool sample = (gpio_get_level((gpio_num_t)r->pin) == 0);
            if (sample == r->current) {
                r->differ = 0;
                continue;
            }
            if (++r->differ < samples_needed)
                continue;
            r->current = sample;
            atomic_store(&r->closed, sample);
            atomic_fetch_add(&r->event_count, 1);
            atomic_store(&r->event_pending, true);
            r->differ = 0;
        }
    }
}

/* Arm GPIOs + spawn the (single) polling task. Idempotent. Caller has
 * already checked the reed_enabled gate. */
static esp_err_t reed_start(void) {
    if (atomic_load(&s_ready))
        return ESP_OK;

    /* Pin map lookup. Up to REED_MAX_INSTANCES GPIOs may carry the
     * "reed" function. Operator can have 0 (module idle), 1 (default
     * rev3.2 layout, D0/GPIO1), or more (custom variants). */
    int pins[REED_MAX_INSTANCES];
    size_t n_pins = app_config_pins_for("reed", pins, REED_MAX_INSTANCES);
    if (n_pins == 0) {
        ESP_LOGI(TAG, "no GPIO mapped to 'reed' in pin map — module idle");
        return ESP_OK;
    }

    for (size_t i = 0; i < n_pins; i++) {
        s_inst[i].pin = pins[i];
        s_inst[i].current = false;
        s_inst[i].differ = 0;
        atomic_store(&s_inst[i].closed, false);
        atomic_store(&s_inst[i].event_pending, false);
        atomic_store(&s_inst[i].event_count, 0);

        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gpio_config GPIO%d: %s — instance %u disabled",
                     pins[i], esp_err_to_name(err), (unsigned)i);
            continue;
        }
        /* Seed initial debounced state so reed_is_closed_nth() returns
         * something meaningful before the first poll lands. */
        bool initial_closed = (gpio_get_level((gpio_num_t)pins[i]) == 0);
        s_inst[i].current = initial_closed;
        atomic_store(&s_inst[i].closed, initial_closed);
    }
    atomic_store(&s_active_count, (int)n_pins);

    /* Pin to CPU0 — see pir.c for rationale (cheap poll task, don't
     * preempt audio_task on CPU1 with a 20 ms debounce tick). */
    BaseType_t ok = xTaskCreatePinnedToCore(reed_task, "reed", 2048, NULL,
                                            tskIDLE_PRIORITY + 1, NULL,
                                            /*core*/ 0);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&s_ready, true);
    int32_t debounce_ms = app_config_get_int("reed_db_ms");
    for (size_t i = 0; i < n_pins; i++) {
        ESP_LOGI(TAG, "reed[%u] armed on GPIO%d (poll %d ms, debounce %" PRId32
                      " ms, NVS-tunable) — initial: %s",
                 (unsigned)i, s_inst[i].pin, REED_POLL_PERIOD_MS, debounce_ms,
                 atomic_load(&s_inst[i].closed) ? "closed" : "open");
        /* Publish initial state + count per instance — same rationale
         * as the singleton-era code: HA stops showing "unknown" right
         * away instead of waiting for the next telemetry tick or the
         * next physical event. */
        mqtt_publish_reed_nth((int)i, atomic_load(&s_inst[i].closed));
        mqtt_publish_reed_count_nth((int)i, 0);
    }
    return ESP_OK;
}

esp_err_t reed_init(void) {
    if (atomic_load(&s_ready))
        return ESP_OK;

    if (!app_config_get_bool("reed_enabled")) {
        ESP_LOGI(TAG, "reed_enabled=OFF — skipping init (pins left in default state)");
        return ESP_OK;
    }
    return reed_start();
}

void reed_apply_config(void) {
    bool want_on = app_config_get_bool("reed_enabled");
    bool is_on   = atomic_load(&s_ready);
    if (want_on && !is_on) {
        ESP_LOGI(TAG, "reed_enabled flipped ON live — arming");
        (void)reed_start();
    } else if (!want_on && is_on) {
        ESP_LOGI(TAG, "reed_enabled flipped OFF live — requesting task exit");
        atomic_store(&s_should_stop, true);
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

bool reed_ready(void) { return atomic_load(&s_ready); }

int reed_active_count(void) { return atomic_load(&s_active_count); }

/* Singleton wrappers — index 0 is the historical reed. Existing
 * callers (main.cpp tick, selftest, telemetry) keep working unchanged. */
bool reed_is_closed(void) {
    return reed_is_closed_nth(0);
}
uint32_t reed_event_count(void) {
    return reed_event_count_nth(0);
}
bool reed_event_consume(void) {
    return reed_event_consume_nth(0);
}

/* Per-instance access — returns false / 0 for indices out of range or
 * not currently active. Callers iterate up to reed_active_count(). */
bool reed_is_closed_nth(int idx) {
    if (idx < 0 || idx >= REED_MAX_INSTANCES) return false;
    if (idx >= atomic_load(&s_active_count)) return false;
    return atomic_load(&s_inst[idx].closed);
}
uint32_t reed_event_count_nth(int idx) {
    if (idx < 0 || idx >= REED_MAX_INSTANCES) return 0;
    if (idx >= atomic_load(&s_active_count)) return 0;
    return (uint32_t)atomic_load(&s_inst[idx].event_count);
}
bool reed_event_consume_nth(int idx) {
    if (idx < 0 || idx >= REED_MAX_INSTANCES) return false;
    if (idx >= atomic_load(&s_active_count)) return false;
    return atomic_exchange(&s_inst[idx].event_pending, false);
}
