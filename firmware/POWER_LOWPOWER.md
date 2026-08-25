# Light-sleep — universal FW (every image)

Goal: cut the flat ~0.9 W floor in the SAVING modes (Triggered-window-closed /
Safe) while keeping FULL mode (Continuous, USB) byte-for-byte unchanged. Grounded
in a 20-agent research sweep of ESP-IDF docs + forums (2026-06-11). The mechanism
is **automatic light sleep** (the only lever that moves the SoC+WiFi block);
camera-sensor standby is the other big lever (the camera is ~45-55% of the load).

**STATUS (2026-06-11): UNIVERSAL FW, light-sleep built into every image but
`pm_lightsleep` DEFAULT OFF — opt-in per unit after a per-unit load burn-in.**
PM is in the shared `.esp32s3` overlay (no separate profile) — one FW: full power
on mains (Continuous), ~0.40 W deep-save on solar (Safe) *when light-sleep is
enabled*. Measured **~0.40 W in Safe (~-57% vs ~0.93 W)**, 30-min energy-integrated,
reachable (MQTT ~0.14 s / HTTPS ~0.6 s), capture visually clean (camera fix),
task_wdt flat over 15+ min *idle*, boots with `ble_enabled=ON` (ex02 config,
~17 KB free, OTA-safe).

**Why DEFAULT OFF (not on-everywhere):** on-by-default the fleet HIL gate
crash-looped. `CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` allocates CPU-retention RAM
on each sleep entry; under *active* load (TLS handshakes, captures, mode-churn) it
competes with the working heap → `sleep_cpu_configure: Failed to enable CPU power
down` → OOM → `task_wdt` → safe-mode (GlitchTip QQ/PR/JQ, ~14:08). Idle burn-in
(15+12 min flat) never hit it — only the load did. Fixes: **CPU power-down disabled
in light sleep** (clock-gate only; stable, a few mW dearer) **and the knob defaults
OFF** so a unit light-sleeps only after it's been load-validated. Robustness over
fleet-wide convenience. Enable per unit (`cmd/cfg/pm_lightsleep ON`) once that
unit clears a load+capture burn-in.

## RELEASE RUNBOOK

The image is universal — `field`/`bench` both carry light-sleep. New low-power
units run `ble_enabled=OFF` (the default) → light-sleep works + RAM comfortable.

1. **Build + flash** (USB): `firmware/tools/build.sh field` (or `bench` for the
   debug image) → `firmware/tools/flash_safe.sh -p /dev/esp32-<mac> flash`. NOTE:
   a board that is currently light-sleeping can't be flashed (USB-Serial-JTAG
   disrupted → esptool `OSError 71 / Protocol error`) — first **wake it** via
   `cmd/cfg/pm_lightsleep OFF` (or `mode_override triggered`), then flash. A truly
   wedged port needs a USB re-plug or `sudo usbreset` of the 303a:1001 device.
2. **Enable saving (opt-in):** `pm_lightsleep` defaults **OFF** — turn it on for
   this unit with `cmd/cfg/pm_lightsleep ON` AFTER it clears a load+capture burn-in
   (see step 4). Then pin the mode: `cmd/cfg/mode_override safe` (or `auto` + low
   SOC later). A mains unit pinned `continuous` stays full-power and needs no flip.
   Set `ble_enabled=OFF` on any unit that still has it ON (e.g. after OTA'ing
   ex02) for the RAM margin + to let Safe actually sleep.
