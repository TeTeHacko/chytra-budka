/* pcm.c — see pcm.h. I2S PDM TX (PCM2PDM) sample + tone player = "1-bit DAC". */
#include "pcm.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "pcm";

#define PCM_RATE_HZ      16000
#define PCM_QUEUE_DEPTH  3
#define PCM_MAX_NOTES    64       /* per tone request; longer is truncated */
#define PCM_SYNTH_CHUNK  256      /* samples per i2s_write while synthesising */
#define PCM_TONE_AMP     32767    /* full-scale square — loudest the PDM can drive.
                                   * A square pins the modulator to the rails (no
                                   * sigma-delta instability), so the RC output swings
                                   * ~rail-to-rail, same as the LEDC buzzer pin. Any
                                   * remaining loudness gap vs the buzzer is the analog
                                   * front-end (RC series R + a gainless follower), not
                                   * the digital level — that's maxed here. */
#define TWO_PI           6.2831853f
#define PI               3.14159265f
/* PDM data-out GPIO comes from the pin map ("pcm" function) — hook the RC
 * filter there. clk is left unused: a sigma-delta + RC filter only needs dout. */

/* Flash-embedded samples (CMakeLists EMBED_FILES). 16-bit LE mono @16 kHz. */
extern const uint8_t coin_pcm_start[] asm("_binary_coin_pcm_start");
extern const uint8_t coin_pcm_end[]   asm("_binary_coin_pcm_end");

/* A request is EITHER a ready sample buffer (samples != NULL) OR a note list
 * to synthesise (notes != NULL, a heap copy the task frees after playback). */
typedef struct {
    const int16_t  *samples;
    size_t          n;
    speaker_note_t *notes;
    size_t          nnotes;
    bool            legato;
    bool            loop;
} pcm_req_t;

static bool              s_inited = false;
static int               s_pin    = -1;    /* resolved pcm GPIO once inited, else -1 */
static i2s_chan_handle_t s_tx     = NULL;
static QueueHandle_t     s_q      = NULL;
static atomic_bool       s_stop    = false;  /* end a looping/long clip */
static atomic_bool       s_in_loop = false;  /* a loop (alarm/test) owns the output now */
static int16_t           s_tone[160];        /* 1 kHz sine RC-tuning test buffer */

/* Write `nsamp` samples of a SQUARE wave at `f` Hz (f == 0 → silence) to the
 * channel, keeping phase continuous via *phase. Returns false if stopped
 * mid-way. Square (not sine) because it's the chiptune sound, and far louder
 * through a small speaker: +3 dB RMS plus odd harmonics that land where a tiny
 * transducer is efficient — a pure low/mid sine is nearly inaudible on one. */
static bool pcm_write_tone(uint16_t f, size_t nsamp, float *phase) {
    static int16_t buf[PCM_SYNTH_CHUNK];
    const float dphi = f ? TWO_PI * (float)f / (float)PCM_RATE_HZ : 0.0f;
    while (nsamp) {
        if (atomic_load(&s_stop)) return false;
        size_t chunk = nsamp < PCM_SYNTH_CHUNK ? nsamp : PCM_SYNTH_CHUNK;
        for (size_t i = 0; i < chunk; i++) {
            if (f == 0) {
                buf[i] = 0;
            } else {
                buf[i] = (*phase < PI) ? (int16_t)PCM_TONE_AMP : (int16_t)(-PCM_TONE_AMP);
                *phase += dphi;
                if (*phase >= TWO_PI) *phase -= TWO_PI;
            }
        }
        size_t wrote = 0;
        if (i2s_channel_write(s_tx, buf, chunk * sizeof(int16_t), &wrote,
                              portMAX_DELAY) != ESP_OK)
            return false;
        nsamp -= chunk;
    }
    return true;
}

/* Render one note list once (staccato carve unless legato, matching speaker.c
 * so both outputs articulate identically). Returns false if aborted by stop. */
