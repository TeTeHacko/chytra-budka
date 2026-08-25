/* camera.c — OV2640/OV3660 capture + IR LED PWM. See camera.h. */

#include "camera.h"
#include "app_main_exports.h"
#include "cb_pm.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_stamp.h"
#include "mqtt.h"
#include "photo_queue.h"
#include "sd_storage.h"
#include "audiofx.h"
#include "status_led.h"
#include "oled.h"

static const char *TAG = "camera";

/* ── IR LED (LEDC PWM) ─────────────────────────────────────────────── */
#define IR_LEDC_TIMER       LEDC_TIMER_0
#define IR_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define IR_LEDC_CHANNEL     LEDC_CHANNEL_0
#define IR_LEDC_DUTY_RES    LEDC_TIMER_8_BIT
#define IR_LEDC_FREQ_HZ     5000
#define IR_WARMUP_MS        80      /* sensor + LED settle */
#define IR_HOLD_MS          400     /* total time LED stays on per shot */

/* State */
static bool s_camera_ready = false;
static atomic_uint_fast32_t s_capture_count = 0;
static atomic_uint_fast32_t s_capture_failures = 0;
static atomic_uint_fast32_t s_capture_seq = 0;  /* monotonic seq for filenames */

/* Serialize concurrent capture requests across tasks (main loop + HTTP). */
static SemaphoreHandle_t s_capture_mtx = NULL;

/* Last-JPEG cache (PSRAM-allocated, replaced on every capture). */
static SemaphoreHandle_t s_last_mtx = NULL;
static uint8_t          *s_last_buf = NULL;
static size_t            s_last_len = 0;

/* Currently-applied sensor profile. capture = stills, stream = MJPEG.
 * The MJPEG handler flips to stream on entry and back to capture on every
 * exit; app_config side-effects read this to decide whether a cam_ /
 * mjpg_ key change should apply immediately or be deferred to the next
 * profile transition. */
typedef enum { CAM_PROFILE_CAPTURE = 0, CAM_PROFILE_STREAM = 1 } cam_profile_t;
static atomic_int s_active_profile = CAM_PROFILE_CAPTURE;

#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
/* Set by the /debug/cam_standby spike: true when the OV3660 is in software
 * power-down. camera_capture_event() reads it to wake-on-capture (shoot, then
 * re-standby) so PIR/MQTT captures still work while the standby measurement
 * runs. Debug-only — never set in field/production (endpoint compiled out). */
static atomic_bool s_sensor_standby = false;
#endif

static esp_err_t ir_led_init(void) {
    ledc_timer_config_t tcfg = {
        .speed_mode      = IR_LEDC_MODE,
        .timer_num       = IR_LEDC_TIMER,
        .duty_resolution = IR_LEDC_DUTY_RES,
        .freq_hz         = IR_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) return err;

    /* Pin map lookup. rev3.2 default is D2/GPIO3 (matches IR_LED_PIN
     * #define). When no slot is mapped to "ir_led" the LEDC channel
     * just doesn't bind to any pad — camera_ir_led_set() still
     * updates the duty register but the signal goes nowhere, which
     * is what an operator who doesn't have an IR LED wired wants. */
    int pin = app_config_pin_for_first("ir_led");
    if (pin < 0) {
        ESP_LOGI(TAG, "no GPIO mapped to 'ir_led' in pin map — channel idle");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "IR LED on GPIO%d (compile-time default %d)", pin, IR_LED_PIN);

    ledc_channel_config_t ccfg = {
        .gpio_num   = (int)pin,
        .speed_mode = IR_LEDC_MODE,
        .channel    = IR_LEDC_CHANNEL,
        .timer_sel  = IR_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    return ledc_channel_config(&ccfg);
}

void camera_ir_led_set(uint8_t duty) {
    ledc_set_duty(IR_LEDC_MODE, IR_LEDC_CHANNEL, duty);
    ledc_update_duty(IR_LEDC_MODE, IR_LEDC_CHANNEL);
}

/* ── Capture indicator LED (plain GPIO, active-high) ───────────────── */
/* Visible LED that lights for the full duration of a capture event,
 * regardless of IR/AGC. Separate from STATUS_LED (onboard, active-low)
 * so a stealth field unit can keep the onboard LED off but still flash
 * a remote/wired indicator (or vice versa). -1 = not configured. */
static int s_capture_led_pin = -1;

static void capture_led_init(void) {
    s_capture_led_pin = -1;
    if (!app_config_get_bool("cap_led_en")) {
        ESP_LOGI(TAG, "capture LED disabled (cap_led_en=OFF)");
        return;
    }
    /* Pin map lookup. rev3.2 default is D3/GPIO4 (matches
     * CAPTURE_LED_PIN #define). The pin-map setter cross-validation
     * already enforces "no two singletons on the same slot" so a
     * collision with ir_led can't reach NVS — but defense-in-depth:
     * if it somehow does (stale NVS from an old firmware), we'd lose
     * the LEDC bind. Belt-and-suspenders check stays. */
    int pin = app_config_pin_for_first("capture_led");
    if (pin < 0) {
        ESP_LOGI(TAG, "no GPIO mapped to 'capture_led' in pin map — disabled");
        return;
    }
    int ir_pin = app_config_pin_for_first("ir_led");
    if (pin == ir_pin) {
        ESP_LOGW(TAG, "capture_led GPIO%d collides with ir_led — "
                      "the LEDC channel owns the pad; disabling capture LED", pin);
        return;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "capture LED GPIO%d config failed", pin);
        return;
    }
    gpio_set_level((gpio_num_t)pin, 0);
    s_capture_led_pin = pin;
    ESP_LOGI(TAG, "capture LED on GPIO%d (active-high)", pin);
}

static inline void capture_led_set(bool on) {
    if (s_capture_led_pin >= 0) {
        gpio_set_level((gpio_num_t)s_capture_led_pin, on ? 1 : 0);
    }
}

/* OV3660 get_agc_gain() returns the REAL analog gain multiplier (the integer
 * part of the 6.4-fixed-point 0x350a/0x350b register), which set_agc_gain()
 * caps at 64x — so the valid range is 0..64, not 0..30. (The old 30 ceiling
 * pre-dates raising cam_gainceil past ~16x; at a 32x gain ceiling a legitimate
 * dark-scene gain of 31-32 would have been mis-flagged as garbage → IR killed
 * exactly when it's needed.) Anything above 64 is a stale/garbage SCCB read. */
#define AGC_MAX_VALID 64

