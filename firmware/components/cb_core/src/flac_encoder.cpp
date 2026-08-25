// flac_encoder.cpp — libFLAC stream_encoder wrapper.
//
// CB_HAVE_LIBFLAC: when defined, real libFLAC encoder is linked in.
// When undefined, all methods are no-ops returning false so audio.cpp
// can guard at runtime with active()/begin() checks and fall back to PCM.
#include "cb/flac_encoder.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef CB_HAVE_LIBFLAC
/* cb_core builds with -Wpedantic -Werror; the libFLAC public header transitively
 * pulls ESP-IDF's <stdio.h>, which uses the `#include_next` GCC extension and
 * trips -Werror=pedantic. The warning is in the IDF/FLAC headers, not our code,
 * so silence it just for this include. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <FLAC/stream_encoder.h>
#pragma GCC diagnostic pop
#endif

namespace cb {

#ifdef CB_HAVE_LIBFLAC

struct FlacEncoder::Impl {
  FLAC__StreamEncoder *enc = nullptr;
  WriteCallback write_cb = nullptr;
  void *write_user = nullptr;
  bool write_failed = false;
  // int16 → int32 staging buffer, reused across process() calls. libFLAC
  // requires FLAC__int32 input even for 16-bit data; right-aligned signed.
  std::vector<FLAC__int32> stage;
};

namespace {

// libFLAC trampoline: forwards encoded bytes to the user callback.
// Note: `samples` parameter is the number of *audio* samples encoded into
// this byte buffer, useful for granule_pos in OGG; we ignore it.
FLAC__StreamEncoderWriteStatus on_write(const FLAC__StreamEncoder *,
                                        const FLAC__byte buffer[],
                                        size_t bytes,
                                        uint32_t /*samples*/,
                                        uint32_t /*current_frame*/,
                                        void *client_data) {
  auto *impl = static_cast<FlacEncoder::Impl *>(client_data);
  if (impl->write_failed) {
    return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
  }
  if (bytes == 0) {
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
  }
  if (!impl->write_cb(static_cast<const uint8_t *>(buffer), bytes,
                      impl->write_user)) {
    impl->write_failed = true;
    return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
  }
  return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

}  // namespace

FlacEncoder::FlacEncoder() : impl_(new Impl()) {}

FlacEncoder::~FlacEncoder() {
  if (active_) finish();
  if (impl_ && impl_->enc) {
    FLAC__stream_encoder_delete(impl_->enc);
  }
  delete impl_;
}

bool FlacEncoder::begin(const Config &cfg, WriteCallback cb, void *user) {
  if (active_ || !impl_ || !cb) return false;
  if (cfg.bits_per_sample != 16 && cfg.bits_per_sample != 24) return false;
  if (cfg.channels < 1 || cfg.channels > 2) return false;

  // Reuse encoder allocation across bursts: cheaper than new/delete each
  // time, and libFLAC supports re-init after finish().
  if (!impl_->enc) {
    impl_->enc = FLAC__stream_encoder_new();
    if (!impl_->enc) return false;
  }

  FLAC__stream_encoder_set_channels(impl_->enc, cfg.channels);
  FLAC__stream_encoder_set_bits_per_sample(impl_->enc, cfg.bits_per_sample);
  FLAC__stream_encoder_set_sample_rate(impl_->enc, cfg.sample_rate);
  FLAC__stream_encoder_set_compression_level(impl_->enc, cfg.compression_level);
  FLAC__stream_encoder_set_blocksize(impl_->enc, cfg.blocksize);
  FLAC__stream_encoder_set_verify(impl_->enc, cfg.verify);
  // Total samples 0 = unknown / streaming. STREAMINFO will lack a length
  // until finish() is reached, but we don't seek-update it (no seek_cb).
  FLAC__stream_encoder_set_total_samples_estimate(impl_->enc, 0);

  impl_->write_cb = cb;
  impl_->write_user = user;
  impl_->write_failed = false;

  FLAC__StreamEncoderInitStatus st = FLAC__stream_encoder_init_stream(
      impl_->enc, on_write,
      /*seek_cb*/ nullptr,
      /*tell_cb*/ nullptr,
      /*metadata_cb*/ nullptr, impl_);
  if (st != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
    return false;
  }

  active_ = true;
  samples_in_ = 0;
  bytes_out_ = 0;
  return true;
}

bool FlacEncoder::process(const int16_t *samples, size_t frames) {
  if (!active_ || !impl_ || !impl_->enc) return false;
  if (frames == 0) return true;

  uint32_t channels = FLAC__stream_encoder_get_channels(impl_->enc);
  size_t total = frames * channels;

  // Grow stage buffer if needed, no shrink (audio frame size is constant in
  // practice — 1920 samples per audio.cpp call).
  if (impl_->stage.size() < total) impl_->stage.resize(total);
  // 16-bit signed → 32-bit signed sign-extended.
  for (size_t i = 0; i < total; ++i) {
    impl_->stage[i] = static_cast<FLAC__int32>(samples[i]);
  }

  // bytes_out_ is updated by counting on_write's `bytes` would be cleaner,
  // but the callback can't see the wrapper without extra plumbing. Snapshot
  // via stream_encoder_get_total_samples_processed isn't exposed for bytes;
  // we let audio.cpp track network egress directly. samples_in_ is enough
  // here as a sanity counter.
  if (!FLAC__stream_encoder_process_interleaved(impl_->enc, impl_->stage.data(),
                                                static_cast<uint32_t>(frames))) {
    return false;
  }
  if (impl_->write_failed) return false;
  samples_in_ += frames;
  return true;
}

bool FlacEncoder::finish() {
  if (!active_ || !impl_ || !impl_->enc) {
    active_ = false;
    return false;
  }
  bool ok = FLAC__stream_encoder_finish(impl_->enc) && !impl_->write_failed;
  active_ = false;
  return ok;
}

#else  // CB_HAVE_LIBFLAC not defined — stub build

struct FlacEncoder::Impl {};

FlacEncoder::FlacEncoder() = default;
FlacEncoder::~FlacEncoder() { delete impl_; }
bool FlacEncoder::begin(const Config &, WriteCallback, void *) { return false; }
bool FlacEncoder::process(const int16_t *, size_t) { return false; }
bool FlacEncoder::finish() {
  active_ = false;
  return false;
}

#endif  // CB_HAVE_LIBFLAC

}  // namespace cb
