// flac_encoder.h — streaming FLAC encoder wrapper for cb_core.
//
// Pure C++17. Wraps libFLAC's stream_encoder API behind a callback that
// receives encoded byte runs (FLAC frames), so the caller decides where the
// bytes go (TCP socket, file, ring buffer, …).
//
// Compile-time toggle CB_HAVE_LIBFLAC controls whether the real libFLAC
// implementation is built:
//   * Defined  → links against libFLAC, full encoder.
//   * Undefined → header parses, methods all return false / no-op so callers
//                 can be written codec-agnostic and gate at runtime.
//
// Wire format choice: native FLAC stream (fLaC magic + STREAMINFO + frames),
// not OGG-FLAC. Each VAD burst opens a fresh TCP connection → STREAMINFO
// header is re-sent every burst, so resync is implicit. Avoids pulling libogg.
//
// Usage:
//   cb::FlacEncoder enc;
//   cb::FlacEncoder::Config c;  // 48k mono s16 defaults
//   enc.begin(c, [](const uint8_t* p, size_t n, void* u) -> bool {
//       return ((MySink*)u)->write(p, n);
//   }, &sink);
//   while (have_frames) enc.process(samples, n_frames);
//   enc.finish();
#pragma once

#include <cstddef>
#include <cstdint>

namespace cb {

class FlacEncoder {
 public:
  // Callback signature: encoder hands you `bytes` of FLAC stream data.
  // Return false to abort the encode (next process() call will fail).
  using WriteCallback = bool (*)(const uint8_t *data, size_t bytes,
                                 void *user);

  struct Config {
    uint32_t sample_rate = 48000;
    uint8_t channels = 1;
    uint8_t bits_per_sample = 16;
    // FLAC blocksize in samples-per-channel. 4096 is libFLAC default for
    // subset compliance and yields good compression on long predictable
    // signal (bird audio = bursts of harmonics + silence).
    uint32_t blocksize = 4096;
    // 0 = fastest / largest, 8 = slowest / smallest. Level 5 is libFLAC
    // default; on ESP32-S3 @ 240 MHz, level 5 mono 48 kHz fits within
    // ~15 % CPU during a streaming burst. Lower this if encode budget is
    // tight; bird recordings already compress to 35-55 % of PCM at L5.
    uint32_t compression_level = 5;
    // If true and bits_per_sample == 16, libFLAC verifies decode == input
    // each frame. Costs ~2× CPU; only flip on for native test / debug.
    bool verify = false;
  };

  FlacEncoder();
  ~FlacEncoder();

  FlacEncoder(const FlacEncoder &) = delete;
  FlacEncoder &operator=(const FlacEncoder &) = delete;

  // Allocate encoder, configure, write `fLaC` magic + STREAMINFO via cb.
  // Returns false if libFLAC missing, init failed, or already active.
  bool begin(const Config &cfg, WriteCallback cb, void *user);

  // Push `frames` samples-per-channel of interleaved int16 PCM. Internal
  // int32 expansion buffer is reused across calls. Returns false on
  // encode error or write callback failure.
  bool process(const int16_t *samples, size_t frames);

  // Flush remaining samples + finalize stream. Encoder is reset; you may
  // call begin() again for the next burst on a new TCP connection.
  bool finish();

  bool active() const { return active_; }
  uint64_t samples_in() const { return samples_in_; }
  uint64_t bytes_out() const { return bytes_out_; }

  // Public forward-decl so libFLAC trampolines in flac_encoder.cpp can
  // cast their `client_data` back to Impl* without friending. Definition
  // is hidden in the .cpp file.
  struct Impl;

 private:
  Impl *impl_ = nullptr;
  bool active_ = false;
  uint64_t samples_in_ = 0;
  uint64_t bytes_out_ = 0;
};

}  // namespace cb