static bool pcm_synth_pass(const pcm_req_t *r, float *phase) {
    for (size_t i = 0; i < r->nnotes; i++) {
        if (atomic_load(&s_stop)) return false;
        uint16_t f  = r->notes[i].freq_hz;
        uint16_t ms = r->notes[i].ms;
        size_t total = (size_t)ms * PCM_RATE_HZ / 1000;
        if (f == 0 || r->legato) {
            if (!pcm_write_tone(f, total, phase)) return false;   /* rest or smooth */
        } else {
            /* Staccato: carve a short silence out of each note (gap = ms/6,
             * clamped 12..30 ms) so the melody articulates instead of slurring. */
            uint16_t gap = ms / 6;
            if (gap < 12) gap = 12;
            if (gap > 30) gap = 30;
            uint16_t on = ms > gap ? (uint16_t)(ms - gap) : ms;
            if (!pcm_write_tone(f, (size_t)on * PCM_RATE_HZ / 1000, phase)) return false;
            if (!pcm_write_tone(0, (size_t)gap * PCM_RATE_HZ / 1000, phase)) return false;
        }
    }
    return true;
}

static void pcm_task(void *arg) {
    (void)arg;
    static const int16_t tail[64] = {0};   /* flush DMA cleanly after a clip */
    pcm_req_t r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;
        if (!r.samples && !r.notes) { free(r.notes); continue; }
        atomic_store(&s_stop, false);

        esp_err_t err = i2s_channel_enable(s_tx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s enable failed: %s", esp_err_to_name(err));
            free(r.notes);
            continue;
        }
        atomic_store(&s_in_loop, r.loop);   /* loops own the output; one-shots are dropped */

        if (r.notes) {
            ESP_LOGI(TAG, "play %u tones%s", (unsigned)r.nnotes, r.loop ? " (loop)" : "");
            float phase = 0.0f;
            bool ok;
            do {
                ok = pcm_synth_pass(&r, &phase);
            } while (ok && r.loop && !atomic_load(&s_stop));
        } else {
            ESP_LOGI(TAG, "play %u samples%s", (unsigned)r.n, r.loop ? " (loop)" : "");
            size_t wrote = 0;
            do {
                err = i2s_channel_write(s_tx, r.samples, r.n * sizeof(int16_t),
                                        &wrote, portMAX_DELAY);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(err));
                    break;
                }
            } while (r.loop && !atomic_load(&s_stop));
        }

        atomic_store(&s_in_loop, false);
        size_t wrote = 0;
        i2s_channel_write(s_tx, tail, sizeof(tail), &wrote, pdMS_TO_TICKS(100));
        i2s_channel_disable(s_tx);   /* idle the pin between clips */
        free(r.notes);               /* NULL-safe for sample requests */
    }
}