3. **Verify**: reachable (MQTT cmd ~0.14 s, HTTPS ~0.6 s), `cmd/photo` captures
   clean, power drops — measure via the board-INDEPENDENT host exporter on
   server-host **:9877**, metric `meter_power_watts{meter_name="<dev>",source="host"}`
   (NOT `cb_meter_*` — that needs the board's BLE, which is off). Energy-integrate
   (`meter_energy_wh` ΔWh over minutes), don't spot-sample.
4. **Load+capture burn-in (the gate for flipping `pm_lightsleep ON`)**: with
   light-sleep on, drive *active* load — repeated HTTPS hits + `cmd/photo` captures
   + mode-churn, not just idle — for >24 h and confirm GlitchTip CHYTRA-BUDKA-JQ
   (task_wdt), QQ (sleep_cpu_configure), and PR (safe-mode) all stay flat and
   captures stay clean. Idle-only burn-in is NOT sufficient — the on-by-default
   crash-loop only showed under load. The fleet HIL gate (ota_upload) exercises
   exactly this; it must come back green.
5. **Release** (person-gated): `git tag v0.4.8` → `firmware/tools/ota_upload.sh`
   (builds `field`, HIL-gates, prompts a **YubiKey touch+PIN to sign** — agents
   can't, SHA256+version manifest, staged canary). The OTA'd `field` image is the
   universal FW; on the mains unit it stays in Continuous (no sleep). Setting a
   unit to Safe (manually or future SOC-auto) is what turns saving on.

## Universal FW — one image for all units

PM is in the **shared `.esp32s3` overlay**, so light-sleep is compiled into every
image (`field`, `bench`) — there is no separate profile. Safety is by construction,
not by build isolation:

- **Mode gate:** light-sleep engages only in Safe (audio off); Continuous/Triggered
  hold a NO_LIGHT_SLEEP lock while audio runs → a mains unit never sleeps.
- **`pm_lightsleep` default OFF (opt-in per unit), `ble_enabled` default OFF.**
  BLE off is what lets Safe actually sleep (the BT controller blocks light sleep)
  and keeps RAM comfortable (~66 KB free); with BLE on the FW still boots
  (verified, ~17 KB free) but is tight — don't run BLE+audio under heavy TLS.
  Light-sleep also runs with **CPU power-down disabled**
  (`# CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP`) — the retention alloc OOM'd under
  load. See the STATUS note above.
- **Stock HZ=100** (no global tick change); saving-mode poll-lengthening clears the
  sleep threshold.
- The existing mains field unit is OTA-safe: in Continuous it never sleeps
  (behaviour unchanged); still recommended to set `ble_enabled=OFF` post-OTA for
  the RAM margin (it loses on-board self-metering → host UC96 exporter covers it).

`cb_pm.c` + the NO_LIGHT_SLEEP locks (audio/camera DMA brackets) are the runtime
machinery; in Continuous the audio lock is held continuously so auto-sleep can
never engage there even if the knob were mis-set.

## Current breakdown (research, anchored to S3 datasheet + forum)

LDO is **linear** (ME6217C33M5G) → Iin≈Iout, so 3.3 V-rail savings DO show on the
5 V UC96 meter. Of the ~180 mA @5V:
- **Camera (OV3660) ~85-100 mA @3.3V — DOMINANT (~45-55%).** PWDN/RESET=-1 on the
  XIAO Sense (verified config.h:105-106) so no GPIO power-down. `esp_camera_deinit`
  alone frees only the XCLK/DMA digital part (~≤15 mA); the **sensor analog draw
  needs SCCB software-standby reg 0x3008 bit7** (forum OV3660: 37.8→~1.45 mA).
- **SoC + WiFi @240 MHz ~70-90 mA** — the block light-sleep moves.
- PSRAM/I2S/sensors/LEDs ~5-15 mA — noise (matches the mic-on/off A/B null result).
- Board residual (LDO Iq + USB-Serial-JTAG) — a few mA, not firmware-removable.

## Findings from the bench (2026-06-11)

0. **Camera corruption under light-sleep + fix (caught by inspecting /last.jpg).**
   Light sleep gates the LEDC-generated camera XCLK while idle, so the OV3660's
   first frame(s) after wake are torn — garbage magenta/green top rows, sometimes
   a fully scrambled frame. Isolated cleanly: capture with `pm_lightsleep` OFF =
   clean, ON = corrupt. (min==max==240 avoids the *DFS* XCLK bug, but light sleep
   stops clocks entirely — a different mechanism.) FIX: a **4-frame post-wake
   drain** in `camera_capture_event` gated on `app_lowpower_active()` (camera.c) —
   flush the torn frames so the sensor/DMA re-sync before the real grab. Verified
   4/4 clean captures in Safe+light-sleep after the fix. `cmd/photo` reporting a
   new `event/photo` does NOT mean the pixels are good — always eyeball /last.jpg.

1. **task_wdt root cause + fix (the original blocker).** Enabling PM+tickless
   swaps in idle hooks that take PM locks under `portENTER_CRITICAL`, perturbing
   the IDLE-task TWDT feed; with `CHECK_IDLE_TASK_CPU0/CPU1=y`+`PANIC=y` it
   reboots. FIX (in the shared .esp32s3 overlay): `# CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1
   is not set` — hot-path tasks stay watched (they all `esp_task_wdt_add(NULL)`).
   This stopped the IDLE-task wdt.

2. **Memory exhaustion — BLE is the hog.** First lowpower boot (BLE on) had
   `int_free=14 KB, dma_largest=3584 B` → I²S DMA-descriptor alloc FAILED
   (`i2s_alloc_dma_desc`) → audio dead; mbedtls AES alloc failed → TLS dead;
   crash-looped to safe-mode. Disabling BLE (`ble_enabled=OFF`) jumped free RAM to
   **`int_free=101 KB, dma_largest=36 KB`** — NimBLE/BT-controller was ~90 KB.
   Audio I²S init then succeeds. Also dropped `PM_SLP_IRAM_OPT`/`PM_RTOS_IDLE_OPT`
   from the overlay (they steal IRAM from the DRAM pool; only latency opts).

3. **BLE blocks light sleep outright.** `BLE_INIT: light sleep mode will not be
   able to apply when bluetooth is enabled.` So light sleep is impossible while
   the BT controller is up — the saving modes MUST run with BLE off.

4. **The board is NOT the meter source.** Its BLE only *detects* UC96
   (`uc96=0`, "V/I/P read is phase 4"). `cb_meter_power_watts{device=ex01}` is
   **host-scanned** (server-host). So disabling the board's BLE is free — it frees
   90 KB, unblocks light sleep, and does NOT lose the power measurement.

5. Runtime BLE toggle is reboot-only (`ble_apply_config` just logs "reboot to
   start/stop"); `ble_enabled` is read at boot in `ble_start`. So a low-power unit
   keeps `ble_enabled=OFF` (the default) — BLE never inits, light sleep works.

## Ranked levers (research synthesis)

1. **Stabilize tickless+auto-light-sleep** (shared .esp32s3 overlay) — DONE: wdt fix + BLE
   off + IRAM opts off. 0 mA itself; gates the rest.
2. **Auto light sleep + WiFi-associated in Safe** (`cb_pm_set_lightsleep(true)`,
   already wired; min==max==240 no-DFS = camera-safe). Moves SoC term ~70-90 → few
   mA. Needs the 20 ms PIR/reed polls fixed (#3) to reach residency.
3. **PIR/reed 20 ms polls → blocking + light-sleep GPIO wake** (LEVEL-triggered;
   PIR=GPIO2, REED=GPIO1). Residency multiplier for #2.
4. **OV3660 0x3008 software-standby + `esp_camera_deinit` in saving modes** — the
   DOMINANT sensor lever (~85→~4 mA if it holds on OV3660). MEASURE-FIRST; runtime
   deinit/re-init is historically fragile — full re-init on return to Continuous,
   `sccb_bus_scan_diag`+reboot on failure. Never deinit in Continuous.
5. DFS (min=40) only AFTER camera deinit (additive/fallback; mostly subsumed by
   light sleep). Strict ordering: raise to 240 before `esp_camera_init`.
6. WiFi teardown + periodic reconnect (Safe-survival, abandons reachability) —
   optional, bench-only, deepest.

## Status / RESULT (2026-06-11)

**Rank 1+2 DONE and MEASURED — light sleep works and is the floor-mover.**

- Universal FW is stable: normal boot, `consecutive_crashes=0`, selftest ok (10/14;
  the 4 fails are battery/ina226/reed/servo, none wired on the bench), camera+audio
  init fine (`int_free=64 KB` with BLE off), no task_wdt, reachable.
- Measured (host exporter on server-host :9877, board-independent, BLE off):
  - Continuous + `pm_lightsleep=ON` → **~1.07 W steady** (audio lock → never sleeps;
    **full mode safe**).
  - Safe + light-sleep OFF → ~0.84 W (steady).
  - Safe + **light-sleep ON** → **~0.40 W** — ENERGY-INTEGRATED over 30 min
    (ΔWh=0.20; an 8-min run read 0.45–0.49, instantaneous "0.36 W" was a lucky-low
    sample). The load is bursty (sleep ~0.2 W / wake bursts ~0.6–1.3 W) so use the
    energy accumulator, not spot samples.
  - ⇒ light sleep **−0.44 W vs no-sleep, ~−57% vs baseline**. Stays reachable +
    PIR→photo (visually clean post-fix) + heartbeat (full functionality).
- Rank 3 (poll-lengthening) DONE: PIR/audio/status-LED/boot-btn/main-loop polls
  stretch to ~500 ms–1 s in Safe via `app_lowpower_active()` (Continuous untouched).
  Removes the 20–100 ms CPU wakes; measurable effect small (floor is WiFi-DTIM +
  camera-analog + SoC-base bound).
- Measurement gotcha (important): the board reads its OWN UC96 over BLE
  (`mqtt.c::mqtt_publish_uc96` → cbprom → `cb_meter_power_watts`), so with BLE off
  there's no board-sourced meter. Use the **separate host service** instead —
  `/opt/uc96-exporter`, `uc96_power_watts{device="budka-example-2"}` on server-host
  **:9877**, which scans the meter independently of the board.

## Deep-save Safe — what it can still do at ~0.40 W (measured 2026-06-11)

The pared-down Safe mode is far from dead — it's a reachable, motion-triggered
camera + weather station that just gives up continuous audio. Verified on the bench
in Safe + `pm_lightsleep=ON` (light sleep wakes the SoC on incoming traffic / events):

| Function | Works? | Latency / cost | Notes |
| --- | --- | --- | --- |
| MQTT control (cmd/cfg, cmd/ota, cmd/reboot) | ✅ | ~0.14 s round-trip | full remote control + recovery |
| HTTPS server (web UI, /capture, /stream, /photo, /last.jpg) | ✅ | ~0.5–0.7 s first byte | on-demand; serving holds CPU awake |
| Photo capture — PIR / reed / cmd / timelapse | ✅ | ~1.5 s | saved to SD + `event/photo` + HTTP URL; capture path gated only on cam_enabled (no mode gate, main.cpp:1395/1442/1456/1489); NO_LIGHT_SLEEP bracket protects the DMA + a 4-frame post-wake drain (see camera fix below) gives clean frames |
| Heartbeat telemetry | ✅ | every `tlm_safe_s` (~15 min) | SOC/V/temp/RH/RSSI/heap/uptime |
| Env sensors (SHT41 int+ext, INA226 solar) | ✅ | on telemetry tick | |
| OTA | ✅ (on demand) | download holds CPU awake | reachable; release-feed gated as usual |
| 24/7 audio / BirdNET | ❌ off | — | the expensive lever; VAD-triggered capture also off (needs audio) |
| BLE self-metering | ❌ off | — | host exporter reads the meter instead |

Energy envelope: the ~0.40 W idle floor dominates (~9.6 Wh/day). A photo is a ~1.2 s
burst (~1.3 W) → even 30 photos/h adds <0.02 Wh/h, negligible. So Safe ≈ "reachable
motion-cam + weather station at half power," bounded by the WiFi-DTIM + camera-analog
+ SoC-base floor, not by occasional activity.

## Next (optional, not yet done)

- **Productization — universal image DONE; on-by-default REVERTED.** Folded into
  one universal FW (PM in the shared `.esp32s3` overlay, no separate profile;
  `ble_enabled` default OFF). `pm_lightsleep` was briefly default ON but the fleet
  HIL gate crash-looped under load (CPU-power-down retention OOM) → reverted to
  **default OFF + CPU power-down disabled**. Remaining = the per-unit load+capture
  burn-in (step 4) before flipping any unit's `pm_lightsleep ON`, then tag/sign.
- **Rank 4 — OV3660 SCCB 0x3008 standby + deinit in Safe (optional, below ~0.40 W).**
  Light sleep already clock-gates the camera XCLK during sleep, so this only trims
  the residual sensor *analog* rail in the awake windows — small marginal gain.
  Risky (runtime deinit/re-init fragility). Measure-first.
- **Rank 3 — PIR/reed polls → light-sleep GPIO LEVEL wake (optional).** The current
  poll-lengthening (~500 ms) already gets most of it; true GPIO-wake would deepen
  residency further. Likely pushes Safe a bit below ~0.40 W.
- **SOC auto-select (deferred):** once the MAX17048 fuel gauge is on the solar
  board, `mode_override=auto` can drop to Safe on low SOC — light-sleep then kicks
  in automatically. The per-mode behaviour is already wired for it.

## Fáze 2 — re-confirm + standby spike + listen_interval (2026-06-19, bench ex01)

New build (`v0.6.1`), bench meter = UC96 `budka-example-2` (`meter_energy_wh`, energy-integrated).

- **Light-sleep re-confirmed on the new build.** Baseline `auto`→Triggered (audio ON,
  LS OFF) = **0.96 W** (20-min avg). Forced Safe + `pm_lightsleep=ON` → **~0.25 W**
  (15-min energy-integrated; power *gauge* is too sparse/0-jittery to average — used
  the Wh counter). That's **−74 % vs the Triggered baseline** (combines audio-off +
  Safe WiFi MAX_MODEM + light-sleep) and lands a bit below the documented 0.40 W —
  the bench has no fuel-gauge/charger rail. NOTE: this is the *combined* posture; the
  isolated light-sleep delta (Safe LS-OFF→LS-ON, −57 %) is the 2026-06-11 number,
  not re-measured here. No regression from the new `wifi_listen_iv` default (=3).

- **`wifi_listen_iv` knob added** (app_config `SCHEMA[]` + `wifi_mgr` →
  `wcfg.sta.listen_interval`). Default 3 (= IDF default = prior behaviour), clamp
  1..10, applied at connect (effect on next reconnect/reboot), only honoured under
  `WIFI_PS_MAX_MODEM` (Safe). The lone un-built lever to push below the Safe floor at
  the cost of downlink latency — operator bumps per-unit. Not yet measured for a
  per-step W delta (needs a Safe-mode run per value).

- **Rank 4 — OV3660 software standby: spike built + recovery proven; delta NOT yet
  confirmed.** Added a BENCH-ONLY `GET /debug/cam_standby?on=1|0` →
  `camera_debug_sensor_standby()` writing `SYSTEM_CTROL0` **0x3008 bit6 (0x40)** under
  the capture mutex + a NO_LIGHT_SLEEP lock, with a 4-frame post-wake drain. (CORRECTION:
  the bit is **6 = software power-down**, not 7 — bit7 is software *reset* per
  `ov3660_regs.h`; writing bit7 would reset, not standby.) Recovery is clean: after both
  a brief and an 8-minute standby, a wake + capture returns a valid 1600×1200 UXGA JPEG,
  no SCCB wedge. **Preliminary, INCONCLUSIVE delta:** 8-min windows in Safe+LS gave
  standby-OFF ~0.30 W vs standby-ON ~0.15 W — i.e. a *hint* of ~0.1 W (≈removing the
  camera's residual analog rail that XCLK-gating leaves on), which at the ~0.25 W floor
  would be a meaningful ~40 % cut. BUT that's only a 2-tick difference at the counter's
  0.01 Wh resolution over 8 min — within noise. A longer (≥15 min/window) confirmation
  was started but aborted early (bench needed for other work). Endpoint is debug-gated
  (compiled out of field/production); spike is NOT wired into the mode FSM. Decision on
  productizing standby still pending a clean longer measurement.

- **FRAGILITY observed: runtime `pm_lightsleep` disable does NOT restore PIR detection —
  reboot needed.** After several enable/disable cycles of `pm_lightsleep` + camera-standby
  toggling, the PIR GPIO read (`/debug/pir`) sat stuck LOW during real motion (no
  detections, `motion_count` frozen) even though the board was back in Triggered with
  `pm_lightsleep=OFF` and `app_lowpower_active()` false (so the poll was at full rate).
  A clean `cmd/reboot` immediately restored detection (first wave → `level=1`,
  `motion_count` 0→1). Root mechanism not pinned down (GPIO state left by automatic
  light-sleep, or the poll task's sampling) — single observation, but it lines up with
  why `pm_lightsleep` is default-OFF + burn-in-gated. IMPLICATION: if a fielded unit ever
  has light-sleep toggled OFF over the air, **reboot it** to be sure PIR/inputs are sane;
  don't trust a live OFF to fully undo the sleep posture. Worth a deliberate repro before
  treating as definitive.

- **Wake-on-capture added (PIR-safe standby).** So the standby spike doesn't kill the core
  PIR→photo function, `camera_capture_event()` now wakes the sensor (clear 0x3008 bit6) +
  drains frames on entry when `s_sensor_standby` is set, and re-enters standby on exit —
  all under the capture mutex, all debug-gated (production path byte-identical). Verified:
  with standby ON, two PIR-path captures (`/debug/capture`) both produced clean 1600×1200
  UXGA frames stamped with the right trigger. This is the shape a productized "standby in
  Safe" would take (idle→standby, capture→wake→shoot→re-standby).