/* Cached AGC populated by `camera_get_agc_gain()` from the telemetry
 * tick. `ir_should_fire()` reads this cached value instead of doing
 * a live SCCB transaction inside the capture hot path.
 *
 * Background: doing `init_status()` per capture issued a synchronous
 * I²C transaction on the same bus the camera's `cam_task` uses for
 * its own DMA/control interrupts. Under PIR-storm capture rates the
 * I²C driver's critical sections held interrupts disabled long enough
 * to trip `int_wdt` on `cam_task`'s ISR entry (observed on bench
 * `cb-ex01`, coredump backtrace pointed at SCCB_Read16
 * inside `ir_should_fire`).
 *
 * The cache moves the read to a single periodic sample on the
 * telemetry tick (one per `tlm_*_s` interval, default 60-300 s).
 * Day/night transitions happen on minute-scale timescales, so the
 * staleness is acceptable for the IR fire decision. A value of -1
 * means "not yet sampled" (boot before first telemetry tick) or
 * "last sample failed"; the IR decision defaults to "don't fire"
 * in both cases — matches the safe-battery posture. */
static atomic_int s_cached_agc = ATOMIC_VAR_INIT(-1);

/* Read the sensor's live AGC gain via init_status(). Acquires
 * s_capture_mtx to avoid concurrent SCCB access with a capture in
 * flight. Returns -1 if no sensor / no mutex / contention / SCCB
 * read returned an out-of-range value (likely bus fault). Side
 * effect: on success, updates s_cached_agc for the capture path;
 * on contention the cache stays at its prior value, so the IR-fire
 * decision keeps using the last-known-good gain. */
int camera_get_agc_gain(void) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->init_status) return -1;
    if (!s_capture_mtx) return -1;

    /* Non-blocking try — telemetry tick must not wait on a capture in
     * flight (held up to 500 ms). A missed refresh is harmless: the
     * cache stays valid for the IR-fire path, and the next telemetry
     * tick (60-300 s away) refreshes it. Previously this was a 100 ms
     * timeout which still let the telemetry tick stall by 100 ms during
     * every capture event. */
    if (xSemaphoreTake(s_capture_mtx, 0) != pdTRUE) {
        return -1;
    }
    cb_pm_no_sleep_acquire();  /* no light-sleep mid SCCB read */
    s->init_status(s);
    int agc = s->status.agc_gain;
    cb_pm_no_sleep_release();
    xSemaphoreGive(s_capture_mtx);
    if (agc < 0 || agc > AGC_MAX_VALID) {
        atomic_store(&s_cached_agc, -1);
        return -1;
    }
    atomic_store(&s_cached_agc, agc);
    return agc;
}

/* Read both live AE controls in ONE SCCB status refresh: AGC gain and the AEC
 * exposure value (line-units). Exposure is the responsive light proxy — the AE
 * loop lengthens exposure as the scene darkens and only raises gain near true
 * darkness, so gain alone pins low indoors. Returns true on success (outputs
 * filled), false on no sensor / contention. Same non-blocking mutex as above. */
bool camera_get_ae(int *gain, int *exposure) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->init_status || !s_capture_mtx) return false;
    if (xSemaphoreTake(s_capture_mtx, 0) != pdTRUE) return false;
    cb_pm_no_sleep_acquire();
    s->init_status(s);
    int g = s->status.agc_gain;
    int e = s->status.aec_value;
    cb_pm_no_sleep_release();
    xSemaphoreGive(s_capture_mtx);
    if (gain)     *gain = g;
    if (exposure) *exposure = e;
    return true;
}

#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
/* BENCH-ONLY measurement spike: toggle the OV3660 software power-down.
 * SYSTEM_CTROL0 (0x3008) bit[6]=0x40 is the sensor's software standby;
 * bit[7]=0x80 is software RESET — do NOT touch that (the doc note saying
 * "bit7" is wrong — it would reset, not standby). Standby gates the
 * sensor's analog rail (per OV3660 datasheet/forum ~37.8 → ~1.45 mA), but
 * light-sleep already clock-gates the XCLK, so this only trims residual
 * analog draw in awake windows — Rank 4 "measure-first" in POWER_LOWPOWER.md.
 * This helper exists purely to MEASURE that delta on the bench power meter;
 * it is deliberately NOT wired into the mode FSM, and compiled out of
 * field/production (debug-endpoints gate).
 *
 * Holds s_capture_mtx (so no capture/stream/profile-apply interleaves the
 * SCCB write) and a NO_LIGHT_SLEEP lock (the XCLK must run for SCCB). On
 * wake (on=false) it drains a few frames to flush post-power-up garbage,
 * same as the light-sleep wake path. Caveat: AGC/AEC is stale after a long
 * standby — the first capture can be mis-exposed until the AE loop settles.
 * Operator must keep the camera idle (no live stream) across the toggle.
 * Returns ESP_OK on a successful SCCB write. */
esp_err_t camera_debug_sensor_standby(bool on) {
    if (!s_camera_ready) return ESP_ERR_INVALID_STATE;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_reg || !s_capture_mtx) return ESP_ERR_NOT_FOUND;
    if (xSemaphoreTake(s_capture_mtx, pdMS_TO_TICKS(1500)) != pdTRUE) {
        ESP_LOGW(TAG, "cam standby: busy (mtx timeout)");
        return ESP_ERR_TIMEOUT;
    }
    cb_pm_no_sleep_acquire();  /* XCLK must run for the SCCB write */
    /* read-modify-write: set/clear only bit6, leave the rest of 0x3008 intact. */
    int r = s->set_reg(s, 0x3008, 0x40, on ? 0x40 : 0x00);
    if (!on && r >= 0) {
        for (int d = 0; d < 4; d++) {
            camera_fb_t *junk = esp_camera_fb_get();
            if (junk) esp_camera_fb_return(junk);
            (void)esp_task_wdt_reset();
        }
    }
    if (r >= 0) atomic_store(&s_sensor_standby, on);
    cb_pm_no_sleep_release();
    xSemaphoreGive(s_capture_mtx);
    ESP_LOGW(TAG, "OV3660 software standby %s (set_reg 0x3008 bit6 -> %d)",
             on ? "ON" : "OFF", r);
    return (r < 0) ? ESP_FAIL : ESP_OK;
}
#endif /* CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS */

