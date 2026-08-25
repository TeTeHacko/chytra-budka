# OLED status display — design proposal

Status: **bench DONE — OLED + generic sensor registry + WiFi onboarding**.
Worktree `feat/oled-status`. Bench-only OLED; field is OTA-only (no OLED on
`ex02`).

## WiFi onboarding + captive portal (DONE — full-clear HW-verified 2026-06-12)

When the box boots as an access point **and** a display is present, the OLED
shows a WiFi-join QR; scanning it joins the AP and the captive portal opens
the `/wifi` form. Verified end-to-end on a genuinely unprovisioned board
(NVS erased + `have_sta_target()` floor forced false to simulate placeholder
compiled creds; NVS backed up + restored after).

- **Random AP password (unprovisioned only).** On the `unprov` first boot,
  `wifi_mgr_use_random_ap_pass()` replaces the public `AP_PASS_DEFAULT` with a
  per-boot 12-char random one (lowercase+digits, no ambiguous chars, never
  logged by value), so a fresh board isn't reachable on the well-known
  credential. Sticky `ap_only` mode keeps its configured/default creds (stable,
  no per-boot churn). Operator-custom AP creds always win.
- **Boot-race solved at both ends.** The password decision needs the display
  known *before* the AP is configured → `oled_probe_present()` is a synchronous,
  side-effect-free probe called in `app_main` before `wifi_mgr_init`. The QR
  appears *after* the panel's 4 s settle → `oled_show_wifi_qr()` stores a
  PERSISTENT onboard-QR slot the OLED task paints whenever it's up.
- **Captive auto-pop** (`dns_hijack.[ch]` — new). The old code only had a
  *comment* claiming a DNS responder; there wasn't one, so the portal never
  auto-opened (operator had to type `172.31.4.1`). Fixed with the standard
  trio: a UDP:53 responder answering every A query with `AP_IP`; DHCP option 6
  advertising the box as the DNS server (`ap_configure_netif`); and a captive
  404 handler on the main `:80` server serving the `/wifi` form (200 content,
  which trips both Android "sign in" + the iOS captive sheet).
- **Scan picker in the captive form.** `/wifi` now has a "📶 Scan networks"
  link → `/wifiscan?to=wifi` (lightweight captive flow, not the heavy
  `/config`); tap a row → back to `/wifi` with the SSID pre-filled. Scanning in
  AP-only works because `wifi_mgr_scan` momentarily flips AP→APSTA and back.
- **Anti-brick confirmed incidentally:** a wrong-password submit stages a
  candidate, fails to reach MQTT, and auto-reverts to known-good after
  `WIFI_CAND_VERIFY_TIMEOUT_S` (240 s) — observed recovering on its own.

## Generic sensor registry (DONE)

`sensors.[ch]` — one table, `CB_SENSORS[]` → channels, drives ALL outputs:
MQTT telemetry, HA discovery, the HTML `/` UI, and the OLED. Previously each
metric was special-cased in 4 places; now **adding a sensor = write a driver
+ add one registry row** and it appears everywhere automatically.

- A sensor instance = `{present, refresh, channels}` (bus-agnostic — the
  channel read fns hide the bus), so any sensor on any I²C bus fits.
- Channel `obj` ids are STABLE (= state-topic suffix = HA object_id): the
  historic `temp`/`humidity`/`temp_ext`/`humidity_ext` keep their HA
  identity; BMP388 adds `temp_bmp` + `pressure`. Never rename an `obj`.
- The single telemetry owner calls `cb_sensors_refresh()` (one I²C read per
  present sensor); HTML/OLED read caches only → no concurrent bus callers.
- Registered today: SHT41 inside (`sht0`/bus0), SHT41 outside (`sht1`/bus1),
  BMP388 (`bmp`/bus0 @0x77). BMP388 is in selftest + `/i2c` expected list.
- Scope = physical I²C sensors. System metrics (RSSI/heap/MCU/SOC/solar)
  keep their existing dedicated paths.

