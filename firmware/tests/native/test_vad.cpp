// test_vad.cpp — unit tests for cb::Vad.
// No framework: plain assertions, exit code 0 == pass.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "cb/vad.h"

namespace {

std::vector<int16_t> sine(uint32_t rate, double freq, double dur_s, double amp_dbfs) {
  size_t n = static_cast<size_t>(rate * dur_s);
  std::vector<int16_t> out(n);
  double amp = std::pow(10.0, amp_dbfs / 20.0) * 32767.0;
  for (size_t i = 0; i < n; i++) {
    double t = static_cast<double>(i) / rate;
    out[i] = static_cast<int16_t>(amp * std::sin(2 * M_PI * freq * t));
  }
  return out;
}

std::vector<int16_t> silence(uint32_t rate, double dur_s) {
  return std::vector<int16_t>(static_cast<size_t>(rate * dur_s), 0);
}

void test_silence_does_not_trigger() {
  cb::Vad::Config cfg;
  cfg.threshold_dbfs = -45.0f;
  cb::Vad vad(cfg);
  auto buf = silence(48000, 1.0);
  bool active = vad.update(buf.data(), buf.size(), 0);
  assert(!active);
  assert(vad.burst_count() == 0);
  std::printf("  ok: silence → no trigger (rms=%.1f dBFS)\n", vad.last_rms_dbfs());
}

void test_loud_signal_triggers() {
  cb::Vad::Config cfg;
  cfg.threshold_dbfs = -45.0f;
  cb::Vad vad(cfg);
  auto buf = sine(48000, 1000, 0.5, -20.0);  // -20 dBFS, well above -45
  bool active = vad.update(buf.data(), buf.size(), 0);
  assert(active);
  assert(vad.burst_count() == 1);
  std::printf("  ok: loud signal → trigger (rms=%.1f dBFS)\n", vad.last_rms_dbfs());
}

void test_burst_extends_then_releases() {
  cb::Vad::Config cfg;
  cfg.threshold_dbfs = -45.0f;
  cfg.burst_ms = 1000;
  cfg.rearm_ms = 500;
  cb::Vad vad(cfg);

  // 100 ms loud → triggers
  auto loud = sine(48000, 1000, 0.1, -20.0);
  assert(vad.update(loud.data(), loud.size(), 0));
  assert(vad.burst_count() == 1);

  // 500 ms silence at t=100ms → still active (within burst_ms)
  auto sil = silence(48000, 0.5);
  assert(vad.update(sil.data(), sil.size(), 100));

  // 600 ms silence at t=600ms → reaches t=1100ms, burst should release at >=1000ms
  // we feed in chunks of 100ms each so the 'now_ms' is sampled per-window
  for (uint32_t t = 600; t < 1200; t += 100) {
    auto chunk = silence(48000, 0.1);
    vad.update(chunk.data(), chunk.size(), t);
  }
  assert(!vad.active());
  std::printf("  ok: burst extends through silence then releases\n");

  // Within rearm window: loud should NOT retrigger
  auto loud2 = sine(48000, 1000, 0.1, -20.0);
  bool a = vad.update(loud2.data(), loud2.size(), 1300);  // within rearm (< 1700)
  assert(!a);
  assert(vad.burst_count() == 1);

  // After rearm window: loud should retrigger
  auto loud3 = sine(48000, 1000, 0.1, -20.0);
  bool a2 = vad.update(loud3.data(), loud3.size(), 2000);
  assert(a2);
  assert(vad.burst_count() == 2);
  std::printf("  ok: rearm window enforced; second burst counted\n");
}

void test_quiet_signal_does_not_trigger() {
  cb::Vad::Config cfg;
  cfg.threshold_dbfs = -45.0f;
  cb::Vad vad(cfg);
  auto buf = sine(48000, 1000, 0.5, -55.0);  // -55 dBFS, below threshold
  bool active = vad.update(buf.data(), buf.size(), 0);
  assert(!active);
  std::printf("  ok: quiet signal → no trigger (rms=%.1f dBFS)\n", vad.last_rms_dbfs());
}

}  // namespace

int main() {
  std::printf("test_vad:\n");
  test_silence_does_not_trigger();
  test_quiet_signal_does_not_trigger();
  test_loud_signal_triggers();
  test_burst_extends_then_releases();
  std::printf("test_vad: PASS\n");
  return 0;
}