/* Decide whether to fire the IR illuminator for this capture based on
 * the sensor's own AGC gain. init_status() always returns 0 in the
 * OV3660 driver regardless of SCCB success, so we detect read faults
 * by clamping against AGC_MAX_VALID.
 *
 * agc_gain is the real analog gain MULTIPLIER (AE raises exposure first, then
 * gain only once exposure is at its ceiling), interpretation (OV3660):
 *   1     — full daylight saturation (sensor needs no help)
 *   ~2-5  — bright indoor / overcast outdoor
 *   ~8+   — dim
 *   32    — gain ceiling pinned (cam_gainceil default) → certainly dark
 *
 * Threshold default 8 (in app_config) is an initial guess for "indoor
 * dusk" — tune on real ambient_agc graphs. Logs print the observed
 * gain on every capture. NOTE: only meaningful now that cam_gainceil is
 * written correctly (see camera_apply_tuning) — with the old broken ceiling
 * the gain stayed pinned at 1 and this decision never fired.
 *
 * Returns true to fire IR. Writes the cached AGC value to *agc_out
 * (-1 if not yet sampled). When AGC is unknown we default to "don't
 * fire" — same safe-battery posture as before, just now reading from
 * the cache. No SCCB transaction inside this function: the live read
 * happens on the telemetry tick via `camera_get_agc_gain()`. */
static bool ir_should_fire(int *agc_out) {
    if (agc_out) *agc_out = -1;
    if (!app_config_get_bool("ir_led_enabled")) return false;

    int agc = atomic_load(&s_cached_agc);
    if (agc < 0 || agc > AGC_MAX_VALID) return false;
    if (agc_out) *agc_out = agc;

    int thresh = (int)app_config_get_int("ir_agc_thresh");
    return agc >= thresh;
}

static esp_err_t apply_profile_internal(const char *fs_key, const char *q_key,
                                        cam_profile_t new_profile,
                                        const char *label) {
    if (!s_camera_ready) return ESP_ERR_INVALID_STATE;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return ESP_ERR_NOT_FOUND;
    int fs = (int)app_config_get_int(fs_key);
    int q  = (int)app_config_get_int(q_key);

    /* set_framesize tears down DMA and re-arms the sensor and shares
     * the SCCB bus with capture_event. The wait must out-last an in-flight
     * capture's mutex hold, else opening the live stream while a capture
     * runs (PIR-triggered photo, or a coincident capture) fails with
     * "stream profile apply failed" (HTTP 500). The old 200 ms was chosen
     * BELOW the ~480 ms IR-warmup window to fail fast — but that surfaced
     * the 500 to the operator, and UXGA stills hold the mutex longer still
     * (bigger frame + drain). 1500 ms lets the stream wait the capture out;
     * the live stream is the field camera-positioning tool, so blocking the
     * HTTP request briefly beats a spurious 500. A genuinely wedged sensor
     * still times out (and the caller re-tries on the next cycle).
     *
     * If the mutex can't be taken, REFUSE to issue the SCCB writes —
     * a concurrent capture's register sequence interleaved with our
     * set_framesize/set_quality would leave the OV3660 in an undefined
     * state until the next reset. The caller (HTTP handler / app_config
     * knob change) re-applies on the next opportunity:
     *   - app_config sets the value in NVS regardless and the next
     *     stream-exit hook in http_server.c calls us again from a
     *     quiet moment;
     *   - HTTP MJPEG-entry retries on the next request. */
    if (!s_capture_mtx) {
        ESP_LOGE(TAG, "profile %s: capture mutex not initialised", label);
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_capture_mtx, pdMS_TO_TICKS(1500)) != pdTRUE) {
        ESP_LOGW(TAG, "profile %s: capture mtx timeout — refusing to "
                      "issue SCCB without lock (will re-try on next "
                      "config/stream cycle)", label);
        return ESP_ERR_TIMEOUT;
    }
    cb_pm_no_sleep_acquire();  /* no light-sleep across SCCB writes + frame drain */
    int rc_fs = s->set_framesize(s, (framesize_t)fs);
    int rc_q  = s->set_quality(s, q);
    /* Only claim the new profile is active once BOTH sensor writes
     * succeeded — otherwise a failed apply (returned as ESP_FAIL below)
     * would leave s_active_profile lying about the sensor state, so the
     * next /capture would shoot at the wrong (e.g. stream) quality. */
    if (rc_fs == 0 && rc_q == 0)
        atomic_store(&s_active_profile, (int)new_profile);

    /* Drain stale frames the OV3660 + DMA pumped in at the OLD
     * framesize/quality. The sensor's JPEG pipeline holds 3+ in-flight
     * frames at high quality, especially right after set_framesize.
     * Symptom of an under-drain was HIL sweep capturing at OLD (fs, q)
     * for the first 1-3 frames after a change → predicate mismatch,
     * test marking those iterations skip.
     *
     * Algorithm: drain UNTIL width matches the requested framesize AND
     * we've consumed at least 3 frames (covers quality-only changes
     * where width never changes and the simple "match" check would bail
     * too early, leaving an old-quality frame in the queue). Capped at
     * 8 pulls so a misconfigured sensor can't wedge us. Cost: up to
     * 8 × frame_period (~250 ms at UXGA, less at smaller framesizes). */
    framesize_t want_fs = (framesize_t)fs;
    extern const resolution_info_t resolution[];  /* from esp32-camera */
    uint16_t want_w = resolution[want_fs].width;
    int dropped = 0;
    bool width_ok = false;
    for (int i = 0; i < 8; i++) {
        camera_fb_t *stale = esp_camera_fb_get();
        if (!stale) break;
        if (stale->width == want_w) width_ok = true;
        esp_camera_fb_return(stale);
        dropped++;
        if (width_ok && dropped >= 3) break;
    }
    cb_pm_no_sleep_release();
    xSemaphoreGive(s_capture_mtx);

    if (rc_fs != 0 || rc_q != 0) {
        ESP_LOGW(TAG, "profile %s applied with errors (fs=%d rc=%d, q=%d rc=%d)",
                 label, fs, rc_fs, q, rc_q);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "profile=%s framesize=%d quality=%d", label, fs, q);
    return ESP_OK;
}

esp_err_t camera_apply_capture_profile(void) {
    return apply_profile_internal("cam_framesize", "cam_quality",
                                  CAM_PROFILE_CAPTURE, "capture");
}

esp_err_t camera_apply_stream_profile(void) {
    return apply_profile_internal("mjpg_framesize", "mjpg_quality",
                                  CAM_PROFILE_STREAM, "stream");
}

bool camera_stream_profile_active(void) {
    return atomic_load(&s_active_profile) == CAM_PROFILE_STREAM;
}

/* Operator-tunable OV3660 image settings, all NVS-backed (see the app_config
 * SCHEMA). Dialled in over the air via cmd/cfg without a reflash — re-applied
 * live by app_config's side-effect dispatch. No-op when the sensor isn't up
 * (camera disabled / detect failed). The schema clamps each range, so the
 * values are already valid; a set_* the sensor doesn't implement returns <0
 * and is harmless. Defaults reproduce the previous hard-coded neutral values. */
