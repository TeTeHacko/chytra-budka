// audio.cpp — onboard PDM mic capture → cb::Vad → cb::ChunkedPoster → relay.
//
// rev3.2: switched from external INMP441 (I²S Std) to the XIAO Sense
// onboard MSM261D3526H1CPM (I²S PDM) so GPIO 7/8/9 free up for the
// SDIO 1-bit microSD slot used by the camera module.
//
// PDM RX is configured STEREO + slot_mask = SLOT_BOTH on line 0
// (see audio_begin) because that is the only combination the IDF I²S
// driver actually pulls a full DMA descriptor through for this MEMS
// mic. The two PDM slots end up carrying the same sample (the mic
// mirrors itself), so the wire format is 2-channel L16 at the
// configured I2S_SAMPLE_RATE. The relay extracts mono on its side.
//
// Codec selection (NVS key `flac_enabled`, default OFF):
//   OFF: POST audio/L16; rate=<I2S_SAMPLE_RATE>; channels=2 — raw PCM
//   ON : POST audio/flac                                    — libFLAC
//                                                             stream
// FLAC is silently no-op'd back to PCM when libFLAC isn't vendored
// (cb::FlacEncoder::begin() returns false → we log + fall back).

#include "audio.h"

#include "cb_pm.h"

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_main_exports.h"
#include "cb/chunked_poster.h"
#include "cb/flac_encoder.h"
#include "cb/mode_fsm.h"
#include "cb/vad.h"
#include "config.h"
#include "device_id.h"
#include "driver/i2s_pdm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "mqtt.h"
#include "secret_helpers.h"
#include "transport_idf.h"

static const char *TAG = "audio";

