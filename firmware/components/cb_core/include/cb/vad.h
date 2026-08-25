// vad.h — RMS-based voice/sound activity detector with burst latching
// Pure C++17, no platform deps.
//
// Usage:
//   cb::Vad::Config cfg;
//   cb::Vad vad(cfg);
//   while (have_pcm) {
//     bool active = vad.update(pcm_ptr, n_samples, now_ms);
//     if (active) post_to_relay(pcm_ptr, n_samples);
//   }
#pragma once

#include <cstddef>
#include <cstdint>

namespace cb {

class Vad {
 public:
  struct Config {
    float threshold_dbfs = -45.0f;   // RMS over window must exceed this to trigger
    uint32_t window_samples = 960;   // 20 ms @ 48 kHz
    uint32_t burst_ms = 30000;       // keep "active" for this long after a trigger
    uint32_t rearm_ms = 5000;        // refractory period after a burst ends
    uint32_t sample_rate = 48000;    // for diagnostics only
  };

  explicit Vad(const Config &cfg);

  // Process n int16 PCM samples (mono). Returns true if currently inside an
  // active burst (i.e. caller should be transmitting audio). The function
  // accumulates samples into rolling 'window_samples'-sized RMS frames; each
  // completed frame may extend the burst deadline.
  bool update(const int16_t *pcm, size_t n, uint32_t now_ms);

  // Last completed window's RMS, in dBFS. -120 if no full window yet.
  float last_rms_dbfs() const { return last_dbfs_; }

  // True iff currently inside a burst (same as update()'s last return value).
  bool active() const { return active_; }

  // Number of bursts started since construction (telemetry).
  uint32_t burst_count() const { return burst_count_; }

 private:
  Config cfg_;
  // window accumulator
  uint32_t window_pos_ = 0;
  uint64_t window_sumsq_ = 0;
  int64_t  window_sum_ = 0;   // Σx → DC mean, removed from RMS (PDM mics carry a
                              // big DC bias; without this the RMS pins at the DC
                              // level and barely moves with actual sound).
  float last_dbfs_ = -120.0f;
  // burst state
  uint32_t burst_until_ms_ = 0;
  uint32_t rearm_after_ms_ = 0;
  bool active_ = false;
  bool in_rearm_ = false;
  uint32_t burst_count_ = 0;
};

}  // namespace cb