void camera_apply_tuning(void) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    int bri = app_config_get_int("cam_brightness");
    int con = app_config_get_int("cam_contrast");
    int sat = app_config_get_int("cam_saturation");
    int shp = app_config_get_int("cam_sharpness");
    int ael = app_config_get_int("cam_ae_level");
    int wb  = app_config_get_int("cam_wb_mode");
    int fx  = app_config_get_int("cam_special_fx");
    int gc  = app_config_get_int("cam_gainceil");
    s->set_brightness(s, bri);
    s->set_contrast(s, con);
    s->set_saturation(s, sat);
    if (s->set_sharpness)   s->set_sharpness(s, shp);
    if (s->set_ae_level)    s->set_ae_level(s, ael);
    s->set_wb_mode(s, wb);                 /* honoured while AWB stays enabled */
    s->set_special_effect(s, fx);
    /* The esp32-camera OV3660 set_gainceiling() writes the enum ORDINAL (0..6)
     * straight into the 6.4-fixed-point ceiling register 0x3A18/0x3A19, which
     * clobbers the sensor's sane factory ceiling (table default 0x0F8 = 15.5x)
     * with a near-zero ceiling. The AGC then can't add gain: reported gain pins
     * at 1x, never climbs to ir_agc_thresh (so auto-IR is dead), and dim scenes
     * just under-expose instead of brightening. Write the REAL ceiling ourselves
     * — map the 2x..64x enum to (mult<<4) in the 6.4 register. (128x exceeds the
     * OV3660's 64x hardware max, so it clamps to 64x.) */
    int gc_real = 0;
    if (s->set_reg) {
        static const uint16_t gc_mult[] = { 2, 4, 8, 16, 32, 64, 64 };
        int gci = (gc < 0) ? 0 : (gc > 6 ? 6 : gc);
        uint16_t reg = (uint16_t)(gc_mult[gci] << 4);   /* 6.4 fixed-point gain */
        if (reg > 0x3FF) reg = 0x3FF;                   /* 10-bit register field */
        s->set_reg(s, 0x3A18, 0x03, (reg >> 8) & 0x03);
        s->set_reg(s, 0x3A19, 0xFF, reg & 0xFF);
        gc_real = gc_mult[gci];
    }
    ESP_LOGI(TAG, "tuning: bri=%d con=%d sat=%d shp=%d ae=%d wb=%d fx=%d gc=%d(%dx)",
             bri, con, sat, shp, ael, wb, fx, gc, gc_real);
}

void camera_apply_orientation(void) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    /* Right-side-up mount: hmirror=1 cancels the sensor's mirrored
     * readout vs. scene; vflip=0 keeps top up. cam_rotate_180 XORs
     * both axes so an upside-down camera produces an upright image.
     *
     * Hold s_capture_mtx so a concurrent camera_capture_event() can't
     * read out a half-flipped frame mid-update. set_hmirror/vflip are
     * fast SCCB writes (sub-ms), short timeout is fine. */
    bool got_lock = false;
    if (s_capture_mtx &&
        xSemaphoreTake(s_capture_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        got_lock = true;
        cb_pm_no_sleep_acquire();  /* no light-sleep across the SCCB writes */
    }
    bool rot = app_config_get_bool("cam_rotate_180");
    s->set_hmirror(s, rot ? 0 : 1);
    s->set_vflip(s, rot ? 1 : 0);
    if (got_lock) {
        cb_pm_no_sleep_release();
        xSemaphoreGive(s_capture_mtx);
    }
    ESP_LOGI(TAG, "orientation: %s (hmirror=%d vflip=%d)",
             rot ? "rotated 180°" : "normal", rot ? 0 : 1, rot ? 1 : 0);
}

/* ── Camera init ───────────────────────────────────────────────────── */
/* Diagnostic: probe every 7-bit I2C address on the SCCB pins to see what
 * actually answers. Useful when esp_camera_init reports the sensor as
 * "not supported" — tells us whether the chip is on the bus at all, at
 * the expected address (0x30 for OV2640, 0x3c for OV3660/OV5640), or at
 * something else entirely. Runs on an ephemeral I2C bus on port 0 then
 * tears it down so the camera driver can claim its own bus on port 1. */
static void sccb_bus_scan_diag(void) {
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bcfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,                          /* auto-pick a free port */
        .sda_io_num = (gpio_num_t)CAM_PIN_SIOD,
        .scl_io_num = (gpio_num_t)CAM_PIN_SIOC,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    if (i2c_new_master_bus(&bcfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "sccb scan: bus alloc failed, skipping");
        return;
    }
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        /* Match SCCB driver's 1 s timeout exactly, so any difference in
         * outcome between this scan and SCCB_Probe rules out timeout-vs-
         * ACK distinction and points at bus parasitics or a probe-API
         * false positive. */
        esp_err_t pr = i2c_master_probe(bus, a, 1000);
        if (pr == ESP_OK) {
            ESP_LOGI(TAG, "sccb scan: 0x%02x ACK", a);
            found++;
        } else if (a == 0x30 || a == 0x3c) {
            /* Print the result for the known camera addresses regardless,
             * so we can see whether they NACK vs TIME-OUT vs error. */
            ESP_LOGI(TAG, "sccb scan: 0x%02x -> %s",
                     a, esp_err_to_name(pr));
        }
    }
    if (!found) {
        ESP_LOGW(TAG, "sccb scan: no devices ACK on SDA=%d SCL=%d — "
                      "camera chip is absent, unpowered, or wired wrong",
                 CAM_PIN_SIOD, CAM_PIN_SIOC);
    }
    i2c_del_master_bus(bus);
}

