/* audio.h — onboard PDM mic capture → cb::Vad → cb::ChunkedPoster relay. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool audio_begin(void);
void audio_end(void);

/* Start the dedicated audio task pinned to CPU1 prio 10. Idempotent —
 * subsequent calls after the task is running are no-ops. Must be called
 * after audio_begin(); the task internally dispatches to
 * audio_pump_continuous/triggered based on app_mode_current(), or sleeps
 * in Safe/Boot. */
void audio_task_start(void);

/* True iff audio_task_start() succeeded and the task handle is live.
 * Selftest publishes this so a silent xTaskCreate OOM is visible. */
bool audio_task_running(void);

void audio_pump_continuous(void);
void audio_pump_triggered(void);

float audio_last_rms_dbfs(void);
uint32_t audio_burst_count(void);
uint32_t audio_chunks_sent(void);
bool audio_streaming(void);

/* Returns true exactly once per VAD burst-start edge (consume-on-read).
 * Used to drive camera capture from the main loop without blocking the
 * audio pump. */
bool audio_vad_capture_consume(void);

/* Re-load VAD parameters from app_config (called when keys change). */
void audio_apply_config(void);

/* True once the mic has delivered at least one full DMA frame since
 * audio_begin(). Selftest reports this so "mic: true" actually means
 * we got data, not just that the I2S driver initialized. */
bool audio_ready(void);

/* Lifetime count of successfully-captured DMA frames. Monotonic; only
 * advances while the audio task is actually pumping (Continuous /
 * Triggered modes — NOT Safe/Boot, where the mic is idle by design).
 * Selftest samples the delta between periodic runs to detect a mic that
 * went dead AFTER boot (bad B2B contact) — which audio_ready() alone
 * can't catch, since it stays true on the lifetime>0 check. */
uint32_t audio_frames_captured(void);

/* Sample rate the audio task is actually capturing at (used for the WAV
 * header on /mic.wav). */
uint32_t audio_sample_rate(void);

/* Build a 16-bit mono RIFF/WAV stream covering the most recent
 * ~seconds of audio, ending at "now". Writes to *out_buf (caller-
 * provided) up to out_cap bytes, returns the number of bytes written
 * (header + samples). Returns 0 if the ring is empty or output is too
 * small. Captures from the left PDM slot only (R is a duplicate of L
 * on the XIAO Sense). */
size_t audio_snapshot_wav(uint8_t *out_buf, size_t out_cap, float seconds);

/* Write a 44-byte RIFF/WAVE header into *h for a *streaming* PCM file
 * of unknown length. Uses data_len = 0xFFFFFFFF — VLC, ffplay, Chrome
 * and Safari all accept this as "play until the connection ends". */
void audio_wav_header_streaming(uint8_t *h, uint32_t rate);

/* Current ring write position (monotonic, atomic load). The HTTP
 * live-mic handler uses this as its starting cursor. */
uint32_t audio_ring_write_pos(void);

/* Copy up to `max_samples` from the ring starting at `read_pos`. Sets
 * *next_read_pos to where the caller should continue. If the caller
 * has fallen more than the ring capacity behind the writer, jump
 * forward (drop oldest samples) to recover; the drop is silent. */
size_t audio_ring_read(uint32_t read_pos, int16_t *dst, size_t max_samples,
                       uint32_t *next_read_pos);

#ifdef __cplusplus
}
#endif
