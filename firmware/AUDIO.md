# Audio output

The ESP32-S3 has **no built-in DAC**. Chytrá Budka therefore has two independent
audio-output paths, both exposed through the **pin-function map** (`pin_dN_fn`,
see `app_config.c`) and both **off by default** — they claim a GPIO only when a
pad is deliberately mapped to them, so unmodified / field boards stay silent.

| Path | Pin function | Module | Signal | Drives |
| ---- | ------------ | ------ | ------ | ------ |
| Chiptune | `buzzer` | `speaker.c` | LEDC square wave | active buzzer / passive piezo / small speaker (via R or transistor) / line-in |
| PCM samples *(experimental)* | `pcm` | `pcm.c` | I²S PDM TX (1-bit sigma-delta) | a DIY "1-bit DAC" = external **RC low-pass + amp** |

Map either to any D0–D7 pad: `cmd/cfg/pin_dN_fn buzzer` / `… pcm`. Both are
singleton functions, and **both can be mapped at once** — wire a buzzer to one
pad and a PCM amp to the other.

## Both at once — the `audiofx` fan-out (audiofx.c)

Every "make a sound" site goes through **`audiofx_*`** (audiofx.c), which fans
each notification out to **both** backends, so it plays on whatever is wired:

- `speaker.c` renders it as a chiptune square wave on the `buzzer` pad;
- `pcm.c` renders it on the `pcm` pad — the **embedded sample** for the coin,
  or **square (chiptune) tones synthesised from the same note list** for
  everything else (so melodies/beeps come out of the PDM "1-bit DAC" too, not
  just the buzzer; a square is louder and brighter through a tiny speaker than
  a pure sine).

Each backend self-gates on its pin mapping, so an unmapped output is a no-op.
**Everything** flows through `audiofx_*`: the boot jingle (`audiofx_boot()`,
fired from `app_main` once both backends are up), the capture beep, the `cmd/*`
MQTT sounds, and the **lifecycle cues** — OTA start / done / fail
(`audiofx_ota_*`) and SoftAP config-mode up (`audiofx_ap()`).

A loop (`cmd/alarm`) owns the output until stopped; one-shot cues that arrive
while a loop is playing are dropped rather than queued (so they can't burst out
when the loop ends).

---

## 1. Chiptune — `buzzer` (speaker.c)

1-bit square wave on one GPIO via LEDC (own timer/channel, distinct from the IR
LED and camera XCLK). Drives an active buzzer directly, or a passive
piezo/speaker through a series resistor / small amp.

**`spkr_tone` (NVS bool, read live):**
- **OFF** = active/self-drive buzzer — pad held full-on for each note; pitch is
  the buzzer's own, so `freq` only gates on/off. Adjacent notes get a short gap
  so melodies come out as rhythm instead of one merged tone.
- **ON** = tone — the LEDC frequency follows the note (passive piezo / speaker /
  line-in). Notes are slightly staccato (a few ms carved out) so a melody
  articulates instead of slurring.

**MQTT commands** (payload format `f:ms,f:ms,…`; `f` Hz, `0` = rest):
- `cmd/beep "freq,ms"` — one tone.
- `cmd/melody "f:ms,…"` — play once (empty → built-in demo).
- `cmd/sfx "f:ms,…"` — play once **legato** (smooth, no staccato — for effects).
- `cmd/alarm "f:ms,…"` — **loop** until `cmd/alarm stop` (or `off`/empty).

**Built-in sounds:** boot chime = SMB *power-up* sweep; capture beep
(`cap_beep_en`) = SMB *coin* on every photo.

**Wiring:** `pad → (series R ~150–470 Ω, or a small NPN/MOSFET for volume) →
speaker → GND`. A square wave is robust — a simple amp follows it directly.

---

## 2. PCM samples — `pcm` (pcm.c) — EXPERIMENTAL

Plays flash-embedded **16-bit / 16 kHz mono PCM** out one GPIO as a hardware
**I²S PDM TX** (1-bit sigma-delta) bitstream — the firmware half of a DIY
"1-bit DAC". With an external **RC low-pass + amp** on that pad it reconstructs
real sound samples (unlike the square-wave chiptune).

