/* speaker.c — see speaker.h. LEDC square-wave beeper on the "buzzer" pad. */
#include "speaker.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "speaker";

/* LEDC allocation — must avoid IR LED (timer/channel 0, see camera.c) and the
 * esp32-camera XCLK (timer/channel 1, see camera.c:574). Timer 2 / channel 2
 * are free. ESP32-S3 has only LOW_SPEED_MODE. */
#define SPK_MODE        LEDC_LOW_SPEED_MODE
#define SPK_TIMER       LEDC_TIMER_2
#define SPK_CHANNEL     LEDC_CHANNEL_2
#define SPK_RES         LEDC_TIMER_10_BIT
#define SPK_DUTY_HALF   (1u << (10 - 1))   /* 50 % → clean square wave */
#define SPK_DUTY_FULL   (1u << 10)         /* 100 % → constant high (DC-on) */

/* Frequency clamp: 10-bit res @ 80 MHz src tops out ~78 kHz; keep well inside
 * the audible/achievable band so ledc_set_freq() never fails. */
#define SPK_FREQ_MIN    50u
#define SPK_FREQ_MAX    12000u

#define SPK_MAX_NOTES   64    /* per melody; longer requests are truncated */
#define SPK_QUEUE_DEPTH 3
/* On a fixed-pitch (active) buzzer the only carrier of melody is rhythm, so
 * adjacent on-notes need a brief silence between them — otherwise they merge
 * into one continuous tone. Tone mode conveys notes by pitch and needs none. */
#define SPK_BUZZER_GAP_MS 28

typedef struct {
    speaker_note_t *notes;   /* heap copy, freed by the task after playback */
    size_t          n;
    bool            loop;    /* repeat until s_stop is raised */
    bool            legato;  /* tone mode: skip the staccato carve (smooth SFX) */
} spk_req_t;

static bool          s_inited  = false;
static int           s_pin     = -1;     /* resolved buzzer GPIO once inited, else -1 */
static QueueHandle_t s_q       = NULL;
static atomic_bool   s_stop    = false;  /* raised by speaker_stop()/speaker_loop() */
static atomic_bool   s_in_loop = false;  /* a loop (alarm) owns the channel now */

static inline void spk_silence(void) {
    ledc_set_duty(SPK_MODE, SPK_CHANNEL, 0);
    ledc_update_duty(SPK_MODE, SPK_CHANNEL);
}

/* Drive one note on the LEDC channel. f == 0 → rest (silence). `tone` true =
 * passive transducer (LEDC freq follows the note); false = active buzzer. */
static void spk_render(bool tone, uint16_t f) {
    if (f == 0) {
        spk_silence();                     /* rest */
    } else if (tone) {
        if (f < SPK_FREQ_MIN) f = SPK_FREQ_MIN;
        if (f > SPK_FREQ_MAX) f = SPK_FREQ_MAX;
        if (ledc_set_freq(SPK_MODE, SPK_TIMER, f) != ESP_OK)
            ESP_LOGW(TAG, "set_freq %u Hz failed", f);
        ledc_set_duty(SPK_MODE, SPK_CHANNEL, SPK_DUTY_HALF);
        ledc_update_duty(SPK_MODE, SPK_CHANNEL);
    } else {
        /* Active buzzer: drive the pad full-on; the element makes its own
         * fixed tone. freq_hz only matters as on(>0)/rest(0). */
        ledc_set_duty(SPK_MODE, SPK_CHANNEL, SPK_DUTY_FULL);
        ledc_update_duty(SPK_MODE, SPK_CHANNEL);
    }
}

static void speaker_task(void *arg) {
    (void)arg;
    spk_req_t r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;
        atomic_store(&s_stop, false);   /* this request owns the channel now */
        atomic_store(&s_in_loop, r.loop);   /* loops own it; one-shots are dropped */

        /* Mode read once per request; an MQTT flip applies to the next one. */
        bool tone = app_config_get_bool("spkr_tone");

        bool aborted = false;
        do {
            for (size_t i = 0; i < r.n; i++) {
                if (atomic_load(&s_stop)) { aborted = true; break; }
                uint16_t f  = r.notes[i].freq_hz;
                uint16_t ms = r.notes[i].ms;
                spk_render(tone, f);
                if (f == 0) {
                    if (ms) vTaskDelay(pdMS_TO_TICKS(ms));   /* rest */
                } else if (tone && r.legato) {
                    /* Smooth SFX (coin / power-up sweep): no staccato carve so
                     * the notes glide together instead of chopping. */
                    if (ms) vTaskDelay(pdMS_TO_TICKS(ms));
                } else if (tone) {
                    /* Staccato: carve a short silence out of each note so the
                     * melody articulates instead of slurring into one flat
                     * drone. Tempo is preserved (gap comes out of the note). */
                    uint16_t gap = ms / 6;
                    if (gap < 12) gap = 12;
                    if (gap > 30) gap = 30;
                    uint16_t on = ms > gap ? (uint16_t)(ms - gap) : ms;
                    vTaskDelay(pdMS_TO_TICKS(on));
                    spk_silence();
                    vTaskDelay(pdMS_TO_TICKS(gap));
                } else {
                    /* Active buzzer (fixed pitch): full-length note, then a gap
                     * so adjacent notes don't merge into one continuous tone. */
                    if (ms) vTaskDelay(pdMS_TO_TICKS(ms));
                    spk_silence();
                    vTaskDelay(pdMS_TO_TICKS(SPK_BUZZER_GAP_MS));
                }
            }
        } while (r.loop && !aborted && !atomic_load(&s_stop));

        atomic_store(&s_in_loop, false);
        spk_silence();
        free(r.notes);
    }
}

