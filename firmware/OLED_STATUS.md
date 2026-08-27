# OLED status display

Status: **bench DONE — OLED + generic sensor registry + WiFi onboarding**.
Bench-only OLED; the field unit has no display (soft-detect no-ops there).

## WiFi onboarding + captive portal

When the box boots as an access point **and** a display is present, the OLED
shows a WiFi-join QR; scanning it joins the AP and the captive portal opens
the `/wifi` form. Verified end-to-end on a genuinely unprovisioned board
(NVS erased + `have_sta_target()` floor forced false to simulate placeholder
compiled creds; NVS backed up + restored after).

- **Random AP password (unprovisioned *and* display fitted).** On the `unprov`
  first boot — and only when `oled_probe_present()` is true, since the QR is the
  sole way to read the value —
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

## Generic sensor registry

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


---

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

## Driver

`oled.c` is a **self-contained SSD1306 driver** (no external component) on the
shared bus0 via `i2c_bus_get()`, with a vendored public-domain 5×7 glyph table
(`font5x7.h`). Detection is deferred (4 s) with a 3-consecutive-ACK consensus
so early-boot bus churn can't trigger a doomed init; absent ⇒ clean no-op.

## Content — auto-rotating status pages (implemented)

128×64, auto-rotating pages (`s_pages[]` in `oled.c`), each reading the same
caches the telemetry publishes — the display never adds its own I²C sensor
traffic:

- **identity/link** — device id, RSSI, SSID/IP, MQTT + uptime (boot splash at startup)
- **power** — SOC/voltage/charge-rate (battery builds), solar V/W if INA226 present
- **camera** — capture count, last trigger, IR/AGC state
- **net** — WiFi/MQTT detail
- **env** — SHT41/BMP388 readings (fast refresh while a Grove sample is hot)
- **diag** — heap, reset reason, selftest summary
- **levels** — animated mic VU + light bars (fast 300 ms refresh)
- **web-URL QR** — scannable link to the box's web UI

Special screens override the rotation: **WiFi-onboarding QR** when the box is
an unprovisioned AP (scan to join, captive portal opens `/wifi`), and the boot
splash.

## Visibility / power policy (per-mode, reuses existing design)

Don't add a new selector — fold into the existing Safe/Triggered/
Continuous behaviour (same pattern as the power-saving-modes work):
- **Bench / Continuous:** always on (or dim-after-idle).
- **Triggered:** wake the display on PIR/reed/photo for ~15 s, else off.
- **Safe / low-power:** off, or only on during the audio window.
- Optional: tie brightness to `ambient_agc`.
