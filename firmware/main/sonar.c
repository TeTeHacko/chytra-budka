/* sonar.c — Grove Ultrasonic Ranger on the "sonar" pin function. See sonar.h.
 *
 * The module is HC-SR04-like but trigger + echo share the single SIG
 * line: we drive a short HIGH pulse, release the line, and the module
 * answers by holding SIG HIGH for the acoustic round-trip time
 * (~58 µs per cm, spec range 3 cm..3.5 m, echo window caps at ~38 ms).
 *
 * Echo width is measured with a both-edges GPIO ISR stamping
 * esp_timer_get_time() — ISR latency is a few µs ≈ sub-mm error, and
 * unlike a busy-wait poll it can't be stretched by a preempting task.
 * The GPIO interrupt is enabled only for the echo window, so our own
 * trigger pulse can't self-stamp.
 *
 * One measurement = median of 3 pings 60 ms apart (spec wants ≥50 ms
 * between pings so a late echo of ping N can't be read as ping N+1).
 * Zero valid pings ⇒ the cached value is invalidated and the absence
 * is WARN-logged once on the transition — a dead/unplugged sensor
 * shows up in the log and HA stops getting fresh values, instead of a
 * silently frozen reading. */

#include "sonar.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt.h"
#include "oled.h"

static const char *TAG = "sonar";

#define SONAR_PINGS_PER_POLL   3
#define SONAR_INTER_PING_MS    60     /* ≥50 ms so echoes can't cross-talk */
#define SONAR_ECHO_TIMEOUT_MS  60     /* module gives up at ~38 ms itself */
#define SONAR_MIN_ECHO_US      100    /* <~1.7 cm — trigger ringing, drop */
#define SONAR_MAX_ECHO_US      25000  /* >~4.3 m — spec max is 3.5 m */
#define SONAR_POLL_S_MIN       1
/* Sampling boost while the OLED ENV page is on-screen (the operator is
 * looking at the readout) or while the proximity trigger is armed
 * (sonar_trig_cm > 0 — detection latency must not depend on the MQTT
 * cadence). MQTT keeps the sonar_poll_s cadence regardless — the boost
 * feeds the display/trigger, not the recorder. */
#define SONAR_BOOST_POLL_MS    500
/* Published for a "clear" reading (≥ sonar_clear_cm): far outside the
 * sensor's real 3.5 m range, so dashboards/automations can't mistake
 * it for a measurement. */
#define SONAR_CLEAR_SENTINEL_CM 999.0f

static atomic_bool  s_ready       = ATOMIC_VAR_INIT(false);
static atomic_bool  s_should_stop = ATOMIC_VAR_INIT(false);
static atomic_bool  s_have_cm     = ATOMIC_VAR_INIT(false);
static atomic_bool  s_is_clear    = ATOMIC_VAR_INIT(false);
static atomic_bool  s_trig_pending = ATOMIC_VAR_INIT(false);
static atomic_uint_fast32_t s_trig_count = ATOMIC_VAR_INIT(0);
/* Distance cache crosses tasks (poll task writes, telemetry/HTTP read):
 * float stored as its bit pattern in an atomic u32. */
static _Atomic uint32_t s_last_cm_bits;

static int                 s_pin = -1;
static SemaphoreHandle_t   s_echo_done;
static volatile int64_t    s_t_rise, s_t_fall;

static void IRAM_ATTR echo_isr(void *arg) {
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (gpio_get_level((gpio_num_t)s_pin)) {
        s_t_rise = now;
    } else {
        s_t_fall = now;
        BaseType_t hpw = pdFALSE;
        xSemaphoreGiveFromISR(s_echo_done, &hpw);
        if (hpw)
            portYIELD_FROM_ISR();
    }
}

/* One trigger→echo cycle. False on timeout or an out-of-spec pulse. */
static bool sonar_ping(float *out_cm) {
    gpio_num_t pin = (gpio_num_t)s_pin;

    /* Trigger pulse — interrupt stays disabled so our own edges don't
     * stamp the echo timestamps. */
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    esp_rom_delay_us(2);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(10);
    gpio_set_level(pin, 0);

    /* Release the line to the module and open the echo window. The
     * module raises SIG a few hundred µs later; the pulldown keeps an
     * absent sensor's floating line LOW ⇒ clean timeout, not noise. */
    s_t_rise = s_t_fall = 0;
    xSemaphoreTake(s_echo_done, 0);  /* drain any stale give */
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_intr_enable(pin);
    bool got = (xSemaphoreTake(s_echo_done,
                               pdMS_TO_TICKS(SONAR_ECHO_TIMEOUT_MS)) == pdTRUE);
    gpio_intr_disable(pin);
    if (!got || s_t_fall <= s_t_rise || s_t_rise == 0)
        return false;

    int64_t width_us = s_t_fall - s_t_rise;
    if (width_us < SONAR_MIN_ECHO_US || width_us > SONAR_MAX_ECHO_US)
        return false;
    *out_cm = (float)width_us / 58.0f;
    return true;
}