void speaker_init(void) {
    if (s_inited) return;

    /* Pin-map lookup, exactly like ir_led/capture_led in camera.c. When no
     * slot carries "buzzer" the module stays idle — the safe default, and the
     * reason the field board is untouched until a pad is deliberately mapped. */
    int pin = app_config_pin_for_first("buzzer");
    if (pin < 0) {
        ESP_LOGI(TAG, "no GPIO mapped to 'buzzer' in pin map — speaker idle");
        return;
    }

    ledc_timer_config_t tcfg = {
        .speed_mode      = SPK_MODE,
        .timer_num       = SPK_TIMER,
        .duty_resolution = SPK_RES,
        .freq_hz         = 2000,           /* placeholder; set per note in tone mode */
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&tcfg) != ESP_OK) {
        ESP_LOGW(TAG, "ledc_timer_config failed — speaker disabled");
        return;
    }

    ledc_channel_config_t ccfg = {
        .gpio_num   = pin,
        .speed_mode = SPK_MODE,
        .channel    = SPK_CHANNEL,
        .timer_sel  = SPK_TIMER,
        .duty       = 0,
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    if (ledc_channel_config(&ccfg) != ESP_OK) {
        ESP_LOGW(TAG, "ledc_channel_config failed (GPIO%d) — speaker disabled", pin);
        return;
    }

    s_q = xQueueCreate(SPK_QUEUE_DEPTH, sizeof(spk_req_t));
    if (!s_q) {
        ESP_LOGE(TAG, "queue alloc failed — speaker disabled");
        return;
    }
    if (xTaskCreate(speaker_task, "speaker", 2560, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed — speaker disabled");
        vQueueDelete(s_q);
        s_q = NULL;
        return;
    }

    s_pin = pin;
    s_inited = true;
    ESP_LOGI(TAG, "buzzer on GPIO%d (mode=%s)", pin,
             app_config_get_bool("spkr_tone") ? "tone" : "buzzer");
    /* Boot SFX is fired from app_main via audiofx_boot() once both this and
     * pcm_init() are up, so the power-up jingle plays on both outputs. */
}

static void spk_enqueue(const speaker_note_t *notes, size_t n, bool loop, bool legato) {
    if (!s_inited || !s_q || !notes || n == 0) return;
    /* Don't queue one-shots behind an endless loop (alarm) — they'd burst
     * out when it stops. The loop owns the channel until speaker_stop(). */
    if (!loop && atomic_load(&s_in_loop)) return;
    if (n > SPK_MAX_NOTES) n = SPK_MAX_NOTES;

    speaker_note_t *copy = malloc(n * sizeof(*copy));
    if (!copy) return;
    memcpy(copy, notes, n * sizeof(*copy));

    spk_req_t r = { .notes = copy, .n = n, .loop = loop, .legato = legato };
    if (xQueueSend(s_q, &r, 0) != pdTRUE) {
        free(copy);   /* queue full — drop rather than block the caller */
        ESP_LOGW(TAG, "play queue full — dropping %u-note request", (unsigned)n);
    }
}

void speaker_beep(uint16_t freq_hz, uint16_t ms) {
    speaker_note_t n = { freq_hz, ms };
    spk_enqueue(&n, 1, false, false);
}

void speaker_play(const speaker_note_t *notes, size_t n) {
    spk_enqueue(notes, n, false, false);
}

void speaker_sfx(const speaker_note_t *notes, size_t n) {
    spk_enqueue(notes, n, false, true);   /* legato — smooth, no staccato */
}

void speaker_loop(const speaker_note_t *notes, size_t n) {
    atomic_store(&s_stop, true);   /* end any running loop before taking over */
    spk_enqueue(notes, n, true, false);
}

void speaker_stop(void) {
    atomic_store(&s_stop, true);
}

int speaker_gpio(void) {
    return s_inited ? s_pin : -1;
}