void pcm_init(void) {
    if (s_inited) return;

    int pin = app_config_pin_for_first("pcm");
    if (pin < 0) {
        ESP_LOGI(TAG, "no GPIO mapped to 'pcm' in pin map — pcm idle");
        return;
    }

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan, &s_tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed — pcm disabled");
        return;
    }

    /* CODEC one-line mode (data_fmt = PCM): runs the hardware PCM→PDM
     * sigma-delta modulator, so the 16-bit samples we write are emitted as a
     * properly modulated 1-bit PDM bitstream that an external RC low-pass
     * reconstructs to clean analog — the DIY 1-bit DAC.
     *
     * NOT "DAC line mode": that one uses data_fmt = RAW, which DISABLES the
     * PCM2PDM converter (i2s_pdm.c sets pcm2pdm_conv_en = (data_fmt==PCM)) and
     * expects pre-encoded raw PDM bits. Feeding PCM samples into RAW mode just
     * clocks the int16 words out bit-for-bit, so the RC averages them into a
     * faint, distorted, popcount-correlated ghost — loud-but-wrong's quiet
     * cousin. (This was the original "no audio / náznak zvuku" bug.)
     *
     * The internal BCLK still clocks the data out; we just don't route it to a
     * pad (clk unused) — an RC-filter DAC needs only dout. */
    i2s_pdm_tx_config_t cfg = {
        .clk_cfg  = I2S_PDM_TX_CLK_DEFAULT_CONFIG(PCM_RATE_HZ),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                   I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk  = I2S_GPIO_UNUSED,    /* RC filter only needs dout */
            .dout = (gpio_num_t)pin,
            .invert_flags = { .clk_inv = 0 },
        },
    };
    if (i2s_channel_init_pdm_tx_mode(s_tx, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "pdm_tx init failed — pcm disabled");
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return;
    }

    s_q = xQueueCreate(PCM_QUEUE_DEPTH, sizeof(pcm_req_t));
    if (!s_q) {
        ESP_LOGE(TAG, "queue alloc failed — pcm disabled");
        return;
    }
    if (xTaskCreate(pcm_task, "pcm", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed — pcm disabled");
        vQueueDelete(s_q);
        s_q = NULL;
        return;
    }

    s_pin = pin;
    s_inited = true;
    ESP_LOGI(TAG, "PDM TX on GPIO%d @%d Hz (PCM2PDM) — DIY 1-bit DAC",
             pin, PCM_RATE_HZ);
}

static void pcm_enqueue(pcm_req_t r) {
    if (!s_inited || !s_q) { free(r.notes); return; }
    /* While a loop (alarm/test) owns the output, drop one-shots instead of
     * queuing them behind an endless loop — otherwise capture coins from PIR/VAD
     * photos pile up and burst out when the loop finally stops. */
    if (!r.loop && atomic_load(&s_in_loop)) { free(r.notes); return; }
    if (r.loop) atomic_store(&s_stop, true);   /* end any running loop first */
    if (xQueueSend(s_q, &r, 0) != pdTRUE) {
        ESP_LOGW(TAG, "play queue full — dropping clip");
        free(r.notes);
    }
}

void pcm_play(const int16_t *samples, size_t n) {
    if (!samples || n == 0) return;
    pcm_enqueue((pcm_req_t){ .samples = samples, .n = n });
}

void pcm_play_tones(const speaker_note_t *notes, size_t n, bool legato, bool loop) {
    if (!s_inited || !notes || n == 0) return;
    if (n > PCM_MAX_NOTES) n = PCM_MAX_NOTES;
    speaker_note_t *copy = malloc(n * sizeof(*copy));
    if (!copy) return;
    memcpy(copy, notes, n * sizeof(*copy));
    pcm_enqueue((pcm_req_t){ .notes = copy, .nnotes = n, .legato = legato, .loop = loop });
}

void pcm_stop(void) {
    atomic_store(&s_stop, true);
}

int pcm_gpio(void) {
    return s_inited ? s_pin : -1;
}

bool pcm_play_named(const char *name) {
    if (!name) return false;
    if (strcmp(name, "coin") == 0) {
        pcm_play((const int16_t *)coin_pcm_start,
                 (size_t)(coin_pcm_end - coin_pcm_start) / sizeof(int16_t));
        return true;
    }
    if (strcmp(name, "test") == 0) {
        /* 1 kHz SINE @16 kHz (16 samples/period), ~0.88 FS — a clean tone to
         * tune the RC by ear; loops until cmd/pcm stop. (A square would rasp:
         * its harmonics are what the filter is meant to remove.) */
        size_t tn = sizeof(s_tone) / sizeof(s_tone[0]);
        for (size_t i = 0; i < tn; i++)
            s_tone[i] = (int16_t)(29000.0f * sinf(TWO_PI * (float)i / 16.0f));
        pcm_enqueue((pcm_req_t){ .samples = s_tone, .n = tn, .loop = true });
        return true;
    }
    if (strcmp(name, "stop") == 0 || strcmp(name, "off") == 0) {
        pcm_stop();
        return true;
    }
    return false;
}