static void sonar_task(void *arg) {
    (void)arg;
    bool was_valid = false;
    bool near = false;   /* proximity-trigger state (task-local) */
    int64_t last_pub_us = 0;
    while (true) {
        /* Median of up to 3 pings. */
        float v[SONAR_PINGS_PER_POLL];
        int n = 0;
        for (int i = 0; i < SONAR_PINGS_PER_POLL; i++) {
            if (i > 0)
                vTaskDelay(pdMS_TO_TICKS(SONAR_INTER_PING_MS));
            float cm;
            if (sonar_ping(&cm))
                v[n++] = cm;
        }
        int32_t period_s = app_config_get_int("sonar_poll_s");
        if (period_s < SONAR_POLL_S_MIN)
            period_s = SONAR_POLL_S_MIN;
        int32_t trig_cm  = app_config_get_int("sonar_trig_cm");
        int32_t clear_cm = app_config_get_int("sonar_clear_cm");
        if (n > 0) {
            /* tiny insertion sort → median */
            for (int i = 1; i < n; i++)
                for (int j = i; j > 0 && v[j] < v[j - 1]; j--) {
                    float t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
                }
            float cm = v[n / 2];
            if (!was_valid)
                ESP_LOGI(TAG, "echo back — %.1f cm (%d/%d pings)", cm, n,
                         SONAR_PINGS_PER_POLL);
            was_valid = true;

            /* "Clear" split: with no real target the module answers with
             * a fixed-width artifact pulse (constant fake ~60–80 cm on
             * the bench) — indistinguishable from a real object by pulse
             * shape. The operator measures that artifact and sets
             * sonar_clear_cm just below it; readings at/above are then
             * reported as nothing-in-range instead of a distance. */
            bool clear = (clear_cm > 0 && cm >= (float)clear_cm);
            atomic_store(&s_is_clear, clear);
            if (!clear) {
                uint32_t bits;
                memcpy(&bits, &cm, sizeof(bits));
                atomic_store(&s_last_cm_bits, bits);
                atomic_store(&s_have_cm, true);
            } else {
                atomic_store(&s_have_cm, false);
            }

            /* Proximity photo trigger: far→near edge with hysteresis
             * (re-arms only after the target retreats past trig + 10 %,
             * min 10 cm — so a bird sitting AT the threshold can't
             * machine-gun captures at the 2 Hz sampling rate). A clear
             * reading counts as far. */
            if (trig_cm > 0) {
                float hyst = (float)trig_cm * 0.1f;
                if (hyst < 10.0f) hyst = 10.0f;
                if (clear || cm >= (float)trig_cm + hyst) {
                    near = false;
                } else if (!near && cm < (float)trig_cm) {
                    near = true;
                    atomic_fetch_add(&s_trig_count, 1);
                    atomic_store(&s_trig_pending, true);
                    ESP_LOGI(TAG, "proximity trigger: %.1f cm < %" PRId32
                                  " cm (count=%u)", cm, trig_cm,
                             (unsigned)atomic_load(&s_trig_count));
                }
            }

            /* Publish at the sonar_poll_s cadence even when the boost
             * is sampling faster — HA history stays at the knob rate
             * while the panel/trigger get every sample. Clear readings
             * publish the 999 sentinel so dashboards see "nothing in
             * range" instead of the artifact distance. */
            int64_t now = esp_timer_get_time();
            if (now - last_pub_us >= (int64_t)period_s * 1000000LL) {
                mqtt_publish_distance(clear ? SONAR_CLEAR_SENTINEL_CM : cm);
                last_pub_us = now;
            }
        } else {
            atomic_store(&s_have_cm, false);
            atomic_store(&s_is_clear, false);
            if (was_valid)
                ESP_LOGW(TAG, "no echo on GPIO%d — sensor unplugged or "
                              "target out of range (3 cm..3.5 m)", s_pin);
            was_valid = false;
        }

        /* Sleep: ~2 Hz while the operator is watching the OLED ENV page
         * OR the proximity trigger is armed (its latency must not hang
         * off the MQTT knob); else the knob period in 1 s slices —
         * exiting early when the ENV page comes up so the boost engages
         * within a second of the button press. */
        if (oled_env_page_visible() || trig_cm > 0) {
            vTaskDelay(pdMS_TO_TICKS(SONAR_BOOST_POLL_MS));
            if (atomic_load(&s_should_stop))
                goto stop;
        } else {
            for (int32_t sec = 0; sec < period_s; sec++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (atomic_load(&s_should_stop))
                    goto stop;
                /* Wake early when the ENV page comes up OR the trigger
                 * gets armed mid-sleep — otherwise a fresh
                 * sonar_trig_cm would wait out the full poll period
                 * before its first evaluation. */
                if (oled_env_page_visible() ||
                    app_config_get_int("sonar_trig_cm") > 0)
                    break;
            }
        }
        continue;
stop:
        gpio_intr_disable((gpio_num_t)s_pin);
        gpio_isr_handler_remove((gpio_num_t)s_pin);
        atomic_store(&s_have_cm, false);
        atomic_store(&s_is_clear, false);
        atomic_store(&s_trig_pending, false);
        atomic_store(&s_ready, false);
        atomic_store(&s_should_stop, false);
        ESP_LOGI(TAG, "task exiting (sonar_enabled=OFF)");
        vTaskDelete(NULL);
        return;
    }
}

