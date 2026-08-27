// test_vad_wrap.cpp — VAD timer wrap-around (uint32_t millis overflow at ~49.7 d)
//
// Regression test for the original bug:
//   if (now_ms >= burst_until_ms_) { ... }
// where burst_until_ms_ = now_ms + burst_ms can wrap below now_ms,
// causing the comparison to immediately end the burst after the wrap.

#include <cassert>
#include <climits>
#include <cmath>
#include <cstdio>
#include <vector>

#include "cb/vad.h"

namespace {

std::vector<int16_t> sine(uint32_t rate, double freq, double dur_s, double dbfs) {
  size_t n = static_cast<size_t>(rate * dur_s);
  std::vector<int16_t> out(n);
  double amp = std::pow(10.0, dbfs / 20.0) * 32767.0;
  for (size_t i = 0; i < n; i++) {
    double t = static_cast<double>(i) / rate;
    out[i] = static_cast<int16_t>(amp * std::sin(2 * M_PI * freq * t));
  }
  return out;
}
std::vector<int16_t> silence(uint32_t rate, double dur_s) {
  return std::vector<int16_t>(static_cast<size_t>(rate * dur_s), 0);
}

// Trigger right before millis() wraps; verify burst stays latched across the
// wrap and only ends after burst_ms wall-clock has elapsed.
//
// IMPORTANT: tests use loud/silence sizes that are exact multiples of
// window_samples (960 @ 48kHz = 20ms) so window boundaries align with
// signal boundaries — otherwise leftover loud samples bleed into the
// next silence window and cause spurious retriggers.
void test_burst_survives_wrap() {
  cb::Vad::Config cfg;
  cfg.threshold_dbfs = -45.0f;
  cfg.burst_ms = 1000;
  cfg.rearm_ms = 500;
  cfg.window_samples = 960;
  cb::Vad vad(cfg);

  const uint32_t T0 = UINT32_MAX - 200;  // 200 ms before wrap

  // Loud burst arrives just before wrap (40 ms = 2 windows exactly)
  auto loud = sine(48000, 1000, 0.04, -20.0);
  bool a = vad.update(loud.data(), loud.size(), T0);
  assert(a);
  assert(vad.burst_count() == 1);

  // 100 ms still pre-wrap → still active
  auto sil100 = silence(48000, 0.1);  // 4800 samples = 5 windows
  assert(vad.update(sil100.data(), sil100.size(), T0 + 40));

  // Cross the wrap. Feed silence at progressively later times, all in 100ms
  // chunks. burst_until_ms_ = T0 + 1000 wraps to ~800.
  for (uint32_t dt = 140; dt < 1000; dt += 100) {
    assert(vad.update(sil100.data(), sil100.size(), (uint32_t)(T0 + dt)));
  }

  // 1100 ms after T0 → burst should release (burst_ms=1000 elapsed)
  vad.update(sil100.data(), sil100.size(), (uint32_t)(T0 + 1100));
  assert(!vad.active());
  std::printf("  ok: burst survived uint32 wrap, released after burst_ms\n");
}

// rearm_after_ms_ also crosses the wrap; second trigger within the rearm
// window must NOT count, but trigger after rearm window must.
void test_rearm_survives_wrap() {
  cb::Vad::Config cfg;
  cfg.threshold_dbfs = -45.0f;
  cfg.burst_ms = 200;
  cfg.rearm_ms = 800;
  cfg.window_samples = 960;
  cb::Vad vad(cfg);

  const uint32_t T0 = UINT32_MAX - 100;

  // 20 ms loud = exactly 1 window → no leftover bleed into silence
  auto loud20 = sine(48000, 1000, 0.02, -20.0);
  auto sil100 = silence(48000, 0.1);
  auto sil40 = silence(48000, 0.04);  // 2 windows for fine control

  // First trigger right before wrap
  vad.update(loud20.data(), loud20.size(), T0);
  assert(vad.burst_count() == 1);

  // Burst ends at T0+200 (wraps); first silence at T0+250 must release it
  vad.update(sil40.data(), sil40.size(), (uint32_t)(T0 + 250));
  assert(!vad.active());

  // Second loud during rearm window (rearm until T0+1050) → must NOT retrigger
  vad.update(loud20.data(), loud20.size(), (uint32_t)(T0 + 500));
  assert(vad.burst_count() == 1);

  // Loud after rearm window → retriggers
  vad.update(loud20.data(), loud20.size(), (uint32_t)(T0 + 1100));
  assert(vad.burst_count() == 2);
  std::printf("  ok: rearm enforced across wrap; second burst counted after\n");

  // Drain the second burst's leftover state across more wrap arithmetic
  vad.update(sil100.data(), sil100.size(), (uint32_t)(T0 + 1400));
  assert(!vad.active());
  std::printf("  ok: post-wrap second burst also releases cleanly\n");
}

void test_zero_window_clamped() {
  cb::Vad::Config cfg;
  cfg.window_samples = 0;  // would div-by-zero without clamp
  cfg.threshold_dbfs = -45.0f;
  cb::Vad vad(cfg);
  auto buf = silence(48000, 0.001);
  vad.update(buf.data(), buf.size(), 0);
  std::printf("  ok: window_samples=0 clamped (no div-by-zero)\n");
}

}  // namespace

int main() {
  std::printf("test_vad_wrap:\n");
  test_zero_window_clamped();
  test_burst_survives_wrap();
  test_rearm_survives_wrap();
  std::printf("test_vad_wrap: PASS\n");
  return 0;
}
