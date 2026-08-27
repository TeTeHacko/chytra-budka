# Power budget & sleep architecture

> The **measured** power reality of this hardware/firmware — real load from
> live telemetry, why the board never sleeps by default, what light-sleep
> actually saves, and the sizing math for a future solar/battery stage.
> Everything with a number in this doc was measured on the fielded hardware;
> the solar/battery charging stage itself is designed but **not yet built**.

## TL;DR

- The board draws a **flat ~1.1 W (≈27 Wh/day)** continuously — measured, not
  estimated. It **never deep-sleeps and barely modem-sleeps**; it is designed
  "always-on, always-listening, always-reachable."
- Both fielded units currently run on **USB** (no battery present — fuel gauge
  reads the −1 sentinel). The "solar" telemetry is a toy USB source, not the
  planned 18 V/10 W panel.
- At ~27 Wh/day the load — not the cell choice — is the dominant problem. A
  10 W panel + 3× 18650 is a **summer-only** rig with ~1 day of buffer; CZ
  winter can't sustain it.
- The highest-leverage fix is **firmware power management**, but it forces a
  product trade-off: you can have any **two of** {24/7 audio, always-reachable,
  low power}. Deep sleep means giving up the first two.

## Measured load (live Mimir, 2026-06-10)

Source: `cb_*` metrics in the `mimir` datasource, fed by `metrics-bridge/cbprom.py`
from the UC96 BT power meters. Telemetry is **sparse** (BT meters drop in/out),
but the load figure is robust because the load itself is stable.

| Metric (field unit `budka-example-1` / `ex02`) | Value |
| --- | --- |
| `cb_meter_power_watts` — avg over 7 d | **1.14 W** @ ~5.04 V (≈ 0.23 A) |
| min / max over 7 d | 0.86 / **1.51 W** |
| max over 30 d | **1.51 W** (same — see "flat across modes") |
| derived daily energy | **≈ 27 Wh/day** |
| `cb_battery_voltage_volts` / SOC | **−1** (sentinel) → no MAX17048 → running on USB |
| `cb_mode_info` | `triggered` now; `continuous` *was* active within 30 d |
| `solar-example-1` meter | avg 0.68 W, peak 1.44 W, 4.44 V, sparse → **not** the 18 V/10 W panel |

Dev unit `budka-example-2` / `ex01` independently agrees: `avg_over_time(...[7d])`
= **1.07 W**. Two units, same ~1.1 W.

**Flat across modes:** `continuous` mode was active within the last 30 d, yet
peak power never exceeded 1.51 W (same as the 7 d peak). The firmware *does*
differentiate modes (PS policy + stream vs VAD-gate, below), but the
always-on baseline — I²S frame read every 32 ms + VAD DSP at 240 MHz + WiFi
associated — dominates whether audio frames are actually TX'd or not.

> Sub-second WiFi-TX / camera-capture current spikes (≈300–500 mA) are not
> captured by the UC96 sampling, but for **energy** budgeting the averaged
> figure is what matters, so ~27 Wh/day stands.

## Why it never sleeps (firmware, with citations)