### Still pending (smaller now)
- **Field build:** the field board has no BMP388 yet (soft-detect no-ops,
  so it's safe) — fit one before relying on its `pressure`/`temp_bmp`.
- Optional: Grafana panels for pressure; decide if a runtime (NVS) sensor
  map is ever worth it over the compile-time table (probably not).

---

## Bench bring-up (DONE)

## Bring-up state (2026-06-12)

Minimal "na hraní" bring-up is written, builds + flashes clean on the bench:
- `oled.c` / `oled.h` — self-contained SSD1306 driver (no external
  component), on the SHARED bus0 via `i2c_bus_get()`, 100 kHz.
- `font5x7.h` — vendored Adafruit GFX 5×7 glyph table (public domain).
- Boot splash (id + fw) + auto-refreshing **Page 1** (id/RSSI, SSID, IP,
  MQTT + uptime, link banner). Wired into `main.cpp` after http start.
- Detection deferred into a task (4 s) + 3-consecutive-ACK consensus, so
  early-boot bus churn and false-ACKs can't trigger a doomed init. Absent
  ⇒ clean no-op; SHT41 (the required sensor) unaffected.

### Sensors on the status screen
- **in** = BMP388 (bus0 @ 0x77): temperature + pressure (hPa).
- **out** = SHT41 (bus0 @ 0x44): temperature + RH.
- bus1 (the second I²C) is no longer shown — freed, per plan.
- `bmp388.[ch]` is a minimal self-contained Bosch driver (chip-id check,
  calibration, NORMAL mode, float compensation). Verified on the bench:
  `BMP388 ready at 0x77: 24.65 C 993.1 hPa` — temp agrees with the SHT41
  (24.5 °C), so the calibration/compensation parse is correct.

### Reliability — what actually went wrong (resolved)
Two separate red herrings before the real causes were found (4.7 kΩ
pull-ups were present the whole time — signal integrity was never it):

1. **A faulty SHT41.** The original sensor ACKed its address (so `/i2c`
   showed `0x44 OK 3/3`) but could not complete a measurement
   (`ESP_ERR_INVALID_RESPONSE` every read) → no temp/RH anywhere.
   Swapping the unit fixed it. Hardened firmware so a dead/transient
   internal SHT41 isn't lost for the session: `sht41_init` is now
   re-entrant and `publish_full_telemetry` re-probes it once a minute
   (the *external* sensor already had this; the required *internal* one
   didn't).

2. **I²C transaction timeouts, not bus signal.** Once a 3rd device
   (BMP388) shared bus0, the OLED's long bursts (25 B init, ~1 KB flush)
   intermittently failed with GlitchTip `i2c.master: I2C software timeout
   / bus still busy`. Root cause: the IDF i2c_master per-transaction
   timeout *includes the wait for the bus*, and ours were too short
   (50/200 ms) — a flush queued behind a concurrent SHT41 measurement
   (+ retries) blew the timeout. Fix: bump the OLED timeouts
   (`I2C_CMD_TMO_MS` 250, `I2C_FLUSH_TMO_MS` 1000) so it waits out a busy
   bus instead of failing. Verified: 4 clean boots + an 8×-snapshot
   contention stress, zero failures. The OLED task is also self-healing
   now (re-bringup on sustained failure), so it never freezes on a stale
   frame.

The display reads SHT41 from the telemetry-filled cache (not its own live
read) so it never adds a 3rd concurrent caller. (Camera's own 0x3C is the
OV3660 SCCB on a *separate* bus — unrelated.)

## What's on the bench (confirmed 2026-06-12)

`/i2c` scan on `ex01`, bus0 (D4/D5, GPIO5/6):

```
found: 0x31 0x3c 0x44 0x6f
  0x3c  ← the display (TIB098 = generic 128×64 mono OLED)
  0x44  SHT41 (temp/RH)  OK
```

- **0x3C** is the display, as expected. 0x31/0x6f are almost certainly
  false-ACKs (SDA rise-time on the shared bus — same artefact noted in
  the bus1 debug pass; ignore them).
- TIB098 is a **0.96" 128×64 monochrome OLED**. At 0x3C / 128×64 the
  controller is **SSD1306** (the SH1106 1.3" variant needs a +2px column
  offset — confirm before drawing or the image shifts/wraps).
- It is wired on **bus0, which also carries the SHT41** — the *required*
  ambient sensor. This is the single most important constraint below.

## Hard constraints

1. **Must not starve the SHT41.** The OLED shares bus0 with the required
   sensor. The driver MUST go through the existing `i2c_bus_get()` handle
   (the IDF i2c_master driver serialises transactions internally), use a
   short per-transfer timeout, and treat any NACK/timeout as "display
   absent" — degrade silently, never retry-spin, never block a sensor
   read. A 128×64 full-frame flush is ~1 KB over I²C; cap refresh to keep
   bus0 occupancy low (see refresh policy). Robustness over breadth: a
   missing/failed display must be a one-line log, not a fault.
2. **Soft-detect at boot.** Probe 0x3C once; if absent, skip the whole
   subsystem. No display = no behaviour change anywhere else.
3. **Power.** OLED draws ~10–20 mA lit (content-dependent). The power
   budget is already tight (audio + always-reachable block deep sleep).
   The display must NOT be a constant always-on 24/7 draw in the field —
   see visibility policy. On the bench, always-on is fine.
4. **Field is OTA-only.** No display is fielded yet; this is a bench
   feature until/unless a board ships with one. Keep it `Kconfig`-gated
   so non-display boards build identically.

## Library

Recommend **u8g2** via the component manager (`idf_component.yml`):
mature, tiny, rich fonts, page-buffer mode (low RAM), and built-in QR-ish
bitmap support. The alternative is IDF-native `esp_lcd_panel_ssd1306`
(no external dep, but text/fonts are DIY). Given we want crisp small
fonts + an onboarding screen, u8g2 wins. Page-buffer mode keeps each I²C
burst short (good for constraint #1).

## Proposed content — paged status panel

128×64 with the 6×8 font ≈ 21 chars × 8 lines. Design as **auto-rotating
pages** (~4 s each), with event-driven interrupts (a capture/trigger
briefly forces the activity page). All fields below map to telemetry the
firmware already publishes (`state/*`), so nothing new to measure.

**Page 1 — Identity & link**
```
budka ex01      [▮▮▮ ]   ← device id + RSSI bars (state/rssi)
SSID: doma                  ← wifi_mgr current SSID / "AP MODE"
198.51.100.90              ← IP
MQTT ● up   up 3d04h        ← MQTT conn (●/○) + uptime (state/uptime_s)
```

**Page 2 — Environment & power**
```
T 21.4°C  RH 48%           ← SHT41 (state/temp, state/humidity)
MCU 47°C                    ← state/mcu_temp
Bat 87%  4.01V  ▲           ← state/soc, v_bat, crate (▲chg/▼dis/=)
Sol 5.1V 0.32W              ← state/solar_* (hide if INA226 missing)
```

**Page 3 — Activity & mode**
```
MODE: TRIGGERED             ← state/mode (Safe/Triggered/Continuous)
PIR 12   REED closed        ← motion_count / reed
audio ● -42 dBFS            ← audio_active + rms_dbfs (bar)
photo 14  last 12:03        ← capture_count + last event/photo time
```

**Page 4 — Health (auto-shown on error)**
```
fw 1.7.3                    ← state/fw_version
heap 142 KB                 ← state/heap_free
reset: panic                ← state/reset_reason (highlight if not clean)
SD 61% free                 ← sd_storage
```

### Special screens (override the rotation)
- **Onboarding / AP mode** — when unprovisioned, show the AP SSID +
  password + portal URL, optionally as a **QR code** the phone scans to
  join. This is a real field-setup win: read WiFi creds straight off the
  box instead of a serial console (pairs with the WiFi-reconfig work).
- **OTA in progress** — progress bar + version while `cmd/ota` runs.
- **Capture flash** — 1 s "📷 capturing…" when `event/photo` fires.
- **Boot splash** — id + fw version for ~2 s at startup.

## Visibility / power policy (per-mode, reuses existing design)

Don't add a new selector — fold into the existing Safe/Triggered/
Continuous behaviour (same pattern as the power-saving-modes work):
- **Bench / Continuous:** always on (or dim-after-idle).
- **Triggered:** wake the display on PIR/reed/photo for ~15 s, else off.
- **Safe / low-power:** off, or only on during the audio window.
- Optional: tie brightness to `ambient_agc`.

## Open decisions (for you)

1. **Scope now:** just this design doc, or also land a minimal bring-up
   (u8g2 + boot splash + Page 1) on the bench to prove the wiring?
2. **Field intent:** is this bench-diagnostic only, or do you want a
   display in the fielded enclosure eventually? (Drives power policy +
   faceplate cutout on `hw/faceplate`.)
3. **QR onboarding** worth it, or plain-text AP creds enough?
4. Confirm controller is SSD1306 (vs SH1106) — I can detect this at
   bring-up by which offset renders clean.
```
