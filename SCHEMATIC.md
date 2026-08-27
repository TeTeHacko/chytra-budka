# Chytrá Budka — Wiring Schematic (rev 3.2)

> **Power:** every unit built so far is powered over **USB-C** (bench/PC,
> mains adapter or powerbank). A solar/battery charging stage is designed but
> **not yet built** — the measured power budget it has to satisfy is in
> [firmware/POWER.md](firmware/POWER.md).

> **rev3.2 changes vs rev3.1:** OV3660 camera firmware-enabled (photo-trap mode), SHT41 ambient T+RH sensor added on shared I²C bus, AM312 PIR + 940 nm IR LED added for motion-triggered captures. KiCad project lives in [`firmware/hw/`](firmware/hw/) (build via `make`). See [`firmware/hw/README.md`](firmware/hw/README.md) for KiCad library setup and pin assignment.

## Wiring diagram

> Pin assignments in the diagrams below are the rev3.2 **compile-time
> defaults**. Every D-pin is runtime-reassignable via the `pin_d{0..7}_fn`
> knobs — see the runtime pin map section in [WIRING.md](WIRING.md).

Component-level block diagram with colored wires (red = +5V/VBAT, orange = +3V3, black = GND, blue = SDA, green = SCL, goldenrod = PWM/sense, purple = PIR signal):

![wiring](firmware/hw/wiring.svg)

Source: [`firmware/hw/wiring.dot`](firmware/hw/wiring.dot) — render with `make -C firmware/hw wiring`.

## Block Diagram (as built)

```
                ┌───────────────────────────────────────────────┐
                │           IP65 ABS Box (100×68×50 mm)         │
                │                                               │
 USB-C 5 V      │  ┌────────────────────────────────────┐       │
 (mains adapter │  │XIAO ESP32-S3 Sense                 │       │
  or powerbank)─┼─►│ ● 5V / GND ◄── USB-C (gland)       │       │
                │  │ ● D4 SDA / D5 SCL ◄─► I²C bus      │       │
                │  │     (SHT41 0x44; MAX17048 0x36     │       │
                │  │      + INA226 0x40 on batt builds) │       │
                │  │ ● D1 (GPIO2, RTC) ◄── AM312 PIR    │       │
                │  │ ● D2 (GPIO3) PWM ─► MOSFET ─► IR   │       │
                │  │     LED 940 nm (night photo-trap)  │       │
                │  │ ● D3 (GPIO4) ─► capture LED        │       │
                │  │ ● PDM mic onboard (GPIO41/42;      │       │
                │  │     no ext. mic — SDIO conflict)   │       │
                │  │ ● microSD SDIO 1-bit (GPIO7/8/9)   │       │
                │  │ ● OV3660 camera (onboard, enabled) │       │
                │  │ ● WiFi U.FL ─► SMA bulkhead        │       │
                │  └────────────────────────────────────┘       │
                │                                               │
                │  Camera lens ──► lid hole (Ø10 mm), sealed    │
                │  SMA bulkhead ─► side wall (ext. antenna)     │
                │  USB cable ────► cable gland on the side      │
                │  Drain 2 mm ───► bottom (anti-condensate)     │
                └───────────────────────────────────────────────┘
                              ▲
                              │ WiFi 2.4 GHz (SMA dipole)
                              │ MQTT + OTA + audio chunks
                              ▼
                   ┌───────────────────────────────┐
                   │  server-host                  │
                   │  - audio_relay (HTTP→RTSP)    │
                   │  - BirdNET-Go (RTSP consumer) │
                   │  192.0.2.x                    │
                   └───────────────────────────────┘
```

## Mode FSM (as implemented)

Three SOC-driven modes — thresholds live in `firmware/main/config.h`, the FSM
in `firmware/components/cb_core/src/mode_fsm.cpp`:

- **Continuous** — enter at SOC ≥ 65 %, leave below 50 %.
- **Sound-triggered** — the default (also forced whenever no fuel gauge is
  detected); holds between 30 % and 65 %.
- **Safe** — enter below 30 %, recover at ≥ 35 %; audio off, WiFi power-save,
  opt-in light-sleep.

Hysteresis prevents thrashing: continuous→sound-triggered at 50 %,
sound-triggered→continuous at 65 %. Critical SOC = 30 % triggers safe mode to
protect the battery from deep discharge. None of the automatic tiers
deep-sleeps and WiFi stays associated throughout — the box remains reachable
for MQTT/OTA in every mode. What each mode does (and costs) is measured in
[firmware/POWER.md](firmware/POWER.md).

## Server-side audio relay

ESP32 streams **16 kHz stereo s16le PCM** (raw L16 per RFC 3190) in HTTP/1.1 chunked POSTs to
`relay/relay.py` on `server-host:8765` with a Bearer token. The relay
spawns one ffmpeg per stream name that re-encodes (no-op transcode for
`pcm_s16le`) and pushes RTSP into the local mediamtx instance on
`:8554`. BirdNET-Go pulls `rtsp://127.0.0.1:8554/chytra-budka` and
consumes it identically to any other RTSP source — no BirdNET-Go
config changes beyond adding the URL.

Implementation lives in `relay/`:

- single-writer guard per stream (no PCM mixing)
- `gap_silence` linger keeps the RTSP path alive across short WiFi
  flaps so BirdNET-Go does not lose its inference window
- Prometheus `/metrics`, JSON `/streams`, idle/total watchdogs
- systemd unit + idempotent `install.sh`
- host E2E test that drives the same C++ `Vad`/`ChunkedPoster` code the
  ESP32 runs against the real relay (`relay/test-e2e.sh`, 12/12)

TLS is terminated upstream (VPN tunnel or LB), so the relay itself
listens plain HTTP.