esp_err_t camera_init(void) {
    if (s_camera_ready) return ESP_OK;

    /* Both mutexes are load-bearing: s_capture_mtx serializes SCCB
     * access across capture + AGC sampler + orientation update;
     * s_last_mtx guards the PSRAM last-JPEG cache shared with HTTP.
     * Bailing out here prevents the "silently raceable" pattern where
     * subsequent code did `if (mtx && ...)` and skipped locking when
     * creation had failed. */
    if (!s_last_mtx) s_last_mtx = xSemaphoreCreateMutex();
    if (!s_capture_mtx) s_capture_mtx = xSemaphoreCreateMutex();
    if (!s_last_mtx || !s_capture_mtx) {
        ESP_LOGE(TAG, "mutex alloc failed (capture=%p last=%p)",
                 s_capture_mtx, s_last_mtx);
        return ESP_ERR_NO_MEM;
    }

    if (ir_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "IR LED PWM init failed (continuing without IR)");
    }
    capture_led_init();

    /* Boot the sensor in the user's persisted capture profile. After
     * init we don't re-apply (esp_camera_init already programmed these
     * values); subsequent runtime changes go through
     * camera_apply_capture_profile() / camera_apply_stream_profile(). */
    int boot_fs = (int)app_config_get_int("cam_framesize");
    int boot_q  = (int)app_config_get_int("cam_quality");
    camera_config_t cfg = {
        .pin_pwdn       = CAM_PIN_PWDN,
        .pin_reset      = CAM_PIN_RESET,
        .pin_xclk       = CAM_PIN_XCLK,
        .pin_sccb_sda   = CAM_PIN_SIOD,
        .pin_sccb_scl   = CAM_PIN_SIOC,
        .pin_d7         = CAM_PIN_D7,
        .pin_d6         = CAM_PIN_D6,
        .pin_d5         = CAM_PIN_D5,
        .pin_d4         = CAM_PIN_D4,
        .pin_d3         = CAM_PIN_D3,
        .pin_d2         = CAM_PIN_D2,
        .pin_d1         = CAM_PIN_D1,
        .pin_d0         = CAM_PIN_D0,
        .pin_vsync      = CAM_PIN_VSYNC,
        .pin_href       = CAM_PIN_HREF,
        .pin_pclk       = CAM_PIN_PCLK,

        .xclk_freq_hz   = 20000000,
        .ledc_timer     = LEDC_TIMER_1,    /* not LEDC_TIMER_0 (IR LED) */
        .ledc_channel   = LEDC_CHANNEL_1,

        .pixel_format   = PIXFORMAT_JPEG,
        .frame_size     = (framesize_t)boot_fs,  /* from NVS, default UXGA */
        .jpeg_quality   = boot_q,                /* from NVS, default 12 (lower=better) */
        .fb_count       = 2,               /* needs PSRAM */
        .fb_location    = CAMERA_FB_IN_PSRAM,
        /* GRAB_LATEST: when fb_get is slower than the sensor, the
         * driver silently drops older frames so we always pick up the
         * newest one. Trade-off accepted: motion-trigger snapshots
         * want freshness, not every frame, and esp_camera doesn't
         * expose a drop counter so we couldn't measure them anyway.
         * Switch to CAMERA_GRAB_WHEN_EMPTY for back-pressure semantics
         * if a continuous-FPS use case shows up later. */
        .grab_mode      = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", err);
        /* On failure cam_deinit on ESP32-S3 leaves the LCD_CAM divider
         * and clk_sel programmed (only DMA gets torn down), so XCLK
         * keeps running on GPIO 10. Re-scan now to see whether the
         * sensor became responsive with XCLK live — distinguishes
         * "chip absent" from "chip alive but stuck". */
        ESP_LOGW(TAG, "post-init re-scan with XCLK live:");
        sccb_bus_scan_diag();
        return err;
    }

    /* Sensor enables for outdoor wildlife: auto white-balance + AEC2 +
     * auto-gain on. The operator-tunable LEVELS (brightness/contrast/
     * saturation/sharpness/ae_level/wb_mode/special_fx/gainceiling) come from
     * NVS via camera_apply_tuning() so they can be dialled in over the air. */
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_aec2(s, 1);
        s->set_gain_ctrl(s, 1);
    }
    camera_apply_tuning();

    /* Orientation: hmirror=1 fixes the sensor's mirrored readout vs.
     * the scene; vflip=0 keeps top up. cam_rotate_180 (NVS-backed)
     * XORs both axes for upside-down mounts. */
    camera_apply_orientation();

    s_camera_ready = true;
    atomic_store(&s_active_profile, CAM_PROFILE_CAPTURE);
    /* Seed the AGC cache once at boot so the IR fire decision in
     * the first capture (before the first telemetry tick) doesn't
     * default to "don't fire" purely for lack of data. */
    int initial_agc = camera_get_agc_gain();
    ESP_LOGI(TAG, "camera ready (PID=0x%04x framesize=%d JPEG q=%d, fb in PSRAM, initial AGC=%d)",
             s ? s->id.PID : 0, boot_fs, boot_q, initial_agc);
    return ESP_OK;
}

bool camera_ready(void) { return s_camera_ready; }

camera_fb_t *camera_capture_fb(void) {
    if (!s_camera_ready) return NULL;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        atomic_fetch_add(&s_capture_failures, 1);
        ESP_LOGW(TAG, "esp_camera_fb_get returned NULL");
        return NULL;
    }
    atomic_fetch_add(&s_capture_count, 1);
    return fb;
}

void camera_release_fb(camera_fb_t *fb) {
    if (fb) esp_camera_fb_return(fb);
}

/* Grab one JPEG frame into a caller-owned heap copy, serialized with the
 * capture path via s_capture_mtx. The MJPEG stream uses this instead of a
 * raw camera_capture_fb(): the raw grab took NO lock, so an operator tapping
 * Capture while a stream was open ran two concurrent esp_camera_fb_get()s
 * (stream loop + camera_capture_event) on a driver that isn't multi-task
 * safe → frame-ring corruption / wedge. Holding the mutex only across the
 * grab + memcpy (not the slow TLS send of the copy) keeps captures from
 * stalling on a slow client. Returns byte length (0 on failure / OOM /
 * lock timeout); caller frees *out. */
size_t camera_grab_jpeg_copy(uint8_t **out) {
    if (out) *out = NULL;
    if (!out || !s_camera_ready) return 0;
    if (s_capture_mtx &&
        xSemaphoreTake(s_capture_mtx, pdMS_TO_TICKS(1000)) != pdTRUE)
        return 0;
    cb_pm_no_sleep_acquire();  /* no light-sleep across the frame DMA + copy */
    camera_fb_t *fb = camera_capture_fb();  /* raw grab — we hold the lock */
    size_t len = 0;
    if (fb && fb->buf && fb->len) {
        uint8_t *buf = (uint8_t *)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
        if (!buf) buf = (uint8_t *)malloc(fb->len);
        if (buf) {
            memcpy(buf, fb->buf, fb->len);
            *out = buf;
            len  = fb->len;
        }
    }
    if (fb) camera_release_fb(fb);
    cb_pm_no_sleep_release();
    if (s_capture_mtx) xSemaphoreGive(s_capture_mtx);
    return len;
}

