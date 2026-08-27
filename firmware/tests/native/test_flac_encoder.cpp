// test_flac_encoder.cpp — validates cb::FlacEncoder via in-memory roundtrip.
//
// Flow: synthetic PCM → FlacEncoder → in-memory FLAC stream → libFLAC stream
// decoder → decoded PCM → assert bit-exact equality with input. Also asserts
// compression ratio matches the rough expectation for the input class
// (sine wave compresses very well; white noise barely at all).
//
// Build: needs libFLAC (`-lFLAC`) and CB_HAVE_LIBFLAC defined; run from
// firmware/tests/native via `make test`.
#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "cb/flac_encoder.h"

namespace {

constexpr uint32_t SR = 48000;
constexpr uint8_t CH = 1;

// ─── encoder-side sink: collect FLAC bytes into a vector ────────────────

bool sink_write(const uint8_t *p, size_t n, void *user) {
  auto *v = static_cast<std::vector<uint8_t> *>(user);
  v->insert(v->end(), p, p + n);
  return true;
}

// ─── decoder-side: feed vector via read callback, accumulate samples ─────

struct DecoderCtx {
  const uint8_t *data;
  size_t len;
  size_t pos;
  std::vector<int16_t> decoded;
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  uint32_t bps = 0;
  bool error = false;
};

FLAC__StreamDecoderReadStatus on_read(const FLAC__StreamDecoder *,
                                      FLAC__byte buffer[], size_t *bytes,
                                      void *user) {
  auto *ctx = static_cast<DecoderCtx *>(user);
  size_t avail = ctx->len - ctx->pos;
  if (avail == 0) {
    *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
  size_t take = std::min(*bytes, avail);
  std::memcpy(buffer, ctx->data + ctx->pos, take);
  ctx->pos += take;
  *bytes = take;
  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus on_decoded(const FLAC__StreamDecoder *,
                                          const FLAC__Frame *frame,
                                          const FLAC__int32 *const buffer[],
                                          void *user) {
  auto *ctx = static_cast<DecoderCtx *>(user);
  uint32_t n = frame->header.blocksize;
  // mono only in this test; channels asserted in metadata callback
  for (uint32_t i = 0; i < n; ++i) {
    ctx->decoded.push_back(static_cast<int16_t>(buffer[0][i]));
  }
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void on_metadata(const FLAC__StreamDecoder *,
                 const FLAC__StreamMetadata *meta, void *user) {
  if (meta->type != FLAC__METADATA_TYPE_STREAMINFO) return;
  auto *ctx = static_cast<DecoderCtx *>(user);
  ctx->sample_rate = meta->data.stream_info.sample_rate;
  ctx->channels = meta->data.stream_info.channels;
  ctx->bps = meta->data.stream_info.bits_per_sample;
}

void on_error(const FLAC__StreamDecoder *,
              FLAC__StreamDecoderErrorStatus status, void *user) {
  auto *ctx = static_cast<DecoderCtx *>(user);
  ctx->error = true;
  std::fprintf(stderr, "FLAC decoder error: %s\n",
               FLAC__StreamDecoderErrorStatusString[status]);
}

std::vector<int16_t> decode_flac(const std::vector<uint8_t> &flac_bytes,
                                 DecoderCtx &ctx) {
  ctx.data = flac_bytes.data();
  ctx.len = flac_bytes.size();
  ctx.pos = 0;

  FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
  assert(dec);
  FLAC__StreamDecoderInitStatus st = FLAC__stream_decoder_init_stream(
      dec, on_read, /*seek*/ nullptr, /*tell*/ nullptr, /*length*/ nullptr,
      /*eof*/ nullptr, on_decoded, on_metadata, on_error, &ctx);
  assert(st == FLAC__STREAM_DECODER_INIT_STATUS_OK);
  bool ok = FLAC__stream_decoder_process_until_end_of_stream(dec);
  assert(ok && !ctx.error);
  FLAC__stream_decoder_finish(dec);
  FLAC__stream_decoder_delete(dec);
  return std::move(ctx.decoded);
}

// ─── PCM generators ──────────────────────────────────────────────────────

std::vector<int16_t> make_sine(size_t frames, double hz, double amp = 0.4) {
  std::vector<int16_t> v(frames);
  for (size_t i = 0; i < frames; ++i) {
    double s = std::sin(2.0 * M_PI * hz * static_cast<double>(i) / SR);
    v[i] = static_cast<int16_t>(s * amp * 32767.0);
  }
  return v;
}

std::vector<int16_t> make_silence(size_t frames) {
  return std::vector<int16_t>(frames, 0);
}

std::vector<int16_t> make_noise(size_t frames, uint32_t seed = 0xC0FFEEu) {
  std::vector<int16_t> v(frames);
  uint32_t s = seed;
  for (size_t i = 0; i < frames; ++i) {
    // xorshift32, plenty random for compression-ratio assertion
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    v[i] = static_cast<int16_t>(s & 0xFFFF);
  }
  return v;
}

// Bird-call surrogate: ~70 % silence, ~30 % chirp bursts at 4-8 kHz.
// Models the actual VAD-windowed audio that will hit the encoder in the
// field — sparse harmonics + long quiet stretches.
std::vector<int16_t> make_bird_like(size_t frames) {
  std::vector<int16_t> v(frames, 0);
  size_t i = 0;
  uint32_t seed = 0xB12Du;
  while (i < frames) {
    // jump ahead 0.5-1.5 s of silence
    seed = seed * 1103515245u + 12345u;
    size_t silence = SR / 2 + (seed % (SR));
    i += silence;
    if (i >= frames) break;
    // ~150 ms chirp, 4 kHz → 8 kHz sweep
    size_t burst = SR * 15 / 100;
    for (size_t j = 0; j < burst && i < frames; ++j, ++i) {
      double t = static_cast<double>(j) / SR;
      double f = 4000.0 + 4000.0 * static_cast<double>(j) / burst;
      double env = std::sin(M_PI * static_cast<double>(j) / burst);  // hann
      v[i] = static_cast<int16_t>(0.6 * env *
                                  std::sin(2.0 * M_PI * f * t) * 32767.0);
    }
  }
  return v;
}

// ─── single-shape roundtrip helper ───────────────────────────────────────

void roundtrip(const char *label, const std::vector<int16_t> &pcm,
               double expected_min_compress, double expected_max_compress) {
  std::vector<uint8_t> flac;
  cb::FlacEncoder enc;
  cb::FlacEncoder::Config cfg;
  cfg.sample_rate = SR;
  cfg.channels = CH;
  cfg.verify = true;  // libFLAC's own decode-verify on every frame

  bool ok = enc.begin(cfg, sink_write, &flac);
  assert(ok);

  // Push in 1920-sample chunks, mirroring audio.cpp's frame size.
  constexpr size_t FRAME = 1920;
  size_t pos = 0;
  while (pos < pcm.size()) {
    size_t take = std::min(FRAME, pcm.size() - pos);
    ok = enc.process(pcm.data() + pos, take);
    assert(ok);
    pos += take;
  }
  ok = enc.finish();
  assert(ok);
  assert(!enc.active());
  assert(enc.samples_in() == pcm.size());

  // Decode back
  DecoderCtx ctx;
  std::vector<int16_t> decoded = decode_flac(flac, ctx);
  assert(ctx.sample_rate == SR);
  assert(ctx.channels == CH);
  assert(ctx.bps == 16);
  assert(decoded.size() == pcm.size());

  // Bit-exact lossless check
  for (size_t i = 0; i < pcm.size(); ++i) {
    if (decoded[i] != pcm[i]) {
      std::fprintf(stderr,
                   "[%s] mismatch at sample %zu: got %d want %d\n", label, i,
                   decoded[i], pcm[i]);
      std::abort();
    }
  }

  // Compression ratio sanity: flac_bytes / pcm_bytes
  size_t pcm_bytes = pcm.size() * sizeof(int16_t);
  double ratio = static_cast<double>(flac.size()) / pcm_bytes;
  std::printf(
      "  %-12s pcm=%6zu B flac=%6zu B ratio=%.3f (expect %.2f-%.2f)\n",
      label, pcm_bytes, flac.size(), ratio, expected_min_compress,
      expected_max_compress);
  assert(ratio >= expected_min_compress);
  assert(ratio <= expected_max_compress);
}

}  // namespace

int main() {
  std::printf("== FlacEncoder roundtrip ==\n");

  // 5 s of each signal at 48 kHz mono = 240 000 samples.
  constexpr size_t N = SR * 5;

  // Bounds were calibrated against libFLAC 1.5.0 default L5 on this synthetic
  // signal set; they exist to catch regressions, not to be tight specs.
  // Pure silence: dominated by stream + STREAMINFO header. Measured ~0.002.
  roundtrip("silence", make_silence(N), 0.0005, 0.01);
  // 440 Hz sine: long-period predictable. Measured ~0.22.
  roundtrip("sine_440", make_sine(N, 440.0), 0.10, 0.30);
  // 8 kHz sine: shorter period, still predictable. Measured ~0.07.
  roundtrip("sine_8k", make_sine(N, 8000.0), 0.03, 0.15);
  // Bird-like (sparse chirps + ~70 % silence): measured ~0.07.
  roundtrip("bird_like", make_bird_like(N), 0.02, 0.20);
  // White noise: encoder unable to predict; measured ~1.00.
  roundtrip("white_noise", make_noise(N), 0.85, 1.05);

  // Multi-burst lifecycle: encoder must work after begin/finish/begin.
  // Each 1-s slice is mostly silence in the bird-like generator.
  std::printf("== multi-burst reuse ==\n");
  for (int burst = 0; burst < 3; ++burst) {
    roundtrip("burst", make_bird_like(SR * 1), 0.001, 0.20);
  }

  std::printf("== all FLAC tests passed ==\n");
  return 0;
}
