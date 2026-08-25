/* soil.c — Grove Soil Moisture on the "soil" pin function. See soil.h.
 *
 * The probe is a plain resistive divider: SIG voltage rises with
 * moisture (≈0 mV in dry air, ~1.5–2 V in saturated soil at 3V3).
 * There is no presence detection possible on an analog pin — a
 * floating input reads plausible garbage — so soil_enabled doubles as
 * the operator's "it is physically wired" statement, same contract as
 * reed_enabled. */

#include "soil.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "app_config.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt.h"
#include "oled.h"

static const char *TAG = "soil";

#define SOIL_AVG_SAMPLES   16
#define SOIL_POLL_S_MIN    1
/* Sampling boost while the OLED ENV page is on-screen — see sonar.c;
 * same contract: the boost feeds the panel, MQTT keeps soil_poll_s. */
#define SOIL_BOOST_POLL_MS 500

/* ADC plumbing is created once and kept for the process lifetime (a
 * second adc_oneshot_new_unit on the same unit fails, so a disable→
 * enable cycle must not recreate it). The poll task is what starts and
 * stops with the soil_enabled gate. */
static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t         s_cali;
static adc_channel_t             s_chan;
static bool                      s_cali_ok;
static bool                      s_adc_ready;
static int                       s_pin = -1;
static atomic_bool               s_armed = ATOMIC_VAR_INIT(false);
static atomic_bool               s_should_stop = ATOMIC_VAR_INIT(false);

/* Cache for the HTML page (telemetry task writes, httpd reads). */
static _Atomic uint32_t s_last_mv_bits, s_last_pct_bits;
static atomic_bool      s_have_last = ATOMIC_VAR_INIT(false);

static uint32_t f2bits(float f) { uint32_t b; memcpy(&b, &f, sizeof(b)); return b; }
static float bits2f(uint32_t b) { float f; memcpy(&f, &b, sizeof(f)); return f; }

/* Poll task: one averaged conversion + publish every soil_poll_s (read
 * live, so the operator can speed it up while calibrating). Analog
 * moisture drifts on garden timescales normally, but during dry/wet
 * calibration the operator IS the signal — a telemetry-tick cadence
 * (minutes) made the readout look random; seconds make it live. */
static void soil_task(void *arg) {
    (void)arg;
    int64_t last_pub_us = 0;
    while (true) {
        int32_t period_s = app_config_get_int("soil_poll_s");
        if (period_s < SOIL_POLL_S_MIN)
            period_s = SOIL_POLL_S_MIN;
        float mv, pct;
        if (soil_read(&mv, &pct)) {
            /* Cache (OLED/HTML) is fresh every sample; MQTT stays at
             * the soil_poll_s cadence even under the OLED boost. */
            int64_t now = esp_timer_get_time();
            if (now - last_pub_us >= (int64_t)period_s * 1000000LL) {
                mqtt_publish_soil(mv, pct);
                last_pub_us = now;
            }
        }

        /* Sleep: ~2 Hz while the OLED ENV page is up, else the knob
         * period in 1 s slices with an early exit when it comes up. */
        if (oled_env_page_visible()) {
            vTaskDelay(pdMS_TO_TICKS(SOIL_BOOST_POLL_MS));
            if (atomic_load(&s_should_stop))
                goto stop;
        } else {
            for (int32_t sec = 0; sec < period_s; sec++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (atomic_load(&s_should_stop))
                    goto stop;
                if (oled_env_page_visible())
                    break;
            }
        }
        continue;
stop:
        atomic_store(&s_armed, false);
        atomic_store(&s_should_stop, false);
        ESP_LOGI(TAG, "task exiting (soil_enabled=OFF)");
        vTaskDelete(NULL);
        return;
    }
}

/* Arm the ADC channel for whatever pad carries "soil" + spawn the poll
 * task. Idempotent; caller has already checked the soil_enabled gate. */
