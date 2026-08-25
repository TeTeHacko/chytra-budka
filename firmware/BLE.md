# BLE meter reading

> **Status: UC96 path IMPLEMENTED + bench-verified (2026-06-06).**
> Optional BLE subsystem so the box can read the Atorch UC96 power meter without
> degrading WiFi. **Default OFF** (`ble_enabled` NVS knob). As of v0.2.0 it's
> compiled into BOTH images (field + bench) — `CONFIG_CHYTRA_BUDKA_BLE` and the
> memory diet live in the shared `sdkconfig.defaults.esp32s3`, so it's one
> canonical build. With `ble_enabled` OFF the BT controller is never initialised
> (`ble_start()` gates `nimble_port_init`), so a board is byte-for-byte
> unaffected until BLE is flipped on — which makes shipping it to the field
> low-risk. Flip it on only once a board is stable and watch `diag/selftest`
> `int_free`/`dma_free` (the field has more peripherals than the bench, so less
> internal-DRAM headroom). The original design plan is kept below for rationale.

## What was actually built (and where reality diverged from the plan)

**Implemented (Mode 1 — UC96 GATT, multi-meter):** active low-duty scan →
connect → MTU exchange → discover `0xffe0`/`0xffe1` → subscribe to the CCCD →
decode 36-byte report frames with `ble_parse_uc96()` → publish per meter over
MQTT (`<base>/meter/<mac>/{voltage,current,power,energy,temperature}`) + per-meter
HA discovery. Verified on the bench reading a live UC96 (V/I/P/Wh/temp into MQTT
+ HA), stable alongside WiFi/MQTT/i2s/camera, no `task_wdt`.

Divergences from the plan, all deliberate after hitting hardware:

- **ACTIVE scan, not passive.** The UC96's name (`UC96_BLE`) lives only in the
  SCAN RESPONSE; its `0xffe0` service UUID is in the ADV. We scan active and
  match on **either**. A passive scan never sees the name.
- **~50 % scan duty, not a ~2 % cadence.** At 3 % (window 10 ms / itvl 320 ms)
  with `PREFER_WIFI` coex eating BLE's slice, the radio caught **zero** adverts
  in 55 s. 30 ms / 60 ms reliably catches the meter. Scan runs only while a
  meter slot is free, then pauses — so it's bounded, not forever-hot.
- **Persistent connection + notify stream**, not periodic connect/read/
  disconnect. The UC96 sleeps its BT radio ~2 min if nobody connects; an active
  connection keeps it awake and streaming (~1 frame/s). Cleaner than re-scanning.
- **Multi-meter.** The field rig is panel→UC96→powerbank→UC96→Xiao, so up to
  `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` (3) meters connect at once, each keyed by
  its MAC. *2-meter concurrency is coded but UNTESTED — only one meter at the
  bench.*
- **MTU exchange is mandatory.** The 36-byte frame > the 23-byte default MTU
  (20 B payload) → `ble_att_set_preferred_mtu(128)` + `ble_gattc_exchange_mtu`
  per connection, else notifications truncate and the parse fails.
- **The RAM risk was real and is the headline lesson — see the memory diet
  below.** Enabling BLE OOM'd internal DRAM and `task_wdt` crash-looped the box
  into safe mode. Fixed by a diet in `sdkconfig.defaults.esp32s3` (fleet-wide).
- **Mode 2 (BTHome scan) NOT wired.** `ble_parse_bthome()` exists + is
  host-tested, but `ble.c` only does the UC96 path. BTHome is future work.
- **One knob, not five.** Only `ble_enabled` (BOOL, default false) shipped;
  `ble_uc96`/`ble_scan`/`ble_period_s`/`ble_scan_s` weren't needed for the
  single always-on UC96 path.

### Memory diet — the BT controller fits ONLY in internal DRAM

`esp_get_free_heap_size()` is **PSRAM-inclusive** on the S3 (~5 MB free) and
HID the real constraint: the BT controller + NimBLE + WiFi/i2s/camera DMA all
draw from the ~512 KB **internal** SRAM. Stock bench build left ~50 KB internal
free; turning BLE on starved i2s DMA (`i2s_alloc_dma_desc` failed) + lwip, the
audio task (TWDT-subscribed to catch a wedged i2s/relay) wedged on the failed
alloc, `task_wdt` fired, 3× → safe mode. Coredump named the `audio` task; root
cause was the OOM.

Fix (`sdkconfig.defaults.esp32s3`, fleet-wide, ~+20 KB internal freed):

- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` — small heap allocs go to PSRAM
  (default forces everything ≤16 KB internal). Task stacks + explicit
  `MALLOC_CAP_DMA/INTERNAL` allocs are unaffected.
- WiFi static RX/TX buffers (DMA → internal): `STATIC_RX_BUFFER_NUM=6`,
  `STATIC_TX_BUFFER_NUM=8`, `RX_BA_WIN=6` (must be ≤ 2× STATIC_RX or the build
  `#error`s).
- BT/NimBLE: `BT_CTRL_BLE_MAX_ACT=5`, `SCAN_DUPL_CACHE_SIZE=20`,
  `BT_NIMBLE_MAX_CONNECTIONS=3`, trimmed ACL/MSYS pools.

`diag/selftest` now ships `int_free` / `dma_free` / `dma_largest`
(`MALLOC_CAP_INTERNAL` / `MALLOC_CAP_DMA` + largest contiguous DMA block) so
the internal-DRAM headroom is visible — this is the number that matters, not
the PSRAM-inflated free-heap on the status page. Measured: BLE-off ≈ 72 KB
internal free; BLE-on + 1 connection ≈ 19 KB, stable.

### Web UI + allowlist + BTHome (v0.3.0)

A device-local management page at **`/ble`** (same top chrome as `/config`):

- **Scan** — every in-range device the radio surfaces (UC96 by name/`0xffe0`,
  BTHome by `0xFCD2`, plus named others), deduped by MAC, with RSSI + age. Held
  in a small RAM buffer (`s_disc[]`) updated in the DISC handler; `ble_snapshot()`
  merges it with the connected-meter table for the UI.
- **Name / save / forget** — names + the allowlist live in NVS via the new
  **`ble_store`** module (namespace `ble_dev`, key `d_<12-hex-mac>` → friendly
  name; mirrors `wifi_store`). Saving a device == allowlisting it.