esp_err_t camera_capture_event(const char *trigger_reason) {
    if (!s_camera_ready) return ESP_ERR_INVALID_STATE;
    bool status_led_active = false;

    /* Serialize: only one capture in flight at a time across all tasks.
     * 500 ms cap: main loop tiká po 40 ms audio framech, takže 2 s by
     * blokovalo ~50 framů a 1/15 TWDT okna. Skip on contention — caller
     * (PIR/MQTT/VAD) can retry on the next tick. */
    if (s_capture_mtx &&
        xSemaphoreTake(s_capture_mtx, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "capture: busy (mtx timeout)");
        return ESP_ERR_TIMEOUT;
    }
    cb_pm_no_sleep_acquire();  /* no light-sleep across IR warm + frame DMA */

#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
    /* Wake-on-capture: if the /debug/cam_standby spike powered the OV3660 down,
     * wake it for this shot (clear 0x3008 bit6) + drain a few post-wake frames,
     * then re-enter standby on the way out. Keeps PIR/MQTT captures working
     * while the standby power measurement runs. Under the capture mutex, so no
     * profile-apply / AGC read interleaves the SCCB writes. */
    bool was_standby = atomic_load(&s_sensor_standby);
    if (was_standby) {
        sensor_t *ws = esp_camera_sensor_get();
        if (ws && ws->set_reg) ws->set_reg(ws, 0x3008, 0x40, 0x00);
        for (int d = 0; d < 4; d++) {
            camera_fb_t *junk = esp_camera_fb_get();
            if (junk) esp_camera_fb_return(junk);
            (void)esp_task_wdt_reset();
        }
    }
#endif

    /* Post-light-sleep resync: in the deep-save mode, light sleep gates the
     * LEDC camera XCLK while idle, so the first frame(s) after wake are torn
     * (garbage top rows — verified on the bench). Flush a few frames here so
     * the sensor/DMA re-sync before the real grab below. ~4 frames at UXGA is
     * a few hundred ms — acceptable in a survival posture; no-op in
     * Continuous/Triggered (no sleep there, so no corruption). */
    if (app_profile_sleeps()) {
        for (int d = 0; d < 4; d++) {
            camera_fb_t *junk = esp_camera_fb_get();
            if (junk) esp_camera_fb_return(junk);
            (void)esp_task_wdt_reset();
        }
    }

    status_led_capture_begin();
    capture_led_set(true);
    if (app_config_get_bool("cap_beep_en")) {
        /* SMB "coin" on whatever audio output is wired (buzzer chiptune + PDM
         * sample). No-op unless a pad is mapped to "buzzer" and/or "pcm". */
        audiofx_coin();
    }
    /* Bench OLED photo flash, fired HERE so it's in sync with the jingle and
     * only on a real capture event — not polled off camera_capture_count()
     * (which also ticks on every MJPEG stream frame). No-op when no panel. */
    oled_flash(OLED_FLASH_PHOTO);
    status_led_active = true;

    /* Stopwatch for end-to-end capture duration that ends up in EXIF
     * UserComment. Starts after the mutex is held so we measure work
     * we actually did, not the time we waited for a previous capture.
     * Includes IR warmup + throwaway + ASIC capture. */
    int64_t t_capture_start = esp_timer_get_time();

    /* Capture is the longest single operation a TWDT-subscribed task does.
     * Worst path: 500 ms mutex wait + 80 ms IR warm + 200 ms throwaway fb
     * + 500 ms main fb + 5 s slow-SD write + 2× MQTT publish + 400 ms IR
     * hold = ~7 s, well over the 10 s TWDT budget when the SD is flaky.
     * Pet the watchdog at each stage boundary so a single slow stage
     * doesn't reboot the board. esp_task_wdt_reset() no-ops with
     * ESP_ERR_NOT_FOUND when called from a non-subscribed task (HTTP
     * worker path via /capture), so the call is safe everywhere. */
    (void)esp_task_wdt_reset();

    /* Ambient-light gate: only fire IR when the sensor is gain-pinned.
     * Decision made before turning the LED on so the AGC reading reflects
     * actual ambient (the camera is grabbing frames in the background
     * between captures, AGC has settled). */
    int agc_observed = -1;
    bool fire_ir = ir_should_fire(&agc_observed);

    /* IR illumination: ramp on, settle, capture, hold, off. Skip the
     * warm-up / throwaway / hold when we're not firing — saves ~480 ms
     * per daytime shot and avoids a useless AGC perturbation. */
    if (fire_ir) {
        camera_ir_led_set(255);
        vTaskDelay(pdMS_TO_TICKS(IR_WARMUP_MS));

        /* Discard one frame after IR on so AEC/AWB adapts. */
        camera_fb_t *throwaway = esp_camera_fb_get();
        if (throwaway) esp_camera_fb_return(throwaway);
    }

    camera_fb_t *fb = camera_capture_fb();
    if (!fb) {
        if (status_led_active) status_led_capture_end();
        capture_led_set(false);
        if (fire_ir) camera_ir_led_set(0);
#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
        if (was_standby) {
            sensor_t *ws = esp_camera_sensor_get();
            if (ws && ws->set_reg) ws->set_reg(ws, 0x3008, 0x40, 0x40);
        }
#endif
        cb_pm_no_sleep_release();
        if (s_capture_mtx) xSemaphoreGive(s_capture_mtx);
        return ESP_FAIL;
    }
    (void)esp_task_wdt_reset();  /* pet after the main sensor blocking call */

    ESP_LOGI(TAG, "captured %ux%u %u bytes (trigger=%s, agc=%d ir=%s)",
             fb->width, fb->height, (unsigned)fb->len,
             trigger_reason ? trigger_reason : "unknown",
             agc_observed, fire_ir ? "on" : "off");

    /* Stamp EXIF metadata (which board, when, why) into the frame. The
     * stamp returns a STANDALONE heap buffer (original JPEG + EXIF APP1),
     * so once we hold it we can hand the driver frame buffer back and
     * release the sensor mutex BEFORE the slow SD/MQTT work below. On the
     * rare stamp failure, copy the raw frame so we still own a standalone
     * buffer to persist. */
    uint32_t this_seq = atomic_fetch_add(&s_capture_seq, 1) + 1;
    char mac_str[20] = {0};
    {
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(mac_str, sizeof(mac_str),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }

    jpeg_stamp_info_t stamp_info = {
        .mac        = mac_str,
        .trigger    = trigger_reason ? trigger_reason : "unknown",
        .sequence   = this_seq,
        .uptime_s   = (uint32_t)(esp_timer_get_time() / 1000000),
        .epoch_ms   = (int64_t)time(NULL) * 1000,
        .agc_gain   = agc_observed,
        .ir_active  = fire_ir,
        .capture_ms = (uint32_t)((esp_timer_get_time() - t_capture_start) / 1000),
    };
    uint8_t *out_buf = NULL;
    size_t   out_len = 0;
    {
        uint8_t *stamped = NULL;
        size_t stamped_len = jpeg_stamp_apply(fb->buf, fb->len,
                                              fb->width, fb->height,
                                              /*quality*/ 12,
                                              &stamp_info, &stamped);
        if (stamped_len) {
            out_buf = stamped;
            out_len = stamped_len;
        } else {
            /* Stamp failed (e.g. PSRAM tight) — copy the raw frame so we
             * can still release the fb + mutex before SD/MQTT. */
            out_buf = (uint8_t *)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
            if (!out_buf) out_buf = (uint8_t *)malloc(fb->len);
            if (out_buf) {
                memcpy(out_buf, fb->buf, fb->len);
                out_len = fb->len;
            }
            ESP_LOGW(TAG, "stamp skipped — copied raw frame");
        }
    }

    /* Read live sensor framesize/quality for the event payload while we
     * still hold the sensor. */
    int cur_fs = -1, cur_q = -1;
    {
        sensor_t *cs = esp_camera_sensor_get();
        if (cs) {
            cur_fs = (int)cs->status.framesize;
            cur_q  = (int)cs->status.quality;
        }
    }

    /* Sensor + frame work done — return the driver frame buffer, IR off,
     * and RELEASE the sensor mutex NOW. The SD write + MQTT publishes below
     * touch neither the sensor nor the fb, so holding the mutex across them
     * (the old behaviour) needlessly blocked a concurrent MJPEG stream's
     * profile-apply for the multi-second SD write → "stream profile apply
     * failed". The IR burst-hold is dropped too (it only saved an ~80 ms
     * re-warm on back-to-back shots, which are rare). */
    camera_release_fb(fb);
    if (fire_ir) camera_ir_led_set(0);
    capture_led_set(false);
    if (status_led_active) status_led_capture_end();
#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
    if (was_standby) {
        sensor_t *ws = esp_camera_sensor_get();
        if (ws && ws->set_reg) ws->set_reg(ws, 0x3008, 0x40, 0x40);  /* back to standby */
    }
#endif
    cb_pm_no_sleep_release();
    if (s_capture_mtx) xSemaphoreGive(s_capture_mtx);

    if (!out_buf) {
        ESP_LOGW(TAG, "capture: OOM securing image buffer — dropped");
        return ESP_ERR_NO_MEM;
    }

    /* ── Post-processing, OUTSIDE the sensor mutex ─────────────────────
     * Persist + publish on the standalone out_buf. Every sink here is
     * already individually thread-safe (FATFS volume lock, esp-mqtt's
     * internal lock, photo_queue's own mutex, s_last_mtx), so a concurrent
     * capture (cam_wrk vs the /capture HTTP path) is safe without holding
     * the sensor mutex across this slow work. */
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    const char *mac_tail = (mac_str[0]) ? (mac_str + 9) : "unknown";
    /* mac_str = "ee:eE:ee:tt:tt:tt" → tail starts at index 9 (after 3rd ':') */
    char mac_tag[12];
    {
        size_t mi = 0;
        for (const char *p = mac_tail; *p && mi < sizeof(mac_tag) - 1; ++p) {
            mac_tag[mi++] = (*p == ':') ? '_' : *p;
        }
        mac_tag[mi] = 0;
    }
    const char *trig = trigger_reason ? trigger_reason : "unk";

    /* Persist into the date-tree (/sdcard/YYYY-MM-DD/… or /sdcard/boot/…); the
     * full path written comes back in `path` for the MQTT event. */
    char path[128] = {0};
    bool sd_ok = false;
    if (sd_storage_ready()) {
        bool autoprune = app_config_get_bool("sd_autoprune");
        int  min_free  = (int)app_config_get_int("sd_min_free");
        int  keep_days = (int)app_config_get_int("sd_keep_days");

        esp_err_t we = sd_storage_write_capture(&tm_info, mac_tag, trig, this_seq,
                                                out_buf, out_len, path, sizeof(path));
        if (we != ESP_OK && autoprune) {
            /* Card likely full — free oldest buckets and retry once. 0 = default
             * (max) budget: this path already blocks the shot, so free fast. */
            sd_storage_autoprune(min_free, keep_days, 0);
            (void)esp_task_wdt_reset();
            we = sd_storage_write_capture(&tm_info, mac_tag, trig, this_seq,
                                          out_buf, out_len, path, sizeof(path));
        }
        if (we == ESP_OK) {
            sd_ok = true;
            ESP_LOGI(TAG, "saved %s (%u B)", path, (unsigned)out_len);
            /* Routine retention (autoprune) + legacy flat-root migration moved
             * OFF this worker to the supervisor loop (main.cpp). Pruning a deep
             * backlog of old day-folders deletes up to 300 files PER PASS — tens
             * of seconds on a tired card — and doing it inline here serialized
             * EVERY capture behind the prune → "mostly no photo yet" (verified:
             * disabling autoprune unstuck captures). The capture worker now only
             * grabs + stamps + writes + enqueues; maintenance runs decoupled on
             * the WDT-fed main loop, so a slow/backlogged prune never blocks a
             * shot. The full-card write-retry autoprune above stays inline (it's
             * the recovery that lets THIS shot land). */
        } else {
            ESP_LOGW(TAG, "SD save failed (card full?) for %s_%s", mac_tag, trig);
        }
        (void)esp_task_wdt_reset();
    }

    /* MQTT publish — ALWAYS via the async photo_queue, NEVER inline on this
     * worker. cam_wrk is WDT-subscribed; esp_mqtt_client_publish runs the send
     * on the CALLING task and can block well past network.timeout_ms (broker/
     * socket backpressure, or the MQTT API lock held by a stuck send) — long
     * enough to trip the 30 s task-WDT and reboot. That was the "captures wedge
     * after one shot → no photo yet" failure: the worker stuck in the inline
     * publish, never draining the next request. Enqueue is a bounded PSRAM
     * memcpy; the drain task does the event+image publish off the capture path
     * and retries across reconnects. Kick it so a connected board publishes
     * promptly (kick is a no-op when disconnected — drains on the next
     * MQTT_EVENT_CONNECTED). A frame > ENTRY_CAP_BYTES (400 KB — above the
     * driver's own UXGA JPEG fb bound, so it shouldn't happen for real
     * captures) is dropped from MQTT but is still on SD above. */
    {
        esp_err_t qe = photo_queue_enqueue(
            out_buf, out_len,
            trigger_reason ? trigger_reason : "unknown",
            sd_ok ? path : NULL,
            esp_timer_get_time(), this_seq,
            agc_observed, fire_ir, cur_fs, cur_q);
        if (qe == ESP_OK) {
            photo_queue_kick();
        } else if (qe == ESP_ERR_INVALID_SIZE) {
            ESP_LOGW(TAG, "photo %u too big for queue (%u B) — SD only",
                     (unsigned)this_seq, (unsigned)out_len);
        } else {
            ESP_LOGW(TAG, "photo_queue_enqueue: %s", esp_err_to_name(qe));
        }
    }
    (void)esp_task_wdt_reset();

    /* Refresh last-JPEG cache (PSRAM). HTTP /last.jpg + /capture read this. */
    if (s_last_mtx && xSemaphoreTake(s_last_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint8_t *new_buf = (uint8_t *)heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM);
        if (!new_buf) new_buf = (uint8_t *)malloc(out_len);
        if (new_buf) {
            memcpy(new_buf, out_buf, out_len);
            if (s_last_buf) free(s_last_buf);
            s_last_buf = new_buf;
            s_last_len = out_len;
        }
        xSemaphoreGive(s_last_mtx);
    }

    free(out_buf);
    return ESP_OK;
}

uint32_t camera_capture_count(void) {
    return atomic_load(&s_capture_count);
}
uint32_t camera_capture_failures(void) {
    return atomic_load(&s_capture_failures);
}

uint8_t *camera_last_jpeg_dup(size_t *out_len) {
    if (!s_last_mtx) return NULL;
    if (xSemaphoreTake(s_last_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return NULL;
    uint8_t *copy = NULL;
    size_t len = 0;
    if (s_last_buf && s_last_len > 0) {
        copy = (uint8_t *)heap_caps_malloc(s_last_len, MALLOC_CAP_SPIRAM);
        if (!copy) copy = (uint8_t *)malloc(s_last_len);
        if (copy) {
            memcpy(copy, s_last_buf, s_last_len);
            len = s_last_len;
        }
    }
    xSemaphoreGive(s_last_mtx);
    if (copy && out_len) *out_len = len;
    return copy;
}

bool camera_capture_dimensions(uint16_t *w, uint16_t *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    int fs = (int)app_config_get_int("cam_framesize");
    if (fs < 0 || fs >= FRAMESIZE_INVALID) return false;
    uint16_t rw = resolution[fs].width, rh = resolution[fs].height;
    if (!rw || !rh) return false;
    if (w) *w = rw;
    if (h) *h = rh;
    return true;
}

size_t camera_last_jpeg_peek_header(uint8_t *out, size_t cap) {
    if (!out || cap == 0 || !s_last_mtx) return 0;
    if (xSemaphoreTake(s_last_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return 0;
    size_t n = 0;
    if (s_last_buf && s_last_len > 0) {
        n = (s_last_len < cap) ? s_last_len : cap;
        memcpy(out, s_last_buf, n);
    }
    xSemaphoreGive(s_last_mtx);
    return n;
}

/* ── Capture worker (CPU1 prio 5) ──────────────────────────────────────
 *
 * Decouples camera_capture_event() (100–500 ms blocking, ~480 ms with
 * IR warmup) from the supervisor on CPU0. main loop calls
 * camera_request_event() which is non-blocking; the worker drains the
 * queue serially. Drop-oldest semantics: under a flood of VAD bursts
 * the freshest trigger still lands, and the discarded one is logged.
 *
 * Sized small (4 slots) on purpose — backlog longer than ~2 s (4 × IR
 * shot) wouldn't represent reality anyway; better to drop and log than
 * to grow a stale queue. */
typedef struct {
    char trigger[16];
} cap_req_t;

#define CAP_QUEUE_DEPTH 4
static QueueHandle_t s_capture_q       = NULL;
static TaskHandle_t  s_worker_handle   = NULL;
static atomic_uint_fast32_t s_dropped_total = 0;
static int64_t       s_last_drop_log_us = 0;

esp_err_t camera_request_event(const char *trigger_reason) {
    if (!s_capture_q) return ESP_FAIL;
    cap_req_t req;
    size_t n = strnlen(trigger_reason ? trigger_reason : "", sizeof(req.trigger) - 1);
    memcpy(req.trigger, trigger_reason ? trigger_reason : "", n);
    req.trigger[n] = '\0';

    if (xQueueSend(s_capture_q, &req, 0) == pdTRUE) {
        return ESP_OK;
    }

    /* Queue full — drop oldest to make room for the freshest trigger.
     * Discarded entry counted into s_dropped_total; log at most once per
     * minute so a sustained burst doesn't spam the UART. */
    cap_req_t discard;
    if (xQueueReceive(s_capture_q, &discard, 0) == pdTRUE) {
        atomic_fetch_add(&s_dropped_total, 1);
        int64_t now = esp_timer_get_time();
        if (now - s_last_drop_log_us > 60LL * 1000 * 1000) {
            s_last_drop_log_us = now;
            ESP_LOGW(TAG,
                     "request queue full; dropped oldest='%s' newest='%s' "
                     "(total dropped=%" PRIu32 ")",
                     discard.trigger, req.trigger,
                     (uint32_t)atomic_load(&s_dropped_total));
        }
    }
    if (xQueueSend(s_capture_q, &req, 0) != pdTRUE) {
        return ESP_FAIL;  // shouldn't happen — we just made room
    }
    return ESP_OK;
}

static void camera_worker_task(void *unused) {
    (void)unused;
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "esp_task_wdt_add(cam_wrk) failed — worker not WDT-protected");
    }
    ESP_LOGI(TAG, "worker started on CPU%d prio %u (queue capacity %d)",
             xPortGetCoreID(),
             (unsigned)uxTaskPriorityGet(NULL),
             CAP_QUEUE_DEPTH);
    while (true) {
        esp_task_wdt_reset();
        cap_req_t req;
        /* 10 s receive timeout = WDT_TIMEOUT_S/3 → 3 strikes before panic. */
        if (xQueueReceive(s_capture_q, &req,
                          pdMS_TO_TICKS(10000)) == pdTRUE) {
            (void)camera_capture_event(req.trigger);
        }
    }
}

void camera_worker_start(void) {
    if (s_worker_handle) return;  // idempotent
    if (!s_capture_q) {
        s_capture_q = xQueueCreate(CAP_QUEUE_DEPTH, sizeof(cap_req_t));
        if (!s_capture_q) {
            ESP_LOGE(TAG, "xQueueCreate(capture) failed — worker not started");
            return;
        }
    }
    BaseType_t ok = xTaskCreatePinnedToCore(
        camera_worker_task, "cam_wrk", 6144, NULL, /*prio*/ 5,
        &s_worker_handle, /*core*/ 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(cam_wrk) failed: %d", (int)ok);
        s_worker_handle = NULL;
    }
}

bool camera_worker_running(void) {
    return s_worker_handle != NULL && s_capture_q != NULL;
}

uint32_t camera_request_drops_total(void) {
    return (uint32_t)atomic_load(&s_dropped_total);
}
