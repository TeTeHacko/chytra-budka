#include "cb/vad.h"

#include <cmath>

namespace cb {

Vad::Vad(const Config &cfg) : cfg_(cfg) {
  if (cfg_.window_samples == 0) cfg_.window_samples = 1;
}

bool Vad::update(const int16_t *pcm, size_t n, uint32_t now_ms) {
  for (size_t i = 0; i < n; i++) {
    int32_t s = pcm[i];
    window_sumsq_ += static_cast<uint64_t>(s * s);
    window_sum_   += s;
    window_pos_++;
    if (window_pos_ >= cfg_.window_samples) {
      // AC RMS = sqrt(variance) = sqrt(mean(x²) − mean(x)²). Subtracting the DC
      // mean is essential for PDM mics: their stream carries a large DC bias, so
      // the raw sqrt(mean(x²)) sits pinned at the DC level (~−28 dBFS) and barely
      // tracks sound. Variance isolates the actual acoustic energy. Identical to
      // the old formula for a zero-mean signal (e.g. the native sine tests).
      double mean    = static_cast<double>(window_sum_) / window_pos_;
      double mean_sq = static_cast<double>(window_sumsq_) / window_pos_;
      double var = mean_sq - mean * mean;
      if (var < 0.0) var = 0.0;                 // FP rounding guard
      double rms = std::sqrt(var);
      // dBFS relative to full-scale int16 (32768)
      last_dbfs_ = (rms > 0.0) ? static_cast<float>(20.0 * std::log10(rms / 32768.0))
                               : -120.0f;
      window_sumsq_ = 0;
      window_sum_ = 0;
      window_pos_ = 0;

      // burst latching logic — uses signed-difference comparisons gated by
      // in_rearm_/active_ flags so the monotonic ms timer is free to wrap
      // (uint32_t wraps every ~49.7 days). Without the flags, signed-diff
      // breaks on the very first call when the reference (rearm_after_ms_=0)
      // is meaningless.
      bool over_threshold = (last_dbfs_ >= cfg_.threshold_dbfs);

      // Clear rearm flag once enough wall-clock time has passed.
      if (in_rearm_ && (int32_t)(now_ms - rearm_after_ms_) >= 0) {
        in_rearm_ = false;
      }

      if (over_threshold && !in_rearm_) {
        if (!active_) {
          active_ = true;
          burst_count_++;
        }
        burst_until_ms_ = now_ms + cfg_.burst_ms;
      }
      if (active_ && (int32_t)(now_ms - burst_until_ms_) >= 0) {
        active_ = false;
        in_rearm_ = true;
        rearm_after_ms_ = now_ms + cfg_.rearm_ms;
      }
    }
  }
  return active_;
}

}  // namespace cb