namespace {

// MONO PDM RX: dma_frame_num=256 samples × 2 B = 512 B per descriptor.
// Ask for 2 descriptors per read with 200 ms timeout — the IDF I2S driver
// only releases the previous descriptor when the next one is being
// filled, so requesting just one would block forever waiting for the
// next-trigger; requesting two unblocks at the same rate without losing
// data. 512 samples / 16 kHz = 32 ms per audio task tick.
constexpr size_t FRAME_SAMPLES = 512;
constexpr size_t FRAME_BYTES_S16 = FRAME_SAMPLES * sizeof(int16_t);

i2s_chan_handle_t s_rx = nullptr;

/* True while we hold the NO_LIGHT_SLEEP lock for the running I2S DMA. Keeps
 * the acquire/release balanced across idempotent begin / repeated end calls
 * (Phase 2; no-op unless CONFIG_PM_ENABLE + pm_lightsleep). */
bool s_pm_held = false;

// Static frame buffer — avoid heap churn in hot loop.
int16_t s_buf16[FRAME_SAMPLES];

std::unique_ptr<cb::Vad> s_vad;
std::unique_ptr<cb::IdfTransport> s_tx;
std::unique_ptr<cb::ChunkedPoster> s_poster;
std::unique_ptr<cb::FlacEncoder> s_flac;
bool s_using_flac = false;  // codec actually negotiated for current stream

/* s_streaming used to be a plain bool with a separate atomic mirror
 * (s_streaming_atomic) for the cross-core getter. Two sources of truth
 * is drift waiting to happen; collapse into one atomic. Audio task is
 * the only writer (open_stream/close_stream); telemetry on CPU0 reads
 * via audio_streaming(). Internal audio-task self-reads use
 * memory_order_relaxed — same-core, monotonic, no ordering needed. */
std::atomic<bool> s_streaming{false};
/* Cross-core counter — incremented in audio_task (CPU1) per relay frame,
 * read by telemetry on CPU0 via audio_chunks_sent(). Atomic with
 * relaxed ordering because the value is a monotonic counter and any
 * single torn read can't violate "always grows". */
std::atomic<uint32_t> s_chunks_sent{0};
std::atomic<bool> s_vad_capture_req{false};
std::atomic<bool> s_vad_reconfig_req{false};

/* Cross-core read snapshots — the audio task on CPU1 publishes these,
 * supervisor/telemetry on CPU0 reads them. Direct access to s_vad from
 * another core is a race; these atomics are the publish point. */
std::atomic<float> s_last_rms_dbfs_atomic{-120.0f};
std::atomic<uint32_t> s_burst_count_atomic{0};

/* Single-instance guard for audio_task_start(). */
TaskHandle_t s_audio_task_handle = nullptr;

/* Deferred lifecycle request — audio_task on CPU1 owns s_rx + s_vad
 * lifetime. The supervisor (CPU0) on a mode transition can't directly
 * call i2s_del_channel / s_vad.reset() because audio_task may be
 * mid-pump holding stale pointers. Instead it posts a request here;
 * audio_task picks it up at the top of its loop, between pumps, so
 * no concurrent access is possible. Values: 0 none, 1 begin, 2 end. */
constexpr int LIFECYCLE_NONE  = 0;
constexpr int LIFECYCLE_BEGIN = 1;
constexpr int LIFECYCLE_END   = 2;
std::atomic<int> s_lifecycle_target{LIFECYCLE_NONE};

// Snapshot ring: live mono (left-channel) audio so HTTP can stream
// the last few seconds without disrupting the VAD/relay pump.
// Sized for ~3 s at I2S_SAMPLE_RATE.
constexpr size_t SNAPSHOT_RING_SAMPLES = (size_t)(I2S_SAMPLE_RATE * 3);
int16_t *s_ring = nullptr;
std::atomic<uint32_t> s_ring_write_pos{0};   // monotonic; modulo for indexing
std::atomic<uint32_t> s_frames_captured{0};

SemaphoreHandle_t s_ring_lock = nullptr;

// Reconfigure VAD on the audio task to avoid races with audio_pump_*
// which also touches s_vad. Called at the top of every pump iteration.
void apply_vad_reconfig_if_pending() {
    if (!s_vad_reconfig_req.exchange(false))
        return;
    cb::Vad::Config vc;
    vc.threshold_dbfs = app_config_get_float("vad_thr_dbfs");
    vc.window_samples = 960;
    vc.burst_ms = (uint32_t)app_config_get_int("vad_burst_ms");
    vc.rearm_ms = (uint32_t)app_config_get_int("vad_rearm_ms");
    // VAD counts int16 values per second to convert burst_ms/rearm_ms
    // into sample counts. Our buffer is STEREO L,R-interleaved (the
    // SLOT_BOTH workaround), so the effective int16-per-second rate
    // fed to vad->update() is twice I2S_SAMPLE_RATE. Without this the
    // burst window expires twice as fast in wall-clock time.
    vc.sample_rate = (uint32_t)(I2S_SAMPLE_RATE * 2);
    /* Atomic ownership swap: the new object is fully constructed and
     * assigned before the prior one is destroyed (unique_ptr move-assign
     * deletes the previous holdee). The audio pump task is the sole
     * dereferencer of s_vad, so no read-side race. */
    s_vad = std::make_unique<cb::Vad>(vc);
    ESP_LOGI(TAG, "VAD reconfigured: thr=%.1f dBFS burst=%u ms rearm=%u ms", vc.threshold_dbfs,
             (unsigned)vc.burst_ms, (unsigned)vc.rearm_ms);
}

inline uint32_t now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// FLAC encoder write callback: forward encoded bytes straight to the
// chunked poster as a single HTTP chunk. libFLAC may emit several KB at
// once (frame finalize) or a few hundred bytes; the poster handles any
// non-zero size. Returning false aborts the encode — we use that to
// propagate a TCP failure back through the encoder so the next process()
// call returns false and we tear down the stream cleanly.
bool flac_write_cb(const uint8_t *data, size_t bytes, void *user) {
    auto *poster = static_cast<cb::ChunkedPoster *>(user);
    if (!poster->send(data, bytes)) {
        ESP_LOGW(TAG, "relay: FLAC chunk send failed (%u B)", static_cast<unsigned>(bytes));
        return false;
    }
    return true;
}

// Treat the secrets.h placeholder bearer as "not configured" — skip
// the entire stream open path so we don't burn TCP connects against a
// relay that will reject us, and so WiFi stays quiet while the HW
// bring-up is unfinished. Set a real RELAY_AUTH in secrets.h to enable.
bool relay_auth_is_placeholder() {
    return secret_is_placeholder(RELAY_AUTH);
}

// Build a per-device relay path so the relay can keep multiple boards
// on separate streams even if they share the same RELAY_HOST. Suffix
// comes from device_id_suffix() (e.g. "ex02") so it matches the MQTT
// client_id / hostname suffix exactly. Result cached after first build.
const char *relay_path_for_this_device() {
    static char path[80];
    static bool built = false;
    if (built) return path;
    snprintf(path, sizeof(path), "%s-%s", RELAY_PATH, device_id_suffix());
    built = true;
    return path;
}

/* (Re)create s_tx + s_poster and open a chunked POST to the relay with
 * the given content type. On failure leaves both unique_ptrs reset.
 * Order matters: s_poster references *s_tx, so we drop the poster
 * before the transport. */
bool setup_poster(const char *content_type) {
    s_poster.reset();
    s_tx = std::make_unique<cb::IdfTransport>();
    s_poster = std::make_unique<cb::ChunkedPoster>(*s_tx);
    if (!s_poster->begin(RELAY_HOST, RELAY_PORT, relay_path_for_this_device(),
                         content_type, RELAY_AUTH)) {
        s_poster.reset();
        s_tx.reset();
        return false;
    }
    return true;
}

bool open_stream() {
    if (s_streaming.load(std::memory_order_relaxed))
        return true;

    if (relay_auth_is_placeholder()) {
        static bool s_warned = false;
        if (!s_warned) {
            ESP_LOGW(TAG, "relay: disabled (RELAY_AUTH placeholder); audio send "
                          "suppressed until a real bearer is set in secrets.h");
            s_warned = true;
        }
        return false;
    }

    // Exponential backoff on repeated relay connect failures to avoid
    // spamming hundreds of DNS lookups + TCP connects per second.
    static TickType_t s_next_attempt = 0;
    static uint32_t s_backoff_ms = 0;
    TickType_t now_tick = xTaskGetTickCount();
    if (now_tick < s_next_attempt)
        return false;

    // Decide codec for this burst at open() time. We snapshot once so the
    // stream content-type and the bytes we put on the wire stay consistent
    // for the entire connection even if the operator flips the toggle
    // mid-burst. Effective on next open_stream() call.
    bool want_flac = app_config_get_bool("flac_enabled");
    // PCM Content-Type is derived from I2S_SAMPLE_RATE + 2 channels so
    // the relay sees the correct rate/channels for ffmpeg. Stereo here
    // because of the SLOT_BOTH workaround (see file header).
    static char pcm_ct[48];
    snprintf(pcm_ct, sizeof(pcm_ct),
             "audio/L16; rate=%d; channels=2", (int)I2S_SAMPLE_RATE);

    if (!setup_poster(want_flac ? "audio/flac" : pcm_ct)) {
        static uint32_t s_conn_err_count = 0;
        if (++s_conn_err_count <= 3 || (s_conn_err_count % 50) == 0) {
            ESP_LOGW(TAG, "relay: connect/headers failed (count=%" PRIu32 ")", s_conn_err_count);
        }
        // Backoff: 500ms → 1s → 2s → 4s → 5s (cap)
        s_backoff_ms = s_backoff_ms == 0 ? 500 : (s_backoff_ms < 5000 ? s_backoff_ms * 2 : 5000);
        s_next_attempt = now_tick + pdMS_TO_TICKS(s_backoff_ms);
        return false;
    }

    // Spin up the FLAC encoder if requested. If begin() returns false
    // (libFLAC stub or alloc failure), fall back to PCM transparently for
    // the remainder of this stream. The poster is already framed as
    // audio/flac so we'd be sending PCM bytes under a FLAC header — better
    // to abort and reconnect as PCM.
    s_using_flac = false;
    if (want_flac) {
        if (!s_flac)
            s_flac = std::make_unique<cb::FlacEncoder>();
        cb::FlacEncoder::Config fc;
        fc.sample_rate = I2S_SAMPLE_RATE;
        fc.channels = 2;  // STEREO+SLOT_BOTH on the wire; relay does L→mono
        fc.bits_per_sample = 16;
        fc.blocksize = 4096;
        fc.compression_level = 5;
        if (s_flac->begin(fc, flac_write_cb, s_poster.get())) {
            s_using_flac = true;
        } else {
            ESP_LOGW(TAG,
                     "FLAC requested but encoder unavailable; "
                     "reopening as PCM");
            // We've already sent audio/flac headers; tear down and try
            // again as PCM on a fresh connection.
            s_poster->end();
            if (!setup_poster(pcm_ct)) {
                ESP_LOGW(TAG, "relay: PCM fallback connect failed");
                return false;  // unique_ptrs auto-reset in setup_poster
            }
        }
    }

    s_streaming.store(true, std::memory_order_release);
    s_chunks_sent.store(0, std::memory_order_relaxed);
    s_backoff_ms = 0;  // reset backoff on successful connect
    ESP_LOGI(TAG, "relay: stream opened → %s:%d%s codec=%s", RELAY_HOST, RELAY_PORT,
             relay_path_for_this_device(), s_using_flac ? "flac" : "pcm");
    return true;
}

void close_stream() {
    if (!s_streaming.load(std::memory_order_relaxed))
        return;
    if (s_using_flac && s_flac) {
        // Flush + finalize FLAC stream — emits last frame + STREAMINFO
        // patch attempt (no-seek, so remains a streaming flac with
        // total_samples=0 in header, ffmpeg/libFLAC handle this fine).
        s_flac->finish();
    }
    s_poster->end();
    ESP_LOGI(TAG, "relay: stream closed (status=%d, %u chunks, codec=%s)", s_poster->status_code(),
             static_cast<unsigned>(s_chunks_sent.load(std::memory_order_relaxed)),
             s_using_flac ? "flac" : "pcm");
    s_poster.reset();  // poster references *s_tx, drop it first
    s_tx.reset();
    s_streaming.store(false, std::memory_order_release);
    s_using_flac = false;
    s_chunks_sent.store(0, std::memory_order_relaxed);
}

bool read_frame() {
    size_t got = 0;
    esp_err_t e = i2s_channel_read(s_rx, s_buf16, FRAME_BYTES_S16, &got, pdMS_TO_TICKS(200));

    if (got == 0) {
        // Complete failure — no DMA data at all (mic absent or disconnected)
        static uint32_t s_zero_count = 0;
        if (++s_zero_count <= 3 || (s_zero_count % 250) == 0) {
            ESP_LOGW(TAG, "i2s read: no data (err=%d, count=%" PRIu32 ")", static_cast<int>(e),
                     s_zero_count);
        }
        vTaskDelay(pdMS_TO_TICKS(40));  // yield to prevent TWDT
        return false;
    }

    // Accept partial reads — zero-pad the remainder so downstream gets a
    // consistent frame size.  This handles DMA alignment issues and
    // intermittent B2B connector contact gracefully.
    if (got < FRAME_BYTES_S16) {
        static uint32_t s_partial_count = 0;
        if (++s_partial_count <= 5 || (s_partial_count % 500) == 0) {
            ESP_LOGW(TAG, "i2s read: partial %u/%u bytes (count=%" PRIu32 ")",
                     static_cast<unsigned>(got), static_cast<unsigned>(FRAME_BYTES_S16),
                     s_partial_count);
        }
        // Zero-pad the rest of the buffer
        memset(reinterpret_cast<uint8_t *>(s_buf16) + got, 0, FRAME_BYTES_S16 - got);
    }
    // Mirror left-channel samples into the snapshot ring as raw PCM.
    // STEREO+SLOT_BOTH delivers L,R interleaved with L == R (single
    // mic mirrored across both PDM slots) so we pick even indices for
    // mono. No DC removal / gain / EQ here — the ESP32-S3 PDM RX block
    // doesn't support HW gain or HP filter on this silicon, and doing
    // it in software costs CPU we'd rather spend on capture/WiFi. The
    // relay machine (server-host) has the CPU/GPU budget and is where
    // BirdNET-Go runs its analysis pipeline anyway, so any
    // amplification, DC blocker or denoise belongs there.
    if (s_ring) {
        xSemaphoreTake(s_ring_lock, portMAX_DELAY);
        uint32_t pos = s_ring_write_pos.load();
        for (size_t i = 0; i < FRAME_SAMPLES; i += 2) {
            s_ring[pos % SNAPSHOT_RING_SAMPLES] = s_buf16[i];
            pos++;
        }
        s_ring_write_pos.store(pos);
        xSemaphoreGive(s_ring_lock);
    }
    s_frames_captured.fetch_add(1);
    return true;
}

bool send_frame() {
    if (s_using_flac) {
        // libFLAC consumes samples and may emit 0+ chunked HTTP frames via
        // flac_write_cb. process() takes samples-per-channel, and our
        // buffer is STEREO interleaved (L,R,L,R), so frames = FRAME_SAMPLES/2.
        // process() returns false on encode error or write callback
        // failure; in either case tear down so next iteration reopens
        // cleanly.
        if (!s_flac->process(s_buf16, FRAME_SAMPLES / 2)) {
            ESP_LOGW(TAG, "FLAC process failed, closing stream");
            close_stream();
            return false;
        }
        // chunks_sent counts frame-grain audio handoffs even with FLAC;
        // it's a "we tried" counter, not a wire-chunk counter, which
        // matches the existing telemetry semantics.
        s_chunks_sent.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (!s_poster->send(s_buf16, FRAME_BYTES_S16)) {
        ESP_LOGW(TAG, "relay: send failed, closing stream");
        close_stream();
        return false;
    }
    s_chunks_sent.fetch_add(1, std::memory_order_relaxed);
    return true;
}

/* Internal lifecycle workers. Single-writer of s_rx + s_vad: called
 * only from audio_task (CPU1) at the top of its loop, OR from
 * app_main during the synchronous boot init phase before
 * audio_task_start() has spawned. Never from the supervisor mid-run. */
static bool audio_begin_impl(void);
static void audio_end_impl(void);

}  // namespace

extern "C" bool audio_begin(void) {
    /* Boot-phase callers (app_main on CPU0 before audio_task_start)
     * still get synchronous semantics — single-threaded, no race. */
    if (!s_audio_task_handle) {
        return audio_begin_impl();
    }
    /* Once audio_task is live, lifecycle changes must be applied
     * on its thread to avoid UAF on s_rx / s_vad. Post a request;
     * audio_task picks it up between pumps. Return true optimistically
     * — actual outcome surfaces via audio_ready() once applied. */
    s_lifecycle_target.store(LIFECYCLE_BEGIN, std::memory_order_release);
    return true;
}

extern "C" void audio_end(void) {
    if (!s_audio_task_handle) {
        audio_end_impl();
        return;
    }
    s_lifecycle_target.store(LIFECYCLE_END, std::memory_order_release);
}

namespace {

bool audio_begin_impl(void) {
    if (s_rx)
        return true;  // idempotent

    if (!s_ring) {
        s_ring = static_cast<int16_t *>(
            heap_caps_malloc(SNAPSHOT_RING_SAMPLES * sizeof(int16_t),
                             MALLOC_CAP_SPIRAM));
        if (!s_ring) {
            ESP_LOGW(TAG, "snapshot ring: PSRAM alloc failed, /mic.wav disabled");
        } else {
            memset(s_ring, 0, SNAPSHOT_RING_SAMPLES * sizeof(int16_t));
        }
    }
    if (!s_ring_lock) {
        s_ring_lock = xSemaphoreCreateMutex();
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear = false;

    if (i2s_new_channel(&chan_cfg, /*tx*/ nullptr, /*rx*/ &s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        return false;
    }

    // STEREO mode + only line 0 (both slots): ESP32-S3 supports up to 4
    // PDM RX data lines, and the IDF default for ESP32-S3 stereo PDM
    // happens to scan all 8 (4 lines × L/R) slots — on the XIAO Sense
    // with a single MEMS mic on DIN, that produces 2 real samples then
    // 6 garbage int16 per 8-int16 group (the inactive lines float to
    // ~-30935). Restrict slot_mask to LINE0 only so the buffer is full
    // of mic data with the inactive line/slot interleave gone.
    // MONO mode (slot_mode=MONO with slot_mask=LEFT/RIGHT) is what the
    // Seeed wiki recommends, but in our combination of IDF + XIAO Sense
    // it returned ESP_ERR_TIMEOUT forever or 1-descriptor partial fills.
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(static_cast<uint32_t>(I2S_SAMPLE_RATE)),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .clk = static_cast<gpio_num_t>(I2S_PDM_CLK_PIN),
                .din = static_cast<gpio_num_t>(I2S_PDM_DATA_PIN),
                .invert_flags =
                    {
                        .clk_inv = false,
                    },
            },
    };
    pdm_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_BOTH;  // line 0 L+R only

    // ESP32-S3 PDM RX has no on-chip HP filter / amplify_num control
    // (those slot_cfg fields exist only when SOC_I2S_SUPPORTS_PDM_RX_HP
    // _FILTER is defined). The only knob is the PDM downsample ratio,
    // so use DSR_16S for cleaner anti-alias filtering inside the
    // 1–8 kHz band relevant to bird vocalisations (BirdNET-Go feed).
    pdm_cfg.clk_cfg.dn_sample_mode = I2S_PDM_DSR_16S;

    if (i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_pdm_rx_mode failed");
        i2s_del_channel(s_rx);
        s_rx = nullptr;
        return false;
    }
    if (i2s_channel_enable(s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed");
        i2s_del_channel(s_rx);
        s_rx = nullptr;
        return false;
    }

    cb::Vad::Config vc;
    vc.threshold_dbfs = app_config_get_float("vad_thr_dbfs");
    vc.window_samples = 960;
    vc.burst_ms = (uint32_t)app_config_get_int("vad_burst_ms");
    vc.rearm_ms = (uint32_t)app_config_get_int("vad_rearm_ms");
    // See apply_vad_reconfig_if_pending() for why this is doubled.
    vc.sample_rate = (uint32_t)(I2S_SAMPLE_RATE * 2);
    s_vad = std::make_unique<cb::Vad>(vc);

    /* I2S DMA is now live — block light-sleep so the PDM stream isn't gated
     * mid-frame (Phase 2; no-op when PM compiled out / pm_lightsleep off). */
    if (!s_pm_held) {
        cb_pm_no_sleep_acquire();
        s_pm_held = true;
    }

    ESP_LOGI(TAG,
             "I²S PDM ready (sr=%d, frame=%u samples, clk=GPIO%d "
             "din=GPIO%d)",
             I2S_SAMPLE_RATE, static_cast<unsigned>(FRAME_SAMPLES), I2S_PDM_CLK_PIN,
             I2S_PDM_DATA_PIN);
    return true;
}

void audio_end_impl(void) {
    close_stream();
    if (s_rx) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = nullptr;
    }
    s_vad.reset();
    s_flac.reset();
    /* Mic torn down — allow light-sleep again. */
    if (s_pm_held) {
        cb_pm_no_sleep_release();
        s_pm_held = false;
    }
}

/* Drain any pending begin/end request posted by the supervisor. Called
 * at the top of audio_task before any access to s_rx / s_vad, so the
 * state machine is single-writer and reads always observe a consistent
 * (s_rx, s_vad) pair. */
void apply_lifecycle_if_pending(void) {
    int target = s_lifecycle_target.exchange(LIFECYCLE_NONE,
                                             std::memory_order_acquire);
    if (target == LIFECYCLE_BEGIN) {
        audio_begin_impl();
    } else if (target == LIFECYCLE_END) {
        audio_end_impl();
    }
}

}  // namespace

extern "C" void audio_pump_continuous(void) {
    if (!s_rx)
        return;
    apply_vad_reconfig_if_pending();
    if (!read_frame())
        return;
    if (s_vad) {
        uint32_t prev_burst = s_vad->burst_count();
        s_vad->update(s_buf16, FRAME_SAMPLES, now_ms());  // telemetry
        s_last_rms_dbfs_atomic.store(s_vad->last_rms_dbfs(),
                                     std::memory_order_release);
        s_burst_count_atomic.store(s_vad->burst_count(),
                                   std::memory_order_release);
        /* VAD burst-trigger is tier-independent: a burst start still requests a
         * photo + HA triggered event in Max, even though Max keeps streaming
         * continuously regardless of VAD state. Without this the burst trigger
         * fired only in Active (audio_pump_triggered) — surprising, since the
         * mic is plainly live in Max. vad_enabled gates only the trigger here;
         * the continuous stream itself is unconditional (its whole purpose). */
        if (app_config_get_bool("vad_enabled") &&
            s_vad->burst_count() != prev_burst) {
            mqtt_publish_triggered(s_vad->last_rms_dbfs());
            s_vad_capture_req.store(true);
        }
    }
    if (!s_streaming.load(std::memory_order_relaxed) && !open_stream())
        return;
    send_frame();
}

extern "C" void audio_pump_triggered(void) {
    if (!s_rx || !s_vad)
        return;
    apply_vad_reconfig_if_pending();
    if (!read_frame())
        return;
    uint32_t prev_burst = s_vad->burst_count();
    bool active = s_vad->update(s_buf16, FRAME_SAMPLES, now_ms());
    s_last_rms_dbfs_atomic.store(s_vad->last_rms_dbfs(),
                                 std::memory_order_release);
    s_burst_count_atomic.store(s_vad->burst_count(),
                               std::memory_order_release);
    /* When VAD is disabled, frames are still read (so /mic.wav and the
     * RMS telemetry stay live) and update() still runs for last_rms_dbfs,
     * but the burst-triggered side effects — photo, MQTT triggered event,
     * audio relay stream — are all suppressed. */
    if (!app_config_get_bool("vad_enabled")) {
        if (s_streaming.load(std::memory_order_relaxed)) close_stream();
        return;
    }
    if (s_vad->burst_count() != prev_burst) {
        // Burst just started → notify HA with trigger RMS, request photo.
        /* mqtt_publish_triggered() resolves the wall clock internally;
         * we used to pass now_ms() (uptime) here as if it were epoch,
         * which made the JSON `ts` field meaningless in HA. */
        mqtt_publish_triggered(s_vad->last_rms_dbfs());
        s_vad_capture_req.store(true);
    }
    if (active) {
        if (!s_streaming.load(std::memory_order_relaxed) && !open_stream())
            return;
        send_frame();
    } else if (s_streaming.load(std::memory_order_relaxed)) {
        close_stream();  // burst ended → tear down to save power
    }
}

/* Snapshot getters — read the atomic mirrors instead of s_vad / s_streaming
 * directly, so telemetry on CPU0 can't trip on a partially-updated VAD
 * state being written by the audio task on CPU1. */
extern "C" float audio_last_rms_dbfs(void) {
    return s_last_rms_dbfs_atomic.load(std::memory_order_acquire);
}
extern "C" uint32_t audio_burst_count(void) {
    return s_burst_count_atomic.load(std::memory_order_acquire);
}
extern "C" uint32_t audio_chunks_sent(void) {
    return s_chunks_sent.load(std::memory_order_relaxed);
}
extern "C" bool audio_streaming(void) {
    return s_streaming.load(std::memory_order_acquire);
}
extern "C" bool audio_vad_capture_consume(void) {
    return s_vad_capture_req.exchange(false);
}

/* Dedicated audio task pinned to CPU1 prio 10. Reads the supervisor's
 * mode atomically every iteration and dispatches to the existing
 * pump_continuous/triggered helpers, or sleeps in Safe/Boot. Subscribes
 * to TWDT so a stuck i2s_channel_read or wedged TCP send still triggers
 * the panic + coredump pipeline. Keeping the WDT reset at the *top* of
 * the loop means the timer is fed before any blocking call, not after —
 * a stalled blocking call still has a full WDT timeout window to fire. */
static void audio_task(void *) {
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "esp_task_wdt_add(audio) failed — task not WDT-protected");
    }
    ESP_LOGI(TAG, "task started on CPU%d prio %u",
             xPortGetCoreID(),
             (unsigned)uxTaskPriorityGet(NULL));
    while (true) {
        esp_task_wdt_reset();
        /* Process any begin/end lifecycle posted by the supervisor on
         * mode transition. Done BEFORE the pump dispatch so we never
         * pump on a state about to be torn down — single-writer of
         * s_rx + s_vad is preserved (this task is the only writer
         * outside the synchronous boot path). */
        apply_lifecycle_if_pending();
        /* Idle whenever the capture channel is torn down — NOT just in
         * Safe/Boot. The active-hours window can close while the mode is
         * still Continuous/Triggered (audio_end_impl freed s_rx above); the
         * pump helpers then early-exit on !s_rx, and without this guard the
         * loop busy-spins at prio 10 on CPU1. That starves co-located tasks
         * (the no-affinity esp-mqtt task, when scheduled on CPU1) → observed
         * multi-second cmd/MQTT stalls + task_wdt. Sleep until audio_begin
         * restores s_rx (re-checked next iteration; ≤1 s restart lag). */
        if (!s_rx) {
            vTaskDelay(pdMS_TO_TICKS(app_profile_sleeps() ? 1000 : 200));
            continue;
        }
        auto p = static_cast<cb::Profile>(app_mode_current());
        switch (p) {
            case cb::Profile::Max:
                audio_pump_continuous();
                break;
            case cb::Profile::Active:
                audio_pump_triggered();
                break;
            case cb::Profile::Eco:
            case cb::Profile::Sentinel:
            case cb::Profile::Hibernate:
            case cb::Profile::Boot:
                /* Mic idle in these tiers (audio off by definition in the
                 * sleeping tiers) — s_rx is normally already null here (handled
                 * above); keep the explicit sleep as a belt-and-suspenders yield
                 * in case a profile flip races s_rx. */
                vTaskDelay(pdMS_TO_TICKS(app_profile_sleeps() ? 1000 : 200));
                break;
        }
    }
}