- **Allowlist connect model** — the firmware GATT-connects a UC96, and ingests/
  publishes a BTHome sensor, **only if its MAC is saved**. Unsaved devices appear
  in the scan but are never acted on (a neighbour's UC96 is ignored). The gate is
  in the DISC handler: `kind==UC96 && ble_store_is_saved(id) → try_connect`.
- **State + last values** — `meter_t` keeps the last decoded `ble_uc96_reading_t`;
  BTHome readings live in the disc slot. The page shows connection state
  (streaming/connected/connecting/—) + the latest V/I/P/Wh/temp (UC96) or
  °C/%RH/battery/V (BTHome). `GET /ble.json` is the machine-readable live snapshot;
  `POST /ble/{name,forget,scan}` drive it.
- **BTHome publish** — a saved BTHome sensor is published (throttled ≤1/30 s) to
  `<base>/sensor/<mac>/…` with per-device HA discovery, mirroring the UC96 path.

Runtime-start caveat: a live `ble_enabled=ON` flip usually does NOT start BLE —
`nimble_port_init` needs a big contiguous internal-DRAM block, which a running
system's fragmented heap can't supply (works at boot, before other subsystems
allocate). The `/ble` page detects enabled-but-not-running and tells the operator
to reboot. Scanning is continuous while running (not gated by free meter slots),
so the UI keeps discovering candidates + observing BTHome.

---

## Original design plan (v0.2.0, kept for rationale)

## Goal

Opportunistically read BLE meters when one is in range, publish to MQTT + HA
like every other sensor, and be a silent low-power no-op when nothing's there.

Two reader modes (independent, either/both):

1. **UC96 power meter (GATT central)** — the Atorch UC96 USB-C inline meter
   (the same meter the author's host-side exporter reads). Lets the *box*
   self-report its own draw over BLE during bench power-profiling (feeds the
   capacity/power doc). USB-C inline ⇒ a **bench** tool; in the field there's
   usually no UC96, which is the headline "meter not always present" case.
2. **Passive BTHome / ATC scan (observer)** — cheap BLE thermo-hygrometers
   (BTHome v2, ATC-pvvx, Xiaomi LYWSD03MMC custom-fw, Govee, Ruuvi) broadcast
   readings in their advertisement. Scan + parse, no connection. The field-
   useful mode (climate around the box without wiring I²C/1-wire).

## Hard constraints (these shape every decision below)

- **WiFi has absolute priority over BT.** The box's job is MQTT / audio stream /
  OTA. BLE must never starve them. → SW-coexistence with `ESP_COEX_PREFER_WIFI`,
  short bounded BLE windows, and BLE *defers entirely while audio is actively
  streaming*.
- **A meter is not always present.** Absence is the *normal* case (field, no
  UC96), not an error: no log-spam, no crash, no wasted power, no failed-state
  in HA. Detect-and-degrade — the same posture as the I²C sensors
  (`ina226`/`reed` are optional `*_ready()=false`, not faults).
- **Default OFF**, behind NVS knobs (schema → NVS + HA discovery + `/config` +
  `cmd/cfg`, the standard path). A board that never enables BLE is byte-for-byte
  unaffected at runtime.
- **Robustness over breadth** (a project ground rule). BLE is opt-in, bounded, and can't
  wedge the box: every BLE op is time-boxed and runs off the control path.

## Architecture

- **NimBLE host** (not Bluedroid): far smaller flash/RAM, BLE-only — correct for
  the S3 (which has no BT Classic anyway). Enable in sdkconfig:
  `CONFIG_BT_ENABLED=y`, `CONFIG_BT_NIMBLE_ENABLED=y`, controller default.
- **Compile-time gate `CONFIG_CHYTRA_BUDKA_BLE`** (Kconfig, default `n`): when
  off, none of the NimBLE stack or `ble.c` is built — zero flash/RAM cost for
  builds that don't want it. Turn it on in the `bench` profile (UC96 profiling)
  and optionally `field`. `ble.c` compiles to no-op stubs when the symbol is
  off, mirroring `FlacEncoder` — so `main.cpp`/`app_config.c` stay codec/feature-
  agnostic.
- **Coexistence:** `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` +
  `esp_coex_preference_set(ESP_COEX_PREFER_WIFI)` at init (verified present in
  IDF v6.0.1 `esp_coexist.h`). WiFi wins RF contention.
- **One `ble` FreeRTOS task** (low prio, CPU0), event-driven via NimBLE, that
  wakes on a cadence, does ONE short bounded action, then sleeps. Never holds a
  persistent high-duty connection. NOT TWDT-subscribed — but every action is
  time-boxed by a NimBLE timeout so it can't hang.
- **New module `main/ble.c` + `ble.h`** (ESP-IDF/NimBLE-specific ⇒ in `main/`,
  not the framework-agnostic `cb_core`). Pure-logic bits (UC96 frame parse,
  BTHome TLV parse) factored into a host-testable file (`tests/native`), no
  NimBLE deps — same split as `sd_layout.c`.

## Mode 1 — UC96 power meter (GATT)

Periodic **connect → read a few frames → disconnect** (NOT a persistent
connection — bounded coexistence + clean absence handling).

- **Find:** passive scan ≤ `ble_scan_s`; match adv name `~ /UC96[_-]?BLE/i`
  (from `uc96d.py:73`). Not seen → meter absent → back off, done (no error).
- **Read:** connect, discover service `0000ffe0-0000-1000-8000-00805f9b34fb`
  (the HM-10-style BLE-serial service; char UUID confirmed at impl time from
  `uc96d.py`), subscribe to notifications, collect N frames, disconnect.
- **Parse (port of `uc96d.py:parse_frame`, 36-byte frame, big-endian):**
  `voltage_v = u24(buf[4:7])/100`, `current_a = u24(buf[7:10])/100`,
  `energy_wh = u32(buf[13:17])/100`, `power_w = V*I`, runtime h/m/s. Validate
  length + a header/checksum byte; reject otherwise (return NULL, no publish).
- **Publish:** `state/uc96_*` (V/I/P/Wh) + HA discovery (sensor entities), gated
  on a fresh read. Stale after `2 × ble_period_s` → publish unavailable.
- **Absent / connect-fail:** exponential backoff (0.5→1→2→…→cap `ble_period_s`,
  the `audio.cpp` relay-backoff pattern) so a no-meter field board scans rarely.

## Mode 2 — passive BTHome / ATC scan (observer)

- Periodic **passive** scan (no connect) ≤ `ble_scan_s` every `ble_period_s`.
- Parse **BTHome v2** service-data (UUID `0xFCD2`) TLV (temp 0x02 /100, humidity
  0x03 /100, battery 0x01, …) and **ATC-pvvx** custom format. Each advertiser
  → an HA device keyed by MAC; publish `state/ble/<mac>/*`.
- Optional **allowlist** (`ble_macs` CSV NVS) so the box only ingests known
  sensors, not every BTHome device in WiFi range. Absent → nothing published.

## WiFi-priority coexistence (the #1 constraint)

1. `esp_coex_preference_set(ESP_COEX_PREFER_WIFI)` — controller-level: WiFi wins.
2. **Short windows only:** scan window ≤ a few s; UC96 connect bounded
   (connect-timeout + max frames + hard disconnect). No continuous scan.
3. **Defer to active audio:** before any BLE action, check the audio pump state
   (`app_mode_current()` / an `audio_is_streaming()` accessor). If a burst is
   live, **skip this BLE cycle** — audio's WiFi stream is sacrosanct. BLE catches
   the next cadence.
4. **Cadence, not duty:** `ble_period_s` default 300 s, `ble_scan_s` default 6 s
   → BLE radio time is ~2 % even when enabled.

## Meter-absent handling (the #2 constraint)

- Init with no controller/stack failure if the board simply has nothing nearby.
- `ble` selftest row is **optional** (`required=false`, like `ina226`): reports
  `scanning` / `uc96:seen Ns ago` / `absent` — visibility without flagging
  absence as degraded. No GlitchTip noise for "no meter".
- One INFO log on first "nothing found", then silent until state changes.
- Backoff means a no-meter board scans ~once per `ble_period_s` and otherwise
  the BLE task sleeps → negligible power.

## Config knobs (schema rows in `app_config.c`; NVS ≤15-char keys)

| key | type | default | meaning |
| --- | --- | --- | --- |
| `ble_enabled` | BOOL | **false** | master enable for the whole subsystem |
| `ble_uc96`    | BOOL | false | read the UC96 power meter if present (Mode 1) |
| `ble_scan`    | BOOL | false | passive BTHome/ATC scan (Mode 2) |
| `ble_period_s`| INT  | 300 | seconds between BLE cycles (cadence) |
| `ble_scan_s`  | INT  | 6   | scan window length per cycle |

`apply_side_effects` dispatches `ble_*` → `ble_apply_config()` (start/stop the
task, re-read cadence) — live, no reboot, the `reed_apply_config` pattern.
i18n labels optional (fall back to the English `name`, like the new cam knobs).

## Build / flash / RAM

- NimBLE ≈ +50–80 KB flash, +~30–40 KB RAM (controller + host). App is 55 % free
  on flash; RAM is the thing to watch — budget the NimBLE host pools, allow
  PSRAM for buffers. Verify at build, and that audio + camera still fit.
- `CONFIG_CHYTRA_BUDKA_BLE=n` → none of it built. So `production`/minimal
  builds stay lean; `bench` (and field-with-sensors) opt in.

## Files

| file | change |
| --- | --- |
| `main/ble.c` / `ble.h` | **NEW** — NimBLE task, UC96 GATT reader, BTHome scanner, publish |
| `main/ble_parse.c` / `.h` | **NEW** — pure UC96-frame + BTHome-TLV parsers (host-testable) |
| `main/app_config.c` | 5 schema rows + `ble_apply_config()` dispatch + extern |
| `main/main.cpp` | `ble_start()` after WiFi up (skipped in safe mode); selftest row |
| `main/audio.cpp` | tiny `audio_is_streaming()` accessor for the defer-to-audio gate |
| `main/Kconfig.projbuild` | `CONFIG_CHYTRA_BUDKA_BLE` (default n) |
| `sdkconfig.defaults.bench` | enable BLE + NimBLE + SW coex for the bench profile |
| `tests/native/test_ble_parse.c` | UC96 frame + BTHome TLV unit tests (no NimBLE) |
| `tests/hil/test_ble.py` | optional: assert graceful no-meter behaviour (selftest `ble:absent`, WiFi/audio unaffected) |

## Phasing

1. **Parsers + native tests** (`ble_parse.c`, no radio) — UC96 frame + BTHome TLV.
2. **NimBLE skeleton**: Kconfig gate, stack init, coex=WiFi, the `ble` task,
   passive scan, selftest `ble` row — *absence-correct first* (the field case).
3. **Mode 2 (BTHome publish)** — the field-useful, lowest-risk (scan-only) path.
4. **Mode 1 (UC96 GATT)** — connect/read/disconnect + backoff; bench profiling.
5. **HIL**: no-meter graceful + WiFi/audio-unaffected; bench: a real UC96 read.

## Risks / open questions

- **Coexistence vs audio throughput** — must measure: BLE cycle during a
  continuous-mode audio burst. The defer-to-audio gate should make it a non-
  issue, but verify the relay stream FPS/latency is unchanged with BLE on.
- **RAM headroom** with NimBLE + camera framebuffers + audio + TLS. May need to
  push NimBLE pools / scan buffers to PSRAM.
- **UC96 char UUID + checksum** — confirm the notify characteristic + the frame
  validation byte from `uc96d.py` / a live capture at impl time.
- **Security** — passive scan is read-only; the UC96 GATT connect is to a known
  meter (name-matched, optional MAC allowlist). No inbound BLE control surface.