| Aspect | Finding | Evidence |
| --- | --- | --- |
| PM / DFS / tickless idle | **All disabled** (deliberately — DFS breaks LCD_CAM XCLK on the S3, camera SCCB NACK) | `sdkconfig.defaults.esp32s3:44-52`, `sdkconfig.defaults:104-112` |
| CPU frequency | **Fixed 240 MHz**, no scaling | `sdkconfig.defaults.esp32s3:42` |
| Deep sleep | **Never invoked.** But PIR ext0 wake is already stubbed, and audio has clean `audio_begin()/audio_end()` lifecycle → building blocks exist | `main/pir.c:244` (`esp_sleep_enable_ext0_wakeup`), `main.cpp:279-289` |
| WiFi power save | `WIFI_PS_MIN_MODEM` default; **`WIFI_PS_NONE` in Continuous** (so DTIM doesn't chunk the PCM stream), `MIN_MODEM` in Triggered/Safe/Boot | `wifi_mgr.c:277`, `main.cpp:291-299` |
| Audio task | Pinned CPU1, prio 10; reads a frame **every 32 ms** (512 samples @ 16 kHz). Continuous = send every frame to BirdNET relay; Triggered = VAD-gated send; Safe/Boot = idle 200 ms | `audio.cpp:614-645`, `audio.cpp:533-549` |
| Mode FSM | **SOC-only input, no time-of-day.** Gates the audio pipeline + WiFi PS — **not** the CPU or the WiFi/MQTT connection | `components/cb_core/src/mode_fsm.cpp` |
| BLE | Compiled into every image but **default OFF** (`ble_enabled` NVS knob); BT controller not initialised when off → zero power impact | `ble.c:493-509`, `sdkconfig.defaults.esp32s3` (BLE block) |
| Always-on tasks | CPU0: WiFi(23), LWIP tiT(18, pinned CPU0), esp-mqtt, HTTP listener, glitchtip(3), photo_queue(3), status_led(2), boot-button(1, 200 ms poll). CPU1: audio(10), camera_worker(5), pir(5), reed(5) | various |

### The "two of three" triangle

The firmware prioritises being **always reachable** (OTA-only field units must
be pushable any time) and **always listening** (24/7 BirdNET). Those two
directly preclude low power:

1. **24/7 audio** — I²S DMA + a persistent chunked HTTP POST to the BirdNET
   relay. Can't deep-sleep mid-stream; clocks die, stream tears down.
2. **Always reachable** — WiFi stays associated, MQTT keepalive, HTTP listener.
   Tearing these down loses remote recovery / OTA.
3. **Low power** — needs deep/light sleep, which kills (1) and (2).

You can have **any two**. Deep sleep = give up (1) and (2).

## Power modes — what each mode does (implemented 2026-06-10)

Rather than add a separate "power profile" selector, power saving is wired into
**what each existing FSM mode does** (`mode_override` stays the manual switch:
`auto` / `triggered` / `continuous` / `safe`). The two deployment scenarios map
onto modes the operator pins:

| Scenario | Pin (`mode_override`) | Behaviour |
| --- | --- | --- |
| **Mains + battery backup** | `continuous` (or `auto`) | 24/7 audio stream, WiFi `PS_NONE`, normal telemetry — today's behaviour, no saving needed |
| **Solar — normal** | `triggered` | VAD-gated audio + **active-hours window** (mic off outside it), WiFi `MIN_MODEM`, telemetry `tlm_trig_s` |
| **Solar — survival / low battery** | `safe` | Audio **off**, WiFi `MAX_MODEM` (aggressive doze, still associated), telemetry throttled to `tlm_safe_s` heartbeat, light-sleep (if enabled) |

New NVS knobs (HA-exposed, `cmd/cfg/<key>`):

- `audio_on_h` / `audio_off_h` — local-wall-clock active-hours window for the
  mic in audio-running modes. `on == off` ⇒ **disabled (always-on)**, the
  default, so a fielded unit is unchanged on OTA until an operator sets a
  window (e.g. `5`/`21`). Wraps midnight (`22`/`6`). Logic in `cb::audio_window_open`
  (`components/cb_core/.../power_mode.{h,cpp}`, native-tested). **Fails OPEN**
  before SNTP sync so a bad clock never silences the mic.
- `tlm_safe_s` — telemetry period in Safe (default 900 s heartbeat).
- `pm_lightsleep` — automatic light-sleep, built into every image but **default
  OFF** (opt-in per unit). Engages only in Safe (audio off) + BLE off; inert in
  Continuous (audio lock). On-by-default crash-looped under HIL load — see below.

`state/audio_active` (retained, ON/OFF) reports whether the mic is currently
meant to be running — visible in HA and used to correlate `cb_meter_power_watts`
against the window in Grafana.

**Honest expectation (from the flat-1.1 W finding above):** the *only* lever that
visibly moves the meter is the mic being **off** — i.e. Safe, or Triggered while
its window is closed. WiFi-PS (`MIN`→`MAX_MODEM`) and the telemetry throttle are
near-noise on the UC96 average; they're shipped because the survival posture
wants them, not because they move watts. The saving is **daily Wh** (mic zeroed
for N off-hours), not peak W. A solar box that still listens 24/7 measures ~1.1 W.

### Light-sleep — UNIVERSAL image, but DEFAULT OFF (opt-in per unit)

Light-sleep is compiled into **every image** (PM in the shared `.esp32s3` overlay;
no separate profile) — one FW for all units. But the runtime knob `pm_lightsleep`
**defaults OFF**: it is enabled per unit (solar) after a per-unit load+capture
burn-in, NOT on across the fleet. Behaviour, once enabled, is **mode-driven**:
Continuous/Triggered hold a `NO_LIGHT_SLEEP` lock while audio runs → full power +
function (mains units never sleep); Safe (audio off) → the SoC light-sleeps
between WiFi DTIM wakes (~0.40 W, ~-57%) while staying reachable + PIR→photo. The
mode (`mode_override`, or future SOC-auto) picks. `cb_pm.c` pins `esp_pm` at
**min == max == 240 MHz, NO DFS** (OV3660 XCLK can't drop).

**Why default OFF (HIL, 2026-06-11):** with `pm_lightsleep` on-by-default, the
fleet HIL gate crash-looped — `CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` allocates
CPU-retention RAM on every sleep entry, and under active load (TLS handshakes,
captures, mode-churn) that competes with the working heap → `sleep_cpu_configure:
Failed to enable CPU power down` → OOM → `task_wdt` reboot → safe-mode. Idle-only
testing missed it. Fixes applied: **CPU power-down disabled in light sleep**
(`# CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` — clock-gating only, a few mW dearer
but stable), **and the knob defaults OFF** so a unit only light-sleeps after it's
been load-validated. Robustness over the convenience of fleet-wide on-by-default.

Four HIL findings shaped it: (1) PM+tickless perturbs the idle-task TWDT feed →
`boot after task_wdt`; fixed by **unsubscribing the idle tasks from the TWDT**
(`# CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1`; hot-path tasks stay watched). Tick
stays at the stock **100 Hz** (no global timing change); saving-mode poll-
lengthening (`app_lowpower_active`) clears the sleep threshold. (2) Light sleep
stops clocks entirely, so the OV3660's first frame after wake is torn — fixed by a
4-frame post-wake drain in `camera_capture_event`. (3) NimBLE eats ~90 KB and the
BT controller blocks light sleep, so **light-sleep needs BLE off** (`ble_enabled`
defaults OFF; self-metering moves to the host UC96 exporter). With BLE on the FW
still boots (verified, int_free ~17 KB) but RAM is tight. (4) the CPU-power-down
OOM crash-loop above.


> The mains field unit, on this same FW in Continuous, never sleeps (audio lock) →
> behaviour unchanged. Phase 1 (windowing + per-mode WiFi PS / telemetry) rides
> along in the same image.

## Bench verification (2026-06-10) + measured table

Phase 1 was flashed to the bench (`ex01`) and verified live over MQTT:
- `mode_override=safe` → `state/mode=safe`, `state/audio_active=OFF` (mic torn down).
- `mode_override=triggered`, window disabled (`audio_on_h==audio_off_h==0`) →
  `audio_active=ON`. Set a closed window (e.g. `08`/`10` while the local clock read
  ~21 h) → `audio_active=OFF` **while `state/mode` stayed `triggered`** — i.e. the
  window gates the mic, not the FSM, exactly as designed. Reopen → `ON`. The board's
  clock was SNTP-synced (no fail-open), and cmd/cfg was reliable.

### Measured A/B (bench `ex01`, UC96 meter `aabbcc000003`, 2026-06-10 ~19:14–19:28 UTC)

Held each posture ~5–8 min via `cmd/cfg/mode_override`, read
`avg/min/max_over_time(cb_meter_power_watts{device="ex01"}[…])` in Mimir.

| Posture | avg W | min / max W | notes |
| --- | --- | --- | --- |
| **Continuous — mic ON** | **0.93** | 0.63 / 1.08 | bench wasn't streaming (no relay), so ≈ Triggered-mic-on |
| **Safe — mic OFF** (+MAX_MODEM) | **0.95** | 0.73 / 1.37 | `audio_end()` confirmed (`audio_active=OFF`) |
| Triggered, window OPEN | ~0.93 | — | not separately run — identical mic-on state as Continuous |
| Triggered, window CLOSED | ~0.95 | — | not separately run — same mic-OFF state as Safe |
| Safe + `pm_lightsleep` ON | _n/a_ | _n/a_ | PM compiled out (task_wdt, see above); the only lever that would move this floor |

**Headline finding — mic-off does NOT measurably cut power on this hardware.**
Safe (mic off) measured **0.95 W**, i.e. *not below* Continuous (mic on, 0.93 W) —
the two are identical within meter noise (Δ≈0.02 W; the UC96 jitters ±~0.1 W and
both windows swing 0.6–1.4 W, so anything under ~0.1 W is below resolution). This
**directly confirms the "Flat across modes" thesis**: with the CPU pinned at
240 MHz (no DFS) and WiFi always associated, the always-on floor dominates and the
I²S-read + VAD-DSP load is buried in the noise. Stopping the audio pipeline frees
CPU1 cycles but doesn't lower the 5 V draw because nothing else scales down.

**Consequence for the feature.** Phase 1 (windowing + per-mode WiFi-PS/telemetry)
changes *what the box does* — no nocturnal listening, less MQTT chatter — but **not
its instantaneous watts** on the current always-on hardware. The lever that would
actually move the floor is CPU-frequency-scaling / light-sleep (Phase 2), which is
compiled out pending the tickless↔task-WDT fix. So: **real Wh savings are still
gated on Phase 2**, not on windowing. Windowing remains worth keeping (it's free,
robust, and pays off the moment sleep lands or if a streaming relay makes mic-on
materially more expensive than mic-off — not measured here, no bench relay).

Caveat: short windows + sparse/jittery UC96 sampling; bench on USB, not streaming.
Re-run on the field unit (or a streaming bench) for a streaming-vs-idle delta.

### Measured C: LIGHT SLEEP moves the floor (universal FW, 2026-06-11)

Phase 1 (windowing) couldn't move the floor because the CPU stayed at 240 MHz and
WiFi stayed fully associated. Light-sleep (now compiled into every image)
adds **automatic light sleep** in the saving modes. Measured on the bench
(`ex01`) via the board-independent host exporter `uc96_power_watts{device=
"budka-example-2"}` on server-host :9877 (the board's own BLE meter is unusable here —
BLE must be OFF for light sleep). All rows BLE-off:

| Posture | Power @5V | Notes |
| --- | --- | --- |
| Continuous (full: camera+audio) + `pm_lightsleep=ON` | **~1.07 W (steady)** | audio holds the NO_LIGHT_SLEEP lock → **never sleeps** even with the knob on — full mode is safe by construction |
| Safe (audio off) + light-sleep OFF | ~0.84 W (steady) | |
| Safe (audio off) + **light-sleep ON** | **~0.40 W** | **30-min energy-integrated; −0.44 W vs no-sleep, ~−57% vs baseline** |

> Method note: the light-sleep load is bursty (sleep ~0.2 W / wake bursts ~0.6–1.3 W),
> so instantaneous UC96 samples are unreliable (a first 7-sample pass spread
> 0.25–1.3 W). The authoritative figure comes from **integrating the meter's energy
> accumulator** (`uc96_energy_wh`) over a long window: a 30-min undisturbed run gave
> ΔWh=0.20 → **0.40 W** (an 8-min run earlier read 0.45–0.49; an instantaneous
> "0.36 W" was a lucky-low sample). That 30-min run also held task_wdt flat (no
> reboot) + stayed reachable — the initial burn-in sample.
> Poll-lengthening in Safe (PIR/audio/status-LED/boot-btn/main-loop → ~500 ms–1 s,
> gated by `app_lowpower_active()`) removes the 20–100 ms CPU wakes, but the residual
> floor is WiFi-DTIM + camera-analog + SoC-base bound, so its measurable effect is
> small.

**This is the lever.** Auto light sleep in Safe cuts the board from ~0.84–1.0 W
to **~0.40 W** while staying WiFi-associated + reachable (verified: cmd/cfg
round-trips work, latency ~one DTIM), and still PIR→photo + heartbeat. Light sleep
also clock-gates the LEDC camera
XCLK during the sleep windows, so the camera's dynamic draw drops *without* needing
deinit (rank 4 / SCCB-standby would only trim the residual analog rail further —
optional). BLE-off itself was ~−0.10 W (NimBLE scanning) and is required anyway
(BLE blocks light sleep + ate ~90 KB RAM). Daily: ~0.40 W ≈ 9.6 Wh/day in Safe vs
~27 Wh/day flat — a real path to a winter-viable solar budget.
Remaining levers to push below 0.40 W (each trades some function): OV3660 SCCB
0x3008 standby/deinit (~camera analog) and a longer WiFi listen_interval (more
latency). Energy-integrate (don't spot-sample) to measure them.

Constraints proven on HW: needs the idle-task TWDT unsubscribe + BLE off (memory +
the BT light-sleep block). Field still gets this only after a
>24 h bench burn-in + signed OTA; the lowpower config never touches the shared overlay.

### What Safe + light-sleep can still do at ~0.40 W (measured 2026-06-11)

The pared-down Safe mode is far from dead — it's a reachable, motion-triggered
camera + weather station that gives up continuous audio. Verified on the bench
in Safe + `pm_lightsleep=ON` (light sleep wakes the SoC on incoming traffic / events):

| Function | Works? | Latency / cost |
| --- | --- | --- |
| MQTT control (cmd/cfg, cmd/ota, cmd/reboot) | ✅ | ~0.14 s round-trip — full remote control + recovery |
| HTTPS server (web UI, /capture, /stream, /last.jpg) | ✅ | ~0.5–0.7 s first byte; serving holds the CPU awake |
| Photo capture — PIR / reed / cmd / timelapse | ✅ | ~1.5 s; a NO_LIGHT_SLEEP bracket protects the DMA + a 4-frame post-wake drain keeps frames clean |
| Heartbeat telemetry (SOC/V/temp/RH/RSSI/heap/uptime) | ✅ | every `tlm_safe_s` (default ~15 min) |
| OTA | ✅ on demand | download holds the CPU awake |
| 24/7 audio / BirdNET | ❌ off | the expensive lever; VAD-triggered capture also off |

Energy envelope: the ~0.40 W idle floor dominates (~9.6 Wh/day). A photo is a
~1.2 s burst (~1.3 W) — even 30 photos/h adds <0.02 Wh/h, negligible.

One measured fragility: toggling `pm_lightsleep` OFF at runtime did not restore
PIR detection on one occasion (GPIO read stuck low until a reboot). If you
disable light-sleep over the air, follow it with a reboot.

## Sizing math (for when the load is known)

Li-ion 3.7 V nominal, 80 % usable DoD, ~88 % boost-to-5 V efficiency →
**~9 Wh usable per quality 18650 (3500 mAh)** at the 5 V rail.

At today's **27 Wh/day**:

| Cells | Usable | Autonomy (no sun) |
| --- | --- | --- |
| 3× 18650 | ~27 Wh | **~1 day** |
| 6× | ~54 Wh | ~2 days |
| 9× | ~81 Wh | ~3 days |

Solar in CZ (≈49–50°N, ~45° tilt), need ~39 Wh/day harvest after system losses:

- **Summer** (~4.5 peak-sun-hours): 10 W panel ≈ 45 Wh/day → covers it, surplus.
- **Winter** (Dec, overcast, ~0.5–1 PSH): 10 W ≈ 5–10 Wh/day → **~20 Wh/day
  deficit**. Year-round would need a **40–60 W panel + 6–9 cells**.

So the "napkin" rig (3 cells + 10 W) is summer-only autonomy with ~1 day buffer.
With Safe + light-sleep (~0.40 W ≈ 9.6 Wh/day measured), the same rig becomes
winter-plausible — that's the whole point of the light-sleep work above.