extern "C" void audio_task_start(void) {
    if (s_audio_task_handle) {
        return;  // idempotent
    }
    BaseType_t ok = xTaskCreatePinnedToCore(
        audio_task, "audio", 8192, NULL, /*prio*/ 10, &s_audio_task_handle,
        /*core*/ 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(audio) failed: %d", (int)ok);
        s_audio_task_handle = nullptr;
    }
}

/* Selftest hook — true iff xTaskCreatePinnedToCore succeeded. A false
 * here under normal boot means OOM at task-create time; selftest will
 * surface it on MQTT so a silently-tichá budka has a visible signal. */
extern "C" bool audio_task_running(void) {
    return s_audio_task_handle != nullptr;
}

extern "C" void audio_apply_config(void) {
    if (!s_vad)
        return;  // not initialized; values picked up next audio_begin()
    // Defer the actual reconfigure to the audio pump task to avoid racing
    // with s_vad accesses. Called from MQTT task → audio task is the
    // single owner of s_vad after init.
    s_vad_reconfig_req.store(true);
}

extern "C" bool audio_ready(void) {
    return s_rx != nullptr && s_frames_captured.load() > 0;
}

extern "C" uint32_t audio_frames_captured(void) {
    return s_frames_captured.load();
}

extern "C" uint32_t audio_sample_rate(void) {
    // STEREO + SLOT_BOTH reads two samples per timing slot, but we only
    // keep the left one when writing to the ring (R is a duplicate), so
    // the effective mono rate is the same as the I2S sample_rate_hz we
    // configured.
    return (uint32_t)I2S_SAMPLE_RATE;
}

