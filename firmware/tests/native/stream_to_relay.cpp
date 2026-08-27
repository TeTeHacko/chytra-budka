// stream_to_relay.cpp — integration runner.
// Reads a 16-bit signed mono PCM file at given sample rate, drives Vad +
// ChunkedPoster + PosixTransport against a real relay endpoint at realtime
// pace. Mimics what the ESP32 firmware will do.
//
// Usage:
//   ./stream_to_relay --pcm <file> [--rate 48000] [--host 127.0.0.1]
//                     [--port 8765] [--path /audio/chytra-budka-test]
//                     [--token <bearer>] [--mode continuous|triggered]
//                     [--vad-threshold -45] [--no-realtime]
//
// Environment fallback: RELAY_AUTH_TOKEN is read if --token not given.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

#include "cb/chunked_poster.h"
#include "cb/flac_encoder.h"
#include "cb/vad.h"
#include "transport_posix.h"

namespace {

struct Args {
  const char *pcm = nullptr;
  uint32_t rate = 48000;
  const char *host = "127.0.0.1";
  uint16_t port = 8765;
  const char *path = "/audio/chytra-budka-test";
  const char *token = nullptr;
  std::string mode = "continuous";
  std::string codec = "pcm";  // pcm | flac
  float vad_threshold = -45.0f;
  bool realtime = true;
};

void usage() {
  std::fprintf(stderr,
               "usage: stream_to_relay --pcm <file> [--rate N] [--host H] [--port P]\n"
               "       [--path /audio/X] [--token T] [--mode continuous|triggered]\n"
               "       [--codec pcm|flac] [--vad-threshold dBFS] [--no-realtime]\n");
}

bool parse_args(int argc, char **argv, Args &a) {
  for (int i = 1; i < argc; i++) {
    std::string k = argv[i];
    auto need = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (k == "--pcm") a.pcm = need("--pcm");
    else if (k == "--rate") a.rate = static_cast<uint32_t>(std::atoi(need("--rate")));
    else if (k == "--host") a.host = need("--host");
    else if (k == "--port") a.port = static_cast<uint16_t>(std::atoi(need("--port")));
    else if (k == "--path") a.path = need("--path");
    else if (k == "--token") a.token = need("--token");
    else if (k == "--mode") a.mode = need("--mode");
    else if (k == "--codec") a.codec = need("--codec");
    else if (k == "--vad-threshold") a.vad_threshold = static_cast<float>(std::atof(need("--vad-threshold")));
    else if (k == "--no-realtime") a.realtime = false;
    else if (k == "-h" || k == "--help") { usage(); std::exit(0); }
    else { std::fprintf(stderr, "unknown arg: %s\n", argv[i]); return false; }
  }
  if (!a.pcm) { usage(); return false; }
  if (!a.token) a.token = std::getenv("RELAY_AUTH_TOKEN");
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse_args(argc, argv, args)) return 2;

  FILE *f = std::fopen(args.pcm, "rb");
  if (!f) {
    std::fprintf(stderr, "open %s: %s\n", args.pcm, std::strerror(errno));
    return 1;
  }

  // 20 ms per chunk to match VAD window & ESP32 DMA cadence
  const size_t chunk_samples = args.rate / 50;
  const size_t chunk_bytes = chunk_samples * sizeof(int16_t);
  std::vector<int16_t> buf(chunk_samples);

  cb::PosixTransport tx;
  cb::ChunkedPoster poster(tx);

  const bool use_flac = (args.codec == "flac");
  const char *content_type = use_flac ? "audio/flac"
                                      : "audio/L16; rate=48000; channels=1";
  if (!poster.begin(args.host, args.port, args.path, content_type,
                    args.token)) {
    std::fprintf(stderr, "POST begin failed (%s:%u %s)\n",
                 args.host, args.port, args.path);
    std::fclose(f);
    return 1;
  }
  std::fprintf(stderr, "connected to %s:%u, posting %s to %s\n", args.host,
               args.port, content_type, args.path);

  // FLAC encoder is set up only when needed. Its write callback funnels
  // encoded bytes into the same poster used for PCM, so the rest of the
  // loop is identical (transmit / skip via VAD).
  cb::FlacEncoder flac;
  if (use_flac) {
    cb::FlacEncoder::Config fc;
    fc.sample_rate = args.rate;
    fc.channels = 1;
    fc.bits_per_sample = 16;
    fc.blocksize = 4096;
    fc.compression_level = 5;
    auto write_cb = [](const uint8_t *data, size_t bytes,
                       void *user) -> bool {
      auto *p = static_cast<cb::ChunkedPoster *>(user);
      return p->send(data, bytes);
    };
    if (!flac.begin(fc, write_cb, &poster)) {
      std::fprintf(stderr, "FLAC encoder init failed (libFLAC missing?)\n");
      poster.end();
      std::fclose(f);
      return 1;
    }
  }

  cb::Vad::Config vcfg;
  vcfg.threshold_dbfs = args.vad_threshold;
  vcfg.window_samples = chunk_samples;  // one window per chunk
  vcfg.sample_rate = args.rate;
  cb::Vad vad(vcfg);

  const bool triggered_mode = (args.mode == "triggered");
  const auto chunk_dur =
      std::chrono::microseconds(static_cast<int64_t>(1000000.0 * chunk_samples / args.rate));

  auto start = std::chrono::steady_clock::now();
  uint32_t now_ms = 0;
  uint64_t total_in = 0, total_sent = 0;
  uint64_t chunks_in = 0, chunks_sent = 0;

  while (true) {
    size_t got = std::fread(buf.data(), sizeof(int16_t), chunk_samples, f);
    if (got == 0) break;
    if (got < chunk_samples) {
      std::memset(buf.data() + got, 0, (chunk_samples - got) * sizeof(int16_t));
    }
    total_in += chunk_bytes;
    chunks_in++;

    bool transmit = true;
    bool active = vad.update(buf.data(), chunk_samples, now_ms);
    if (triggered_mode) transmit = active;

    if (transmit) {
      bool ok;
      if (use_flac) {
        ok = flac.process(buf.data(), chunk_samples);
      } else {
        ok = poster.send(buf.data(), chunk_bytes);
      }
      if (!ok) {
        std::fprintf(stderr, "send failed at t=%u ms\n", now_ms);
        break;
      }
      total_sent += chunk_bytes;
      chunks_sent++;
    }

    if (chunks_in % 50 == 0) {
      std::fprintf(stderr, "  t=%5u ms  rms=%6.1f dBFS  active=%d  sent=%llu/%llu chunks\n",
                   now_ms, vad.last_rms_dbfs(), active ? 1 : 0,
                   static_cast<unsigned long long>(chunks_sent),
                   static_cast<unsigned long long>(chunks_in));
    }

    now_ms += static_cast<uint32_t>(chunk_dur.count() / 1000);
    if (args.realtime) {
      auto target = start + std::chrono::microseconds(chunk_dur.count() * chunks_in);
      std::this_thread::sleep_until(target);
    }
  }

  if (use_flac) flac.finish();
  poster.end();
  std::fclose(f);

  std::fprintf(stderr,
               "\nsummary:\n"
               "  chunks read:  %llu (%llu bytes)\n"
               "  chunks sent:  %llu (%llu bytes)\n"
               "  bursts:       %u\n"
               "  status code:  %d\n",
               static_cast<unsigned long long>(chunks_in),
               static_cast<unsigned long long>(total_in),
               static_cast<unsigned long long>(chunks_sent),
               static_cast<unsigned long long>(total_sent),
               vad.burst_count(), poster.status_code());
  return 0;
}
