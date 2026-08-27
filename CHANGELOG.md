# Changelog

All notable changes to this project are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/). Releases are
[SemVer](https://semver.org/) git tags (`vMAJOR.MINOR.PATCH`); pre-1.0, so the
`0.x` line still evolves. The firmware version (`state/fw_version`, OTA
`version.json`) comes from `git describe`, so a tagged build reports e.g.
`v0.1.0` and an untagged commit after it reports `v0.1.0-3-gSHA`.

## [Unreleased]

First-time-builder pass: everything reported against the public repo after
the 0.10.1 release, plus the checks that keep this class of defect from
reaching the public issue tracker again.

### Fixed
- **fw(pir): a bare board no longer reports `"pir": true`.** The boot probe
  tested for one floating pattern and called everything else "driven", so a
  pin held HIGH with nothing wired to it read as a sensor — the one bring-up
  step `/selftest` could not fail. Now three-state: only a line actively sunk
  LOW (an idle AM312) confirms a sensor; "held HIGH" stays unconfirmed and is
  resolved by the existing 3-edge promotion. Also stops seeding the debounced
  state to `false` on an already-HIGH line, which manufactured a phantom
  motion event (and a photo) every boot and started the stuck-high timer that
  surfaced as `pir_wedged`; `pir_wedged()` is now gated on likely_present.
- **server bootstrap fails cleanly.** `init-secrets.sh` cleared its
  cleartext `mosquitto_passwd.plain` only on the happy path, so any docker
  failure left five service passwords on disk silently and indefinitely — now
  removed from an EXIT/HUP/INT/TERM trap. `dev-pki.sh` ran
  `docker run … "$(docker build -q ./toolbox)"`, where `set -e` cannot see the
  build's exit status, so a failed build died as "invalid reference format"
  pointing at the script instead of at the real error.
- **Operator-local files are gitignored and templated.** `flash_safe.sh`'s
  MAC allowlist (`firmware/tools/devices.txt`) was undocumented, had no
  example and was not ignored — one `git add -A` from publishing a fleet's
  MACs. Adds the rule plus `devices.txt.example`. `server/.venv/`, which the
  README quickstart tells you to create, is ignored too.
- **Docs stop contradicting each other.** `idf.py build` is documented
  accurately (profile-less, not broken — IDF auto-applies `sdkconfig.defaults`
  and its `.esp32s3` sibling; skipping `set-target` is the real failure);
  HIL is documented as `./run.sh`, matching the HIL README; and the AP portal's
  password is stated (`chytrabudka` by default, random-per-boot QR only when an
  OLED is fitted) instead of promising a serial-console line that the firmware
  deliberately never prints.
- **HIL refuses to strand a board.** `reset_board` factory-resets the bench
  before any test, but the missing-credentials check ran afterwards — bare
  `pytest` wiped the board and then failed everything with "no route to host".
  The guard now runs before the wipe.
- **SECURITY.md documents the shipped enrollment trust model** (TOFU approval
  + pinned public key, and the `CB_ENROLL_TRUSTED_DEVICES` bypass that must
  not list a field unit) rather than the private MQTT signer's topic/ACL
  anchor.

### Added
- Public export gains a **dangling-reference check**, fatal like the
  leak-check: excluding a path never rewrote the files citing it, so every
  exclusion left dead pointers in shipping sources — invisible privately,
  visible only to someone reading the published repo. Flags excluded paths
  that are still cited (with a `# cite-ok` opt-out for files the docs tell you
  to create) and markdown links with no target. Cleared 14 such references.

## [0.10.1] — 2026-08-26

Docs & public-release patch — no firmware behaviour change (the only firmware
tree touches are two comments). First release published to GitHub.

### Added
- README: field-unit + bench-rig photos, management-UI screenshot (device ids
  pixel-censored, camera thumbnails swapped for CC0 kittens), CI badge,
  build-tier guide, **Try it without hardware** (compose + fake-device), issue
  pointer, linked repository-layout table.
- Public-export pipeline: firmware/tools + server/ + relay/ + metrics-bridge/
  ship; placeholder enrollment CA swap (cmp-asserted); wider scrub map +
  leak-check; real author identity on the squash commit.

### Fixed
- Root README power-mode table now matches the code (enter ≥ 65 %, leave
  < 50 %, safe < 30 %/≥ 35 %; Triggered is the default).
- firmware/README: two `cd firmware` + `firmware/tools/…` double-path blocks;
  `fetch_le_roots.sh` now says WHY it is required (the mbedTLS cert-bundle
  input, per sdkconfig) and the root-README quickstart gained the step; one
  canonical ESP-IDF sourcing line + install link in both READMEs.
- server/README: the mTLS verify block was truncated — replaced with two
  copy-paste commands verified against a fresh stack; local-dev URL +
  /etc/hosts + self-signed note added.
- Solar claims reframed to match reality repo-wide (Solar-Ready; fielded units
  run on USB/powerbank; the MPPT+battery chain is designed, not yet fielded).
- SHOPPING.md rewritten as a plain parts list (procurement diary removed).
- Doc/code drift: cb-<id> AP SSID + MQTT topic naming, stale server status,
  dangling private references (SIGNING_CUSTODY, private commit hashes).

## [0.10.0] — 2026-07-28

Fleet-cutover release: boards, tooling and tests moved off the shared house
broker onto the standalone stack.

### Changed
- Compile-time endpoint defaults + the release tooling point at the stack;
  operator tooling follows; scripted per-device broker migration (with the two
  traps it walks around documented).
- HIL suite runs against the stack broker and got a determinism pass: wait on
  retained state (not a passed edge), one 240 s auto-revert run, per-test
  provisioned-board declaration, one resolver for provisioning creds, EXIF
  asserts inspect a single captured frame, camera sweep opt-in + bench-only.

### Fixed
- fw(mqtt): reboot when enrollment lands after the client identity was
  already decided.
- server: archived-photo dedup by content only (not seq); trusted devices
  exempt from the issuance rate limit; HA state bridge one-way.

## [0.9.0] — 2026-07-27

The standalone server stack and the firmware network/TLS track that talks
to it.

### Added
- **server/**: standalone docker-compose stack — nginx TLS edge, mosquitto
  mTLS broker, manager (enrollment CA + OTA store + web UI), media + metrics
  containers; Swarm/Portainer site deploy; OIDC login incl. a public PKCE
  client, allow-list matching any identity claim (+ regression tests).
- **fw networking**: runtime endpoints (`net_store` candidate ladder,
  `cmd/endpoint`, `state/net`); MQTTS with the mTLS client identity from the
  enrollment cert; HTTPS enrollment transport with TOFU retry + `cmd/cert`
  renew; the embedded sub-CA pinned as the broker trust anchor.
- **fw sensors**: Grove ultrasonic ranger + soil moisture (pin functions
  `sonar`/`soil`), sonar proximity photo trigger, soil/distance on the OLED
  ENV page.

## [0.8.7] — 2026-07-10

- Three camera-only boards onboarded into the fleet.
- MQTT photo cap 200→400 KB (4×400 ring) — fixes stale HA photos.
- Exporter per-host config moved from inventory to group_vars.

## [0.8.6] — 2026-06-27

- OLED blinks only on a real capture (in sync with the jingle); VAD blink
  gated on `vad_enabled`.

## [0.8.5] — 2026-06-23

- OLED layout cleanup — trigger ticks into clean rows.

## [0.8.4] — 2026-06-23

### Fixed
- **Camera AGC was pinned at 1× — the driver wrote the enum ordinal instead
  of the real OV3660 gain ceiling.** Auto-IR night shots were effectively
  dead before this. Real ceiling written, `cam_gainceil` default 32×.
- OLED light bar reads exposure×gain, with VAD/IR threshold ticks.

## [0.8.3] — 2026-06-23

- VAD photo/event trigger fires in Max mode too (was Active-only); default
  D5 → buzzer.

## [0.8.2] — 2026-06-23

- Compile-time default for bus0 flipped to D6/D7 — frees the RTC-capable
  pads (D0–D5) for wake sources.

## [0.8.1] — 2026-06-23

- MAX17048 per-round bus0-recover storm dropped (plain bounded read) — fixes
  shared-bus dropouts / dark OLED.
- Pin map hardening: RTC-capability guard, I²C singleton, boot audit.
- Hibernate: clear EXT1 before arming so a disabled PIR can't wake deep sleep.

## [0.8.0] — 2026-06-22

### Changed
- **BREAKING: device id renamed** `chytra-budka-<m>_<m>` → `cb-<6hex>` —
  MQTT topics, hostname, AP SSID all follow.

### Added
- Capture-time watermark in EXIF + the MQTT caption string.
- Unified web header + merged Status table.

### Fixed
- SD maintenance + photo publish moved off the WDT-watched capture worker;
  retention isolated on its own task with a per-pass budget.
- Robustness audit: oversized stack buffers moved to heap.
- UC96 meter exporter: unified `meter_*` schema, per-host allowlist, BLE
  scan pause while connected.

## [0.7.2] — 2026-06-20

- OLED splash comes up early in boot, boot jingle fires with it.

## [0.7.1] — 2026-06-20

- Audio HIL: matrix-driven tests + `/selftest` GPIO observability.
- `audiofx` fan-out — every sound reaches both outputs; synth tones at full
  scale.

## [0.7.0] — 2026-06-20

Audio output release.

### Added
- LEDC square-wave buzzer as a mappable pin function; staccato melodies,
  legato SFX (`cmd/sfx`), optional capture beep, boot chime.
- Experimental PCM playback over I2S PDM TX (a DIY 1-bit DAC) with loudness
  limiter and test tone; buzzer/PCM selection via `spkr_tone`.
- Arbitrary melodies + looping alarm over MQTT; `AUDIO.md`.

## [0.6.3] — 2026-06-20

- `power_profile` ladder + hibernate deep-sleep; OLED/wake overhaul.
- HA lovelace: FW-agnostic entity resolver + power_profile select.

## [0.6.2] — 2026-06-19

- VAD DC-offset fix; PIR light-sleep wake; powersave knobs; OLED dashboard.
- Faceplate v012 print export.

## [0.6.1] — 2026-06-19

- Faceplate v012: wire comb + baked print config.

## [0.6.0] — 2026-06-18

- Universal I²C transport — any peripheral on any bus.
- Camera-faceplate CAD iterations (XIAO Sense cradle, camera recess).

## [0.5.3] — 2026-06-14

- HIL: TWDT-hang window widened 35 s→55 s (multi-task watchdog timing).

## [0.5.2] — 2026-06-14

- OTA tooling: auto clean-rebuild of a stale binary under `--sign`.

## [0.5.1] — 2026-06-14

### Added
- OLED paged status display: button cycling, env dashboard with big numbers,
  camera light bar, web-URL QR, reboot/OTA overlays; on-demand animated boot
  logo; external OLED-control button as a pin function.
- Project logo artwork.

### Fixed
- SoftAP onboarding crash + WiFi scan picker + mobile-friendly list;
  onboarding creds commit on association when no known-good set exists.
- Bench/debug builds default `ota_enabled` OFF.

## [0.5.0] — 2026-06-12

First fleet release since 0.4.7 (the 0.4.8 tag was the universal/PM build whose
field OTA RAM-starved TLS and was rolled back — see the BLE note below).

### Added
- **WiFi onboarding on the bench OLED.** An unprovisioned boot with a display
  present shows a WiFi-join QR on the SSD1306; scanning it joins the box's AP
  and the captive portal opens the `/wifi` setup form. On the `unprov` first
  boot the AP uses a **random per-boot password** (`wifi_mgr_use_random_ap_pass`,
  replacing the public `AP_PASS_DEFAULT`; never logged by value) so a fresh
  board isn't reachable on a guessable credential — the operator reads it from
  the on-screen QR. Sticky AP-only keeps stable creds; operator-custom AP creds
  always win. HW-verified end-to-end on a genuinely unprovisioned bench.
- **Captive-portal auto-pop** (`dns_hijack.[ch]`): a UDP:53 responder answering
  every A query with the AP IP, DHCP option-6 advertising the box as DNS, and a
  captive 404 handler serving the `/wifi` form — so the portal opens
  automatically on join (previously a comment claimed a DNS responder that
  never existed, so it never did).
- **WiFi scan picker in the captive form**: a "Scan networks" link
  (`/wifiscan?to=wifi`) lists nearby APs and pre-fills the SSID, staying in the
  lightweight captive flow.
- **Onboarding AP creds echoed to the local console** (~every 30 s, AP mode
  only — MQTT + GlitchTip are down then, so it never leaves the box): an
  operator fallback when the QR won't scan, and how the serial-free HIL learns
  the per-boot random AP password to drive the provisioning gate.
- **OLED swaps QR ↔ status on AP join/leave**: while onboarding, the join QR
  shows until a client associates to the SoftAP, then the panel flips to the
  status page (AP id + client count + portal address + sensors); it flips back
  to the QR when they disconnect. Driven by `wifi_mgr_ap_sta_count()`, polled
  by the OLED refresh loop.
- **Generic I²C sensor registry** (`sensors.[ch]`): one `CB_SENSORS[]` table
  drives MQTT telemetry, HA discovery, the `/` HTML UI, the OLED, and `/i2c` +
  `/sensors` — adding a sensor is now one driver + one row. Historic HA
  `object_id`s (`temp`/`humidity`/`temp_ext`/`humidity_ext`) preserved.
- **BMP388** pressure/temp driver (`bmp388.[ch]`, bench bus0 @0x77 → `temp_bmp`
  + `pressure`); bench **SSD1306 OLED** status display + QR + custom boot logo.
- `/sensors` endpoint; MAX17048 on both buses surfaced in `/i2c` + `/sensors`.

### Fixed
- **Intermittent bus1 (bit-bang) SHT41**: the software I²C bus had no mutex, so
  a telemetry refresh racing the HTTP scan corrupted GPIO transfers. Added a
  FreeRTOS mutex on every bit-bang transaction (10/10 vs ~0/6 reads under load).
- Honest `/i2c` + `/sensors` verdicts (real reads, not address probes that
  false-ACK on the bit-bang bus); internal SHT41 boot-init recovery.

### Field deploy note
- This image compiles in `cb_pm`. The field board must have **`ble_enabled=OFF`
  before OTA** or TLS RAM-starves (the 0.4.8 silent-brick); recover a failed
  field OTA by **power-cycling** (bootloader rolls back the unvalidated app) —
  never USB-flash the field.

## [0.4.7] — 2026-06-10

Host-side / infra release — **firmware is byte-identical to v0.4.6** (the change
below is in `metrics-bridge/`, not `firmware/`). Tagged so `main` HEAD is a clean
release the OTA tooling can ship from directly.

### Added
- **`metrics-bridge`: ingest native BLE meter telemetry into Mimir** — the
  on-device BLE (UC96) meter readings now flow through `cbprom.py` into Mimir
  with alerts + the Grafana view, alongside the existing UC96-over-BT path.

## [0.4.6] — 2026-06-10

### Fixed
- **Crash (panic loop) when web-admin auth is enabled under load.** A PSRAM-
  stacked HTTP worker (the async `/capture` task — given a PSRAM stack to survive
  BLE-on internal-DRAM fragmentation) resolved the basic-auth creds on every
  request via `auth_store_get_effective()`, which read NVS — a flash op that
  disables the PSRAM cache. With the task's own stack in PSRAM that trips
  `esp_task_stack_is_sane_cache_disabled()` → `panic_abort`. Coredump-confirmed
  on the bench under concurrent authenticated `/capture` load on a weak link with
  BLE on (the latent risk commit 72ee6e5 flagged). The field never hit it only
  because auth was disabled (placeholder creds) — enabling `/config` auth would
  have exposed it.
  Fix: `auth_store` now caches the effective creds in **internal RAM**, read from
  NVS once at boot (`auth_store_init()`, app_main/internal stack) and refreshed on
  `auth_store_set()` (the mqtt task). The per-request gate serves from RAM with no
  flash access, so it is safe on any task stack.

### Added
- **`test_stress_soak.py`** (opt-in `-m stress`, `CB_STRESS=1`) — concurrency +
  soak load test: hammers fresh TLS handshakes + authenticated bulk endpoints
  (`/mic.wav`, `/capture`) under BLE-on, asserts no reboot, reports TLS-error /
  MQTT-drop degradation. This is what reproduced the crash above and is the
  before/after gauge for memory/TLS tuning.

### Notes
- **HW-AES kept ON** (power). Under BLE-on + concurrent bulk TLS the `esp_aes`
  DMA-buffer alloc can still fail ("esp-aes: Failed to allocate memory",
  CHYTRA-BUDKA-QK) — a **non-crash degradation** (TLS retries). Software AES
  (no DMA buffer) was bench-tested: frees ~2 KB contiguous internal DMA and
  eliminates the OOM, but costs CPU/power; deferred for the field's low-rate-TLS
  duty cycle. The crash-loop that previously amplified this OOM is gone with the
  auth fix. See `firmware/sdkconfig.defaults` (MBEDTLS_HARDWARE_AES note).

## [0.4.5] — 2026-06-09

### Fixed
- **`task_wdt` reboots root-caused to a busy-spin in `/mic.wav`.** Coredump from
  bench v0.4.4 (panic: *"main (CPU 0), IDLE0 (CPU 0) did not reset the watchdog"*)
  pinned it to `mic_wav_get`'s empty-ring wait: `vTaskDelay(pdMS_TO_TICKS(8))`
  rounds to **0 ticks** at `CONFIG_FREERTOS_HZ=100`, and `vTaskDelay(0)` does not
  yield to lower-priority tasks. When the audio ring stayed empty (notably with
  BLE on, which starves the i2s DMA) the loop spun at httpd-task priority,
  starved the main loop + IDLE0 on CPU0, and the task watchdog rebooted ~30 s
  later. This is what made the fleet "unstable since BLE" — the bug is the
  tick-rounding spin; BLE is the amplifier that keeps the ring empty.

### Changed
- **New `cb_delay_ms()` helper (`main/cb_time.h`)** floors every short wait at one
  tick so it always yields. Replaces the silent `pdMS_TO_TICKS(<10 ms) → 0`
  footgun at all 8 sites (`mic_wav_get`, `sht41`, `i2c_bb`, `pir`, `battery`) —
  the sensor ones were also waiting 0 ms, a latent timing bug (e.g. SHT41 needs
  ~8 ms to convert; likely behind some `selftest degraded: sht41` noise).
- **`/mic.wav` hardened**: ends the stream after a 2 s no-audio stall instead of
  holding the socket (and the single `s_mic_busy` slot) open for the full cap;
  default cap lowered 300 → 60 s to match the handler doc.
- **Anti-regression**: `make test` now grep-gates against bare
  `vTaskDelay(pdMS_TO_TICKS(<10 ms))`, and a new HIL test (`test_mic_stream.py`)
  asserts `/mic.wav` always returns within a bound without rebooting the board.

### Tooling
- **`ota_upload.sh` is now release-only**: refuses to deploy an untagged
  `git describe` dev build (`vX.Y.Z-N-gSHA`) — the OTA feed must carry tagged
  SemVer releases (`--force` overrides for a deliberate dev/canary push).
- **`ota_upload.sh` auto-sources the HIL provision creds** from the active (or
  uniquely-configured) NetworkManager WiFi when `CB_PROVISION_SSID/PSK` aren't
  set; reading a stored PSK can still need an authorized session, so an explicit
  export (or `sudo`) remains the fallback.

## [0.4.4] — 2026-06-08

### Fixed
- **"no photo yet" placeholder painted over the photo.** The `.frame` placeholder
  is `position:absolute` while the `<img>` was static, so it stacked ABOVE the
  image (and over a running stream). Fixed the z-order — placeholder (0) < image
  (1) < caption (2) — and JS now hides the placeholder once `#view` loads (covers
  the stream-letterbox case too) and shows it again on error.

### Changed
- **Mic gain folded into the player.** Dropped the separate gain slider; playback
  routes through a fixed 10× Web-Audio boost and the `<audio>` element's own volume
  control sets the effective gain (0–10×, default ~3×). One control, in the player.

## [0.4.3] — 2026-06-06

### Fixed
- **`/capture` + MJPEG stream now WORK with BLE on** (v0.4.2 only stopped them
  hanging; they still 503'd). The per-request HTTP camera task stacks (12 KB
  `/capture`, 8 KB `/stream`) are now allocated from **PSRAM**
  (`xTaskCreatePinnedToCoreWithCaps` + `MALLOC_CAP_SPIRAM`), so they no longer need
  a contiguous internal-DRAM block — which BLE-on fragments down to ~2-3 KB. Bench
  BLE-on: `/capture` 200 + `/stream` 200, no internal-DRAM leak over repeated calls
  (self-delete via `vTaskDeleteWithCaps`). Capture is ~25 s BLE-on (camera/BLE coex)
  but completes reliably. **Caveat:** a PSRAM task stack must not be accessed during
  a flash-cache-disable window — these tasks do camera + TLS only (no flash), but a
  *concurrent* NVS commit / OTA / coredump is a latent crash risk (low; recoverable
  by reboot + the OTA rollback net).

### Changed
- **BLE host-pool buffers trimmed** to reclaim a little internal-DRAM headroom:
  ACL_BUF 12→6, MSYS 8→4, transport ACL/EVT 10/12→6/8, scan-dup cache 20→10.
  `MAX_CONNECTIONS` stays 3 (meter count unchanged) — only per-connection queue
  depth shrinks, fine for the UC96's low-rate 36-byte notify frames.
- `FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` pinned (default on S3) for the PSRAM stacks.

## [0.4.2] — 2026-06-06

### Fixed
- **`/capture` + MJPEG stream no longer HANG when BLE is on (now degrade gracefully).**
  Root cause: each request spawns a worker task (12 KB / 8 KB stack) and BLE-on
  fragments internal DRAM down to a ~2–3 KB largest free block, so `xTaskCreate`
  fails — and the old failure path completed the async request with **no response**,
  so the client hung indefinitely. The failure paths now return **503** ("low memory;
  try with BLE off") instead of hanging. BLE **off**, both work as before; BLE **on**,
  live HTTP capture/stream are limited by internal-DRAM scarcity (the auto-capture
  archive + `/last.jpg`/`/photos`/`/view` still work). Fully restoring them BLE-on
  needs the internal-DRAM headroom from a BLE-buffer trim — tracked separately.
- **Home-page preview no longer jumps; Capture keeps the overlay.** The `/` photo
  frame reserves the camera's **capture aspect-ratio (read from `cam_framesize`, not
  hardcoded)** and shows a placeholder when there's no frame, so the layout doesn't
  reflow when `/last.jpg` loads or 404s. Clicking **Capture** now re-shows the
  caption with the *fresh* frame's EXIF (via `/last.json`) instead of hiding it;
  **Stop-stream** reloads the latest stored frame (+ its caption) instead of clearing.

### Changed
- **BLE on/off moved to the `/ble` page** — Enable / Disable / Reboot buttons, off
  the generic Settings form (one control for the `ble_enabled` knob). BLE still
  starts cleanly only at boot, so a Reboot button sits alongside.

## [0.4.1] — 2026-06-06

### Fixed
- **`esp-aes: Failed to allocate memory` under TLS load with BLE on.** With BLE
  enabled the BT controller takes ~52 KB internal DRAM, leaving the DMA-capable
  heap at ~2-3 KB largest free block (bench-measured: int_free 62→11 KB, dma
  28→3 KB). Concurrent fresh TLS handshakes then starved the HW-AES DMA alloc →
  OOM that destabilised the board. **HTTPS `max_open_sockets` 8→4** bounds peak
  concurrent AES demand: bench BLE-on, the OOM is gone (GlitchTip esp-aes stops
  incrementing under load) and real traffic (keep-alive / sequential) is 12/12;
  a synthetic fresh-handshake storm is now cleanly *refused* (ConnectError, board
  stays up) instead of OOMing.

### Changed
- **Internal-DRAM diet (helps BLE-on headroom).** Per-request HTTP scratch
  buffers (EXIF header peeks, the `/photo` read buffer) now allocate from PSRAM
  (`heap_caps_malloc(MALLOC_CAP_SPIRAM)` with internal fallback) instead of
  internal DRAM, and `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=16384` reserves
  internal headroom for DMA/AES/BT so PSRAM-eligible allocations don't eat it.
- **Multicore: lwIP TCPIP task pinned to CPU0** (`CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`).
  It defaulted to NO_AFFINITY at prio 18 and could be scheduled on CPU1 — where
  it would preempt the prio-10 audio pump and jitter the 32 ms PDM cadence.

### Known issue
- **`/capture` (on-demand HTTP capture) hangs when BLE is on** — reproduced on
  the committed v0.4.0 too, so it predates this change. The PIR/VAD worker
  capture path (the one that actually fills the SD archive) is unaffected.
  Tracked for a separate fix.

## [0.4.0] — 2026-06-06

### Added
- **On-image timestamp + telemetry overlay, in HTML/CSS (no pixel burn-in).**
  The device reads the EXIF it already writes (`jpeg_stamp.c`) back out and shows
  capture time + telemetry as a caption over the photo — on the home page `/`
  (the last stored frame) and in a new single-photo **viewer `/view?d=&f=`** that
  also lists the full EXIF below the image (trigger, device, firmware, sensor,
  AGC/IR/RSSI/MCU&nbsp;temp/battery/SOC/heap/uptime/…). The `/photos` gallery now
  links rows to `/view`. Reading EXIF is a bounded APP1-header scan, **not** a
  JPEG decode, so it's cheap on the S3 — unlike the burned-in overlay that was
  tried and removed (NOTES.md). Pixel burn-in stays dead; only the presentation
  moved to the browser.
- **`exif_read` module.** New `main/exif_read.c` — a defensive, pure-libc reader
  for the APP1/TIFF/IFD layout `jpeg_stamp.c` emits (both byte orders; every
  offset bounds-checked, since the JPEG is untrusted SD content). Plus
  `exif_json_num()` to pull numbers out of the UserComment telemetry JSON.
  Host-tested in `tests/native/test_exif.c` (round-trip + malformed inputs).
- **`GET /photo/exif?d=&f=`** and **`GET /last.json`** — the same metadata as
  JSON (telemetry embedded verbatim), for live debugging / scripting.
- **`camera_last_jpeg_peek_header()`** — copies just the front of the cached
  JPEG so the `/` overlay reads EXIF without duplicating the whole UXGA frame.

### Fixed
- **BLE meter values stretched the table.** The `/ble` + home-page meter rows
  used non-breaking spaces *between* fields too, making each row one
  unbreakable line; now only value↔unit is `&nbsp;`, fields are separated by a
  normal (breakable) space so the row wraps instead of widening the layout.

## [0.3.0] — 2026-06-06

### Added
- **BLE device management on the device web UI (`/ble`).** Local page to scan
  in-range BLE devices, name + save them, and watch their live state and last
  measured values — no Home Assistant needed. Saved devices form an **allowlist**:
  the firmware GATT-connects a UC96 (and ingests a BTHome sensor) ONLY if its MAC
  is saved, so a neighbour's meter is surfaced in the scan but never acted on.
  Bench-verified end-to-end: scan → save+name → auto-connect → `streaming` with
  live V/I/P/Wh/temp. Same top chrome as `/config`. Also `GET /ble.json` (live
  snapshot for polling) and `POST /ble/{name,forget,scan}`.
- **BTHome v2 passive sensors.** `ble_parse_bthome()` is now wired into the scan:
  a saved BTHome thermo-hygrometer (service-data `0xFCD2`) is decoded and
  published per device under `<base>/sensor/<mac>/{temperature,humidity,battery,voltage}`
  with HA discovery (throttled, passive — no connection). Unencrypted v2 only.
- **`ble_store` (NVS allowlist).** New `main/ble_store.c` — a small MAC→friendly-name
  map in its own NVS namespace (mirrors `wifi_store`), backing the allowlist + names.

### Changed
- BLE scan now runs continuously while enabled (not only until the meter slots
  fill) so the web UI keeps discovering candidates and observing BTHome; the
  per-meter connect is gated by the allowlist. The home page (`/`) gains a **BLE**
  link and, when a meter is streaming, a **BLE meters** summary table (last
  values) below the on-board sensors; `/ble` carries the same top nav as `/config`.
  Values render with a non-breaking space between number and unit so they don't
  wrap apart.
- Note: runtime `ble_enabled=ON` reliably starts BLE only at boot (the BT
  controller needs a large contiguous internal-DRAM block that a running system's
  heap is too fragmented to provide). The `/ble` page says so and points at a
  reboot when BLE is enabled-but-not-running.

## [0.2.0] — 2026-06-06

BLE meter reading, now in the single canonical fleet image, plus a build-profile
cleanup. Deployed as a vault-signed OTA after a green HIL gate.

### Added
- **BLE meter reading — Atorch UC96 power meters.** New optional NimBLE
  subsystem (`main/ble.c`, gated by `CONFIG_CHYTRA_BUDKA_BLE`; runtime knob
  `ble_enabled`, **default OFF**). Active low-duty scan finds UC96 meters by the
  `0xffe0` service UUID (ADV) or the `UC96_BLE` name (scan-response), connects,
  negotiates MTU (the report frame is 36 B > the 23 B default), subscribes to
  `0xffe1` notifications, decodes them with the host-tested `ble_parse_uc96()`,
  and publishes V/I/P/Wh/temp per meter to `<base>/meter/<mac>/…` with per-meter
  HA discovery. Up to 3 meters at once (field rig daisy-chains two), keyed by
  MAC; HA friendly-names downstream. WiFi keeps RF priority via SW coexistence
  (`ESP_COEX_PREFER_WIFI`). Bench-verified reading a live meter; 2-meter
  concurrency coded but untested. See [`firmware/BLE.md`](firmware/BLE.md).
  Compiled into BOTH images (field + bench); with `ble_enabled` OFF the BT
  controller is never initialised, so a board is byte-for-byte unaffected until
  it's flipped on.
- **Internal-DRAM telemetry in `diag/selftest`** — `int_free` / `dma_free` /
  `dma_largest` (`MALLOC_CAP_INTERNAL` / `MALLOC_CAP_DMA` + largest contiguous
  DMA block). `esp_get_free_heap_size()` is PSRAM-inclusive and hides the scarce
  internal pool that the BT controller, WiFi, and i2s/camera DMA actually share.

### Changed
- **Build profiles consolidated to two: `field` (the one signed OTA image) and
  `bench` (the one debug image).** They build the SAME code — BLE + the memory
  diet now live in the shared `sdkconfig.defaults.esp32s3`, not a per-profile
  overlay — and differ only by the `/debug/*` endpoints (plus, inherently, the
  OTA signature and poll cadence). `tools/build.sh` no longer wires the
  `signed`/`production`/`production-yubikey`/`secureboot-test` profiles; those
  overlays stay on disk for the documented Secure-Boot path (`SIGNING_CUSTODY.md`),
  built manually via `idf.py` if ever needed.
- **BLE memory diet** (now in `sdkconfig.defaults.esp32s3`, fleet-wide):
  `SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, smaller WiFi static RX/TX buffers
  (`STATIC_RX=6`/`STATIC_TX=8`/`RX_BA_WIN=6`), trimmed BT/NimBLE pools
  (`MAX_CONNECTIONS=3`). The BT controller lives only in internal DRAM; without
  the diet, enabling BLE starved i2s/lwip and `task_wdt` crash-looped the box
  into safe mode. The WiFi-buffer trim applies even with BLE off; bench-validated
  (provision/MQTT/OTA stable on a comparable link).

## [0.1.0] — 2026-06-06

First tagged release. Deployed to the fleet (bench `ex01` + field `ex02`)
as a vault-signed OTA after a green HIL gate. Captures the work below.

### Added
- **Real on-device FLAC encoder** — libFLAC 1.5.0 vendored at
  `components/flac/` (with IDF wrapper + hand-written `config.h`), so
  `flac_enabled` produces real FLAC instead of the PCM stub fallback
  (~half the audio stream size, lossless). ~150 KB flash. On-device runtime
  validation (encoder memory, relay-side decode) still pending.
- **12 new live NVS knobs for field tuning** (cmd/cfg + web `/config` + auto HA
  entities, no reflash): camera image levels `cam_brightness`, `cam_contrast`,
  `cam_saturation`, `cam_sharpness`, `cam_ae_level`, `cam_wb_mode`,
  `cam_special_fx`, `cam_gainceil` (applied live via `camera_apply_tuning()`);
  and battery mode-FSM thresholds `soc_cont_enter/leave`, `soc_safe_enter/leave`
  (read live in `mode_tick()`, replacing the compile-time `SOC_*`). Defaults
  reproduce the prior hard-coded behaviour exactly.
- **SD card date-tree layout + autoprune + fast gallery.** Captures now land in
  `/sdcard/YYYY-MM-DD/HHMMSS_<seq>_<mac>_<trig>.jpg` (pre-SNTP shots in
  `/sdcard/boot/`; legacy flat files served as the `root` bucket) so a 10k-file
  card no longer pays a full-directory scan per request or per write. Space-based
  autoprune (`sd_autoprune` ON by default, `sd_min_free`, `sd_keep_days`) drops
  the oldest day-dir when the card fills. On-device migration folds loose
  flat-root JPEGs into day buckets. New `sd_layout.c` is pure, host-tested
  (`tests/native/test_sd_layout.c`).
- **WebUI photo serving hardened/fast:** day-bucketed `/photos` index,
  `/photos.json` API, immutable `Cache-Control` + `ETag`/304 + `Range`/206 on
  `/photo`, and a PSRAM listing cache keyed on a capture generation counter.
- **OTA pending-verify gate** (`ota.c`): the board won't pull a new image while
  the running one is still pending-verify (not yet mark-valid'd) — never discards
  the rollback safety net by chaining an unconfirmed image.
- **Staged canary OTA rollout** (`ota_upload.sh`, default; `--no-canary` to skip)
  + **`ota_rollback.sh`** for a fast re-serve of an archived signed build.
- **Reproducible build** (`CONFIG_APP_REPRODUCIBLE_BUILD`) + per-version ELF
  archive (local-only, `ota_upload.sh`) so a field/bench coredump is decodable.
- **Mandatory HIL gate before every field deploy**: `ota_upload.sh` runs the
  reset→AP→provision→STA pytest lifecycle and refuses to upload unless green
  (`--no-hil` for emergencies only).
- **SRE / Twelve-Factor hardening pass** (8 phases): auth on all `/debug/*` +
  enroll fragment bounds; Python daemons (`cbprom.py`, `enroll.py`) reliability +
  require an explicit MQTT broker host; alerting + SLOs as code
  (`metrics-bridge/alerts/`); config→NVS where it belonged; `RUNBOOK.md` +
  capacity/power doc.

### Changed
- **STA WiFi connects to the STRONGEST AP**, not the first one found:
  `WIFI_ALL_CHANNEL_SCAN` + `WIFI_CONNECT_AP_BY_SIGNAL` (no RSSI floor — a floor
  could reject the only usable AP). Fixes latching onto a far/weak AP at sites
  with several APs on one SSID.
- **Partition table offset unified to `0x10000` fleet-wide** in the base
  `sdkconfig.defaults` (removed the per-profile 0x8000-vs-0x10000 split that
  could OTA-brick a board whose table offset didn't match the image).

### Fixed
- **Audio relay `connect()` can no longer task_wdt the board on a flaky link**:
  `IdfTransport::connect()` is now a non-blocking connect + bounded `select()`
  (5 s) — `SO_SNDTIMEO` does not bound a blocking TCP handshake, so a lost SYN
  used to block ~75 s (past the watchdog) on the TWDT-subscribed audio task.
- **HIL deploy gate made deterministic on real hardware**: the lifecycle pins
  `ota_enabled=OFF` (and `test_ota`/`cfg_reset`/`factory_reset` restore it) so the
  bench can't self-downgrade to a stale server image mid-run; `test_ota`
  default-skips unless `CB_OTA_STAGED=1`; reboot-recovery budgets matched to the
  firmware timeouts; `pytest-rerunfailures` (`--reruns 2`) absorbs transient
  WiFi-latency spikes without masking real faults.

### Security / signing

- **Soft signed-OTA (verify-on-update)**:
  `sdkconfig.defaults.signed_soft` (`SECURE_SIGNED_APPS_NO_SECURE_BOOT` +
  `SECURE_SIGNED_ON_UPDATE`, RSA-3072) makes the running app verify the RSA-PSS
  signature of every OTA image — no eFuse burn, reversible.
- **YubiKey-gated `age` vault for the signing key** (`tools/setup_signing_vault.sh`):
  the RSA-3072 private key lives age-encrypted, a YubiKey touch unlocks it into
  tmpfs for one signing run, `espsecure` does the PSS sign in software. Custody,
  recovery, and the YubiKey-5.7 on-card-HSM successor are in `SIGNING_CUSTODY.md`.
- `tools/sign_with_yubikey.sh` rewritten to the vault flow (age-unlock → sign →
  verify → shred); replaces the dead `openssl -engine pkcs11` on-card path
  (on-card RSA-3072 PSS is infeasible on the YubiKey 5.1.2 — see SIGNING_CUSTODY).
- **OTA-reject proof on real infrastructure** (bench `ex01`, 2026-05-29): a
  wrong-key OTA image was rejected on-device (`esp_image: signature bad` →
  `New image failed verification`, board kept running); the same image signed
  with the vault key was accepted, installed to `ota_1`, and booted.
- **Hardware Secure Boot v2 burned on the field board** (`ex02`, 2026-05-30):
  RSA-3072 public-key digest in eFuse; every boot the ROM verifies the
  bootloader and the bootloader verifies the app (RSA-PSS). Recoverable config
  (`sdkconfig.defaults.secureboot_test`) — no Flash Encryption, secure-download
  mode left enabled — so a *signed* image can still be USB-flashed to recover
  (with a STABLE esptool; the IDF-bundled `esptool v5.3.dev3` can't drive
  secure-DL). Validated: signed boots, wrong-key rejected, secure-DL reflash works.
- **Fleet partition layout standardized on `PARTITION_TABLE_OFFSET=0x10000`**
  (`sdkconfig.defaults.field`): the signed Secure-Boot bootloader overflows the
  stock 32 KB head, and a single OTA image must match each board's table offset,
  so all fleet boards use 0x10000 (app slots `ota_0`/`ota_1`/`storage` unchanged).
- **Signed OTA deployed to the fleet**: both boards run the same vault-signed
  image from `ota.example.com`; field (HW Secure Boot) + bench (soft signed-OTA) each
  verify the RSA-PSS signature before booting.
- **Network watchdog** (`CHYTRA_BUDKA_NET_WATCHDOG_S`, default 600 s): self-reboot
  if MQTT stays down past the threshold, to recover a wedged network stack;
  routed through `diag_pre_boot_fail_set()` so repeated reboots ramp into
  safe-mode instead of tight-looping.
- Replaced the `tools/glitchtip.sh` wrapper with `glitchtip-cli`; fixed its
  resolve-405 (issue mutations use the org-level endpoint, not project-level).
- **UXGA stills (1600×1200, 4:3) + matched 4:3 stream (XGA 1024×768)**: raised
  the camera framesize cap to 15 and defaulted stills to q=12 so a full UXGA
  frame fits the 160 KB MQTT image buffer (which was resized to match the
  200 KB photo-queue slot cap).
- **Crash-loop safe mode**: after 5 consecutive crash-boots without a clean run,
  boot control-plane-only (skip camera/audio, keep WiFi/MQTT/OTA/HTTP) so a
  corrective OTA can still land on the unreflashable field unit.
- **Post-boot mic-death detection** + **I2C bus0 recovery** (`i2c_bus0_recover()`):
  the periodic selftest flags `mic_stalled` when the capture-frame counter stalls
  in Continuous mode; SHT41 (a required sensor) now recovers a wedged shared bus
  instead of failing until reboot.

### Changed
- **OTA mark-valid decoupled from camera/audio → gated on MQTT connectivity**:
  a dead sensor or a boot-time `xTaskCreate` OOM no longer rolls back an
  otherwise-good, recoverable image; degraded sensors surface via
  selftest→GlitchTip instead. Keeps OTA-recoverability the sole rollback gate.
- **`CONFIG_LWIP_MAX_SOCKETS` 10 → 16** — the netconn ceiling was the lone
  bottleneck behind a network-stack wedge: HTTPS(4) + redirect(3) + MQTT(1) +
  audio-relay(1) ≈ 9 baseline left no headroom for upload / OTA / GlitchTip
  bursts, so `socket()` started failing. `MAX_ACTIVE_TCP` was already 16; raised
  sockets to match.
- Migrated firmware to **ESP-IDF v6.0.1 / mbedTLS 4.0.0** (from v5.5 /
  mbedTLS 3.6.6). v6.0.1 is now the only supported toolchain.
- Reworked EC keypair generation in the TLS enrollment path onto the **PSA
  Crypto API** (`psa_generate_key` + `mbedtls_pk_copy_from_psa`) — portable
  across mbedTLS 4.0.0 and 4.1.0, no private headers, HW-backed RNG.
- `httpd.max_open_sockets` 6 → 4 (TLS 1.3 per-session memory headroom).

### Fixed
- **TLS 1.3 HTTPS handshake crash-loop on mbedTLS 4.0.0**: keeping
  `CONFIG_MBEDTLS_DYNAMIC_BUFFER` enabled left the TLS 1.3 server's input
  buffer NULL at ClientHello parse → `LoadProhibited` on every handshake.
  Disabled it; rapid-reconnect handshakes now succeed with no OOM regression.
- lwIP build break on ESP-IDF v6.0.1 (`struct dhcp` used before its forward
  declaration in `lwip_default_hooks.h`). Worked around with a project-level
  `-include lwip/dhcp.h`; upstream fix submitted as
  [espressif/esp-idf#18668](https://github.com/espressif/esp-idf/pull/18668).
- **GlitchTip event flood**: the log hook now strips the IDF `(boot-ms)` prefix
  (so identical errors GROUP instead of forking a fresh issue each) and denylists
  benign client-side TLS resets (`-0x0050`, idle `select() timeout`). A degraded
  selftest verdict now ships a direct GlitchTip event (the hook installs only
  after the 180 s mark-valid, so boot-time verdicts were previously lost).
- **"stream profile apply failed" (HTTP 500)** when opening the live stream
  during a capture — root-caused and fixed: the capture held the *sensor*
  mutex through the SD write + MQTT publish + IR-hold (multi-second on a slow
  SD), starving the stream's profile-apply. Shortened the critical section to
  sensor ops only (IR + capture + EXIF stamp → standalone heap buffer); SD /
  MQTT / cache now run OUTSIDE the mutex (each sink already thread-safe), plus
  a retry in the stream handler as a safety net.
- **Burned-in image watermark: removed** (was added then reverted). On-chip
  JPEG (OV3660, no OSD) + no HW JPEG codec on the S3 means burning pixels needs
  a full software decode→re-encode — ~3.6 s/UXGA still, measured. The capture
  metadata stays in EXIF + the MQTT event; timelapse/HA overlay downstream.
  See NOTES.md. (Drops ~64 KB of flash — the JPEG converters are unlinked.)
- On-device auth parity: the I²C diagnostic endpoints now honor basic-auth, and
  a loud boot warning + GlitchTip event fire when basic-auth is disabled by
  placeholder `HTTP_BASIC_*` creds.
- `ota_upload.sh`: the always-rebuild bug that clobbered a pre-signed binary
  (find-precedence), plus a fail-closed pre-upload signature check.
- Daemon crash-loop guards: `relay.py` wraps config load; `cbprom.py`/`enroll.py`
  use `connect_async` so a broker-down-at-startup doesn't exit before reconnect.
- **public-export leak-check hardened**: case-insensitive (an upper-case MAC was
  slipping past lower-case-only rules), `firmware/tools` excluded (held the
  YubiKey serial), plus shape-based catch-alls (private-range IPs, PEM keys,
  JWTs) so an unknown secret type also fails the build closed.

### Docs
- Added GPL-3.0 (`LICENSE`) + CC-BY-SA-4.0 (`LICENSE-docs`), `CONTRIBUTING.md`,
  `SECURITY.md`, this changelog.
- Accuracy pass on the toolchain/signing references; `SIGNING_CUSTODY.md`
  (private) is the live runbook for the now-deployed vault signing + field
  Secure Boot, incl. the recovery path and the post-OTA mark-valid gotcha.

### Known not-yet-done
- **Flash Encryption** + the full eFuse lockdown (`sdkconfig.defaults.production`:
  FE release mode, ROM-DL disable, NVS encryption) — NOT deployed. Signed OTA and
  Secure Boot v2 (key-digest only, recoverable) ARE now deployed (see Added); the
  remaining production overlay adds the unrecoverable eFuse burns and stays a
  template until there's a reason to give up field recoverability.
- Server-side companions (relay, Home Assistant, metrics) consolidation into a
  container/Helm bundle.
- Winter (sub-zero) power/thermal hardware revision.