// Build a minimal RIFF/WAVE header for 16-bit PCM, single channel.
static void wav_header_le(uint8_t *h, uint32_t rate, uint32_t n_samples) {
    uint32_t data_len = n_samples * 2;
    uint32_t riff_len = 36 + data_len;
    uint32_t byte_rate = rate * 2;  // 16-bit mono = 2 B per sample
    auto wr32 = [](uint8_t *p, uint32_t v) {
        p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
        p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
    };
    memcpy(h, "RIFF", 4);          wr32(h + 4, riff_len);
    memcpy(h + 8, "WAVEfmt ", 8);  wr32(h + 16, 16);  // fmt chunk size
    h[20] = 1; h[21] = 0;            // PCM
    h[22] = 1; h[23] = 0;            // 1 channel
    wr32(h + 24, rate);
    wr32(h + 28, byte_rate);
    h[32] = 2; h[33] = 0;            // block align
    h[34] = 16; h[35] = 0;           // bits per sample
    memcpy(h + 36, "data", 4);
    wr32(h + 40, data_len);
}

extern "C" size_t audio_snapshot_wav(uint8_t *out_buf, size_t out_cap,
                                     float seconds) {
    if (!s_ring || !out_buf) return 0;
    if (seconds <= 0.0f) seconds = 3.0f;
    uint32_t want = (uint32_t)(seconds * I2S_SAMPLE_RATE);
    if (want > SNAPSHOT_RING_SAMPLES) want = SNAPSHOT_RING_SAMPLES;

    size_t need = 44 + want * 2;
    if (need > out_cap) {
        want = (out_cap > 44) ? ((out_cap - 44) / 2) : 0;
        need = 44 + want * 2;
    }
    if (want == 0) return 0;

    wav_header_le(out_buf, audio_sample_rate(), want);

    /* HTTP-side reader: short timeout instead of portMAX_DELAY. The audio
     * task holds this lock for a single ring memcpy per 20 ms frame
     * (~30 us), so 50 ms is comfortably above worst-case wait under
     * normal load. On contention rewrite the header with sample_count=0
     * and return header-only — a missed snapshot frame is preferable to
     * wedging the HTTP server task. */
    if (xSemaphoreTake(s_ring_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        wav_header_le(out_buf, audio_sample_rate(), 0);
        return 44;
    }
    uint32_t write_pos = s_ring_write_pos.load();
    // Clamp `want` to how many samples we have actually captured so
    // far, so a fresh boot doesn't return seconds of zero-padding.
    uint32_t available = (write_pos < SNAPSHOT_RING_SAMPLES) ? write_pos
                                                              : SNAPSHOT_RING_SAMPLES;
    if (want > available) want = available;

    int16_t *samples = reinterpret_cast<int16_t *>(out_buf + 44);
    uint32_t start = write_pos - want;
    for (uint32_t i = 0; i < want; ++i) {
        samples[i] = s_ring[(start + i) % SNAPSHOT_RING_SAMPLES];
    }
    xSemaphoreGive(s_ring_lock);

    // Rewrite header with the actual sample count (may have been
    // clamped down by `available`).
    wav_header_le(out_buf, audio_sample_rate(), want);
    return 44 + want * 2;
}

extern "C" void audio_wav_header_streaming(uint8_t *h, uint32_t rate) {
    // 0xFFFFFFFF in the riff/data length fields = "unknown / streaming"
    // convention all common players (VLC, ffplay, Chrome MSE, Safari)
    // honour by reading until EOF.
    wav_header_le(h, rate, 0x7FFFFFF0u);  // samples; data_len = 0xFFFFFFE0
    // Patch riff_len + data_len to the standard streaming sentinel.
    auto wr32 = [](uint8_t *p, uint32_t v) {
        p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
        p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
    };
    wr32(h + 4, 0xFFFFFFFFu);
    wr32(h + 40, 0xFFFFFFFFu);
}

extern "C" uint32_t audio_ring_write_pos(void) {
    return s_ring_write_pos.load();
}

extern "C" size_t audio_ring_read(uint32_t read_pos, int16_t *dst,
                                  size_t max_samples,
                                  uint32_t *next_read_pos) {
    if (!s_ring || !dst || max_samples == 0) {
        if (next_read_pos) *next_read_pos = read_pos;
        return 0;
    }
    /* HTTP-streaming reader: short timeout instead of portMAX_DELAY. On
     * contention return 0 samples with read_pos unchanged so the caller
     * retries on the next poll — preferable to wedging /mic.wav. */
    if (xSemaphoreTake(s_ring_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        if (next_read_pos) *next_read_pos = read_pos;
        return 0;
    }
    uint32_t write_pos = s_ring_write_pos.load();
    uint32_t avail = write_pos - read_pos;
    // If we fell more than the ring capacity behind, fast-forward to
    // SNAPSHOT_RING_SAMPLES/2 behind the writer so we have ~1.5s of
    // headroom before the next drop. Dropped samples are silent (no
    // log) — under WiFi backpressure this happens routinely.
    if (avail > SNAPSHOT_RING_SAMPLES) {
        read_pos = write_pos - (SNAPSHOT_RING_SAMPLES / 2);
        avail = SNAPSHOT_RING_SAMPLES / 2;
    }
    if (avail == 0) {
        xSemaphoreGive(s_ring_lock);
        if (next_read_pos) *next_read_pos = read_pos;
        return 0;
    }
    size_t n = (avail < max_samples) ? avail : max_samples;
    for (size_t i = 0; i < n; ++i) {
        dst[i] = s_ring[(read_pos + i) % SNAPSHOT_RING_SAMPLES];
    }
    xSemaphoreGive(s_ring_lock);
    if (next_read_pos) *next_read_pos = read_pos + n;
    return n;
}