- I²S PDM TX in **codec one-line mode** (`I2S_PDM_TX_*_DEFAULT_CONFIG`,
  `data_fmt = PCM`) — this runs the hardware **PCM→PDM sigma-delta converter**,
  so the 16-bit samples come out as a properly modulated 1-bit bitstream that a
  plain RC low-pass reconstructs to clean analog. **Do *not* use "DAC line
  mode"** (`I2S_PDM_TX_*_DAC_DEFAULT_CONFIG`): that is `data_fmt = RAW`, which
  *disables* the converter and expects pre-encoded raw PDM bits — feed PCM into
  it and the RC only averages the raw int16 words into a faint, distorted ghost
  (`náznak zvuku`, brutally quiet). That was the original bring-up bug.
- Own I²S controller, coexists with the PDM-RX microphone (verified: no mic
  regression). `clk` left unused — an RC-filter DAC only needs `dout`.
- **MQTT:** `cmd/pcm coin` (embedded sample) · `cmd/pcm test` (1 kHz sine loop,
  for tuning the RC) · `cmd/pcm stop`. `cmd/pcm` also mirrors the equivalent
  chiptune onto the `buzzer` pad, so a sound comes out whichever pad the amp is
  switched to.
- **Samples:** `coin.pcm` (SMB coin, from `smb_coin.wav`; resampled to 16 kHz,
  loudness-limited). Embedded via `EMBED_FILES`. Add more the same way:
  convert a WAV → 16-bit/16 kHz s16le → `EMBED_FILES` → a case in
  `pcm_play_named()`.

### The DIY "1-bit DAC" hardware
```
pad ──[ R1 ~220–470Ω ]──┬──[ R2 ~220–470Ω ]──┬──[ C 1–10µF ]──► amp → speaker
                        │                    │
                     [ 100nF ]            [ 100nF ]
                        │                    │
                       GND                  GND
```
2nd-order RC low-pass. With 470 Ω + 100 nF, `fc ≈ 3.4 kHz`. The PDM carrier in
DAC mode is `Fpdm ≈ 6.144 MHz`, so even a ~3 kHz RC annihilates it (~130 dB).
**The RC is mandatory:** raw PDM into a speaker is a harsh rasp, and a linear
amp can't follow the MHz bitstream (averages to silence).

### Known limitations (by design — not bugs)
- **PDM, not standard I²S.** This does **not** drive a standard I²S DAC/amp
  (MAX98357A, PCM5102) as-is — those want I²S `std` mode (BCLK/WS/DIN, 3 pins).
  The sample-playback layer (`pcm_play`, embedded samples, MQTT triggers, pin
  map) is backend-agnostic, so adding I²S-std support is a localized change in
  `pcm_init` — deliberately **not** implemented (no such hardware on hand to
  test against; don't ship blind FW for absent HW).
- **Volume** is limited by the analog amp: a unity-gain emitter follower has no
  voltage gain, so a reconstructed sample is quieter than the full-rail square
  chiptune. Firmware already maxes the sample level (makeup gain + soft limiter
  in the sample, full-scale sine test). For more, add a common-emitter gain
  stage, or move to a real I²S amp (see above).
- **Bandwidth** is whatever the RC passes (~3.4 kHz with 470 Ω/100 nF) — fine
  for SFX, muffles highs. A higher cutoff (smaller R/C) brightens it.

---

## Testing

**Native** (`firmware/tests/native`, host gcc — `make test`):
- `test_melody` — the `melody_parse()` payload parser (extracted to `melody.c`
  so it's host-testable): clamping, default ms, rests, leading separators,
  stop-on-garbage, the `max` bound, NULL-safety.

**HIL** (`firmware/tests/hil/test_audio.py` + `audio_matrix.toml`) — audio is
hardware-shaped, so it's *matrix-driven*: declare which wiring is on the bench
and only that variant runs, the rest skip (you never reconfigure for hardware
that isn't there). The firmware exposes `audio_buzzer_gpio` / `audio_pcm_gpio`
in `/selftest` so the host can see which backend came up on which pad.

```
CB_HIL_NO_RESET=1 CB_BENCH_IP=<ip> CB_AUDIO_WIRED=both \
    pytest -m manual firmware/tests/hil/test_audio.py
```
Per variant it: applies the pin map → reboots → asserts the backends init'd on
the expected GPIOs → drives every sound (`beep/melody/sfx/pcm/alarm` + capture
beep) asserting the board never wedges → checks mic/camera coexistence. It can't
*hear* (no mic on the host), so it prints `▶ LISTEN` cues for the by-ear sign-off.

## Notes
- Both paths are NVS-gated via the pin map → safe to ship to the field (inert
  until a pad is mapped). PCM additionally costs ~30 KB flash per embedded clip.