/* Arm the GPIO + ISR + spawn the poll task. Idempotent; caller has
 * already checked the sonar_enabled gate. */
static esp_err_t sonar_start(void) {
    if (atomic_load(&s_ready))
        return ESP_OK;

    s_pin = app_config_pin_for_first("sonar");
    if (s_pin < 0) {
        ESP_LOGI(TAG, "no GPIO mapped to 'sonar' in pin map — module idle");
        return ESP_OK;
    }

    if (!s_echo_done) {
        s_echo_done = xSemaphoreCreateBinary();
        if (!s_echo_done)
            return ESP_ERR_NO_MEM;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << s_pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_config GPIO%d: %s", s_pin, esp_err_to_name(err));
        return err;
    }
    /* main.cpp installs the shared ISR service for the BOOT button well
     * before we run; tolerate "already installed" for any init-order
     * future-proofing. */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "isr service: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_isr_handler_add((gpio_num_t)s_pin, echo_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "isr add GPIO%d: %s", s_pin, esp_err_to_name(err));
        return err;
    }
    gpio_intr_disable((gpio_num_t)s_pin);  /* opened per echo window */

    /* CPU0 like reed/pir — don't preempt audio on CPU1. Stack sized for
     * the esp_mqtt publish call made from the loop (outbox enqueue path
     * runs on the caller's stack — 4 KB gives it honest headroom). */
    BaseType_t ok = xTaskCreatePinnedToCore(sonar_task, "sonar", 4096, NULL,
                                            tskIDLE_PRIORITY + 1, NULL,
                                            /*core*/ 0);
    if (ok != pdPASS) {
        gpio_isr_handler_remove((gpio_num_t)s_pin);
        ESP_LOGW(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&s_ready, true);
    ESP_LOGI(TAG, "ultrasonic ranger armed on GPIO%d (poll %" PRId32 " s, "
                  "median of %d pings)",
             s_pin, app_config_get_int("sonar_poll_s"), SONAR_PINGS_PER_POLL);
    return ESP_OK;
}

esp_err_t sonar_init(void) {
    if (atomic_load(&s_ready))
        return ESP_OK;
    if (!app_config_get_bool("sonar_enabled")) {
        ESP_LOGI(TAG, "sonar_enabled=OFF — skipping init");
        return ESP_OK;
    }
    return sonar_start();
}

void sonar_apply_config(void) {
    bool want_on = app_config_get_bool("sonar_enabled");
    bool is_on   = atomic_load(&s_ready);
    if (want_on && !is_on) {
        ESP_LOGI(TAG, "sonar_enabled flipped ON live — arming");
        (void)sonar_start();
    } else if (!want_on && is_on) {
        ESP_LOGI(TAG, "sonar_enabled flipped OFF live — requesting task exit");
        atomic_store(&s_should_stop, true);
    }
}

bool sonar_ready(void) { return atomic_load(&s_ready); }

bool sonar_last_cm(float *out_cm) {
    if (!atomic_load(&s_have_cm))
        return false;
    uint32_t bits = atomic_load(&s_last_cm_bits);
    memcpy(out_cm, &bits, sizeof(*out_cm));
    return true;
}

bool sonar_is_clear(void) { return atomic_load(&s_is_clear); }

bool sonar_trigger_consume(void) {
    return atomic_exchange(&s_trig_pending, false);
}

uint32_t sonar_trigger_count(void) {
    return (uint32_t)atomic_load(&s_trig_count);
}
