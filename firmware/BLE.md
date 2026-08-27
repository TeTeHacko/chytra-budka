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
> internal-DRAM headroom).

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