static esp_err_t soil_start(void) {
    if (atomic_load(&s_armed))
        return ESP_OK;
    if (s_adc_ready)
        goto spawn;   /* re-enable after a live OFF — ADC survives */

    s_pin = app_config_pin_for_first("soil");
    if (s_pin < 0) {
        ESP_LOGI(TAG, "no GPIO mapped to 'soil' in pin map — module idle");
        return ESP_OK;
    }

    /* GPIO → ADC unit/channel. The pin-map setter refuses non-ADC1 pads
     * for "soil", but legacy NVS written before that guard could still
     * hold one — verify and bail loudly rather than misread. (ADC2 is
     * unusable anyway: it's arbitrated against WiFi on the S3.) */
    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(s_pin, &unit, &s_chan);
    if (err != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGW(TAG, "GPIO%d is not an ADC1 pad — map 'soil' to D0..D5 "
                      "(GPIO1..6); module idle", s_pin);
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    err = adc_oneshot_new_unit(&ucfg, &s_unit);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(err));
        return err;
    }
    /* 12 dB attenuation ⇒ usable range ~0..3.1 V, covering the probe's
     * full swing at 3V3 supply. */
    adc_oneshot_chan_cfg_t ccfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_unit, s_chan, &ccfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adc channel config: %s", esp_err_to_name(err));
        return err;
    }

    adc_cali_curve_fitting_config_t cal = {
        .unit_id  = ADC_UNIT_1,
        .chan     = s_chan,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) == ESP_OK);
    if (!s_cali_ok)
        ESP_LOGW(TAG, "no eFuse ADC calibration — falling back to linear mV");
    s_adc_ready = true;

spawn:
    /* CPU0 like the other cheap poll tasks; 3 KB covers the esp_mqtt
     * outbox-enqueue path called from the loop. */
    if (xTaskCreatePinnedToCore(soil_task, "soil", 3072, NULL,
                                tskIDLE_PRIORITY + 1, NULL, /*core*/ 0) != pdPASS) {
        ESP_LOGW(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_armed, true);
    ESP_LOGI(TAG, "soil moisture ADC armed on GPIO%d (ADC1_CH%d, %s cal, "
                  "poll %" PRId32 " s)",
             s_pin, (int)s_chan, s_cali_ok ? "curve-fit" : "linear",
             app_config_get_int("soil_poll_s"));
    /* Seed the cache once so the OLED/HTML show a value right away
     * (the task's first publish may still be waiting for MQTT). */
    float mv, pct;
    (void)soil_read(&mv, &pct);
    return ESP_OK;
}

esp_err_t soil_init(void) {
    if (!app_config_get_bool("soil_enabled")) {
        ESP_LOGI(TAG, "soil_enabled=OFF — skipping init");
        return ESP_OK;
    }
    return soil_start();
}

void soil_apply_config(void) {
    bool want_on = app_config_get_bool("soil_enabled");
    bool is_on   = atomic_load(&s_armed);
    if (want_on && !is_on) {
        ESP_LOGI(TAG, "soil_enabled flipped ON live — arming");
        (void)soil_start();
    } else if (!want_on && is_on) {
        ESP_LOGI(TAG, "soil_enabled flipped OFF live — requesting task exit");
        atomic_store(&s_should_stop, true);
    }
}

bool soil_ready(void) { return atomic_load(&s_armed); }

bool soil_read(float *out_mv, float *out_pct) {
    if (!atomic_load(&s_armed) || !app_config_get_bool("soil_enabled"))
        return false;

    int acc = 0, n = 0;
    for (int i = 0; i < SOIL_AVG_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(s_unit, s_chan, &raw) == ESP_OK) {
            acc += raw;
            n++;
        }
    }
    if (n == 0)
        return false;

    int raw_avg = acc / n;
    int mv;
    if (s_cali_ok) {
        if (adc_cali_raw_to_voltage(s_cali, raw_avg, &mv) != ESP_OK)
            return false;
    } else {
        mv = raw_avg * 3100 / 4095;  /* uncalibrated linear, 12 dB ≈ 3.1 V FS */
    }

    /* Percent from the operator's two-point calibration. Degenerate
     * knobs (wet ≤ dry, e.g. before first calibration) ⇒ pct = NAN so
     * the publisher can skip it while raw mV still flows. */
    int32_t dry = app_config_get_int("soil_dry_mv");
    int32_t wet = app_config_get_int("soil_wet_mv");
    float pct = NAN;
    if (wet > dry) {
        pct = 100.0f * ((float)mv - (float)dry) / (float)(wet - dry);
        if (pct < 0.0f)   pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
    }

    *out_mv = (float)mv;
    *out_pct = pct;
    atomic_store(&s_last_mv_bits, f2bits((float)mv));
    atomic_store(&s_last_pct_bits, f2bits(pct));
    atomic_store(&s_have_last, true);
    return true;
}

bool soil_last(float *out_mv, float *out_pct) {
    if (!atomic_load(&s_have_last))
        return false;
    *out_mv = bits2f(atomic_load(&s_last_mv_bits));
    *out_pct = bits2f(atomic_load(&s_last_pct_bits));
    return true;
}
