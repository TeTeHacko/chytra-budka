# Native test harness

Pure C++17 host build of the firmware core (`firmware/components/cb_core/`)
— runs on the laptop without an ESP32, lets us iterate on logic at full
speed.

## Layout

- `test_vad.cpp` — RMS thresholding, burst latching, rearm timing
- `test_vad_wrap.cpp` — `uint32_t millis()` wrap-around (~49 days) safety
- `test_mode_fsm.cpp` — battery-SOC mode hysteresis transitions
- `test_chunked_post.cpp` — HTTP chunked POST framing (loopback transport)
- `test_chunked_errors.cpp` — refused / mid-stream / end-after-fail / garbage / long-status error paths
- `test_flac_encoder.cpp` — FLAC encoder wrapper (links system libFLAC)
- `test_sd_layout.c` — SD date-tree path + retention policy (date paths, oldest-bucket ordering, age cutoff, page-slice overflow, filename validation)
- `test_csr.c` — CSR build (links system mbedTLS 3.6.x — host stack, not the IDF 4.0.0 target)
- `test_san_fp.c` — SAN fingerprint computation
- `stream_to_relay.cpp` — integration runner: reads 16-bit mono PCM from
  a file and pushes it to the real relay over TCP at realtime pace,
  exactly the way the ESP32 firmware does

## Build & test

    make             # build everything (unit binaries + integration runner)
    make test        # build + run the unit suites (test_csr/test_san_fp only
                     #   when host mbedTLS >= 4; skipped with a notice otherwise)
    make stream-to-relay   # only build the integration runner

## Integration run (against running relay on server-host)

    # 60 s 440 Hz sine, continuous mode (always transmit)
    export RELAY_AUTH_TOKEN=<token>
    ./build/stream_to_relay \
        --pcm /tmp/opencode/sine60.pcm \
        --rate 48000 \
        --host 127.0.0.1 --port 8765 \
        --path /audio/chytra-budka-test \
        --mode continuous

    # Triggered mode: only forwards while VAD is "active"
    ./build/stream_to_relay --pcm bird.pcm --mode triggered --vad-threshold -45

While this is running, BirdNET-Go (or any RTSP client) can pull
`rtsp://127.0.0.1:8554/chytra-budka-test`.

For a one-shot, fully isolated end-to-end loop (relay + fake ffmpeg +
all four FW scenarios + Prometheus assertions), the server-side `relay`
component ships a `test-e2e.sh` that builds this runner and drives it for
you. (The relay is part of the author's server-side stack — see the note
in the top-level README; it isn't published in this repo yet.)

## Why this matters

The ESP32 runs the same `vad.cpp`, `chunked_poster.cpp`, and `mode_fsm.cpp`
sources behind a thin IDF-side `Transport` adapter. Bugs in framing,
VAD thresholding, or mode transitions get caught here in seconds rather
than after a full flash + serial-monitor session.
