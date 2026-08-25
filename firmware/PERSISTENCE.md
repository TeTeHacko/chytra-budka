# Persistence — what survives what

Until this doc existed the answer to "does X survive a reboot?" was scattered
across [partitions.csv](partitions.csv) comments, the firmware config schema,
and (mostly) tribal knowledge. This page consolidates the **persistence
boundary** so a fix that
relies on state surviving a reset doesn't accidentally rely on a layer that
doesn't actually persist.

The board has four persistence layers, each with different rules. Treat the
table as the source of truth — when a bug says "the value reverted" or "the
counter dropped", look up which layer that value lives in.

## Truth table — survival per reset class

| Layer / value | soft reset (`esp_restart`) | brownout / power loss | panic + reboot | OTA upgrade | OTA downgrade | full NVS erase | factory reflash |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **NVS schema knobs** (`vad_*`, `cam_*`, `cap_led_en`, `reed_db_ms`, `pin_d{0..7}_fn`, `uart_baud`, `ota_enabled`, …) | ✅ | ✅ | ✅ | ✅ | ✅ (if key still in newer schema) / lost (if renamed) | ❌ — reverts to schema defaults | ❌ |
| **NVS WiFi creds** (`wifi_cfg` namespace: known-good + candidate) | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ — falls back to compile-time `secrets.h` default, then SoftAP | ❌ |
| **OTA selector** (`otadata`, `running_app` flag) | ✅ | ✅ | ✅ (if app was marked valid) / rollback (if not) | ✅ flips to new slot | n/a | ✅ — separate partition | ❌ — bootloader picks ota_0 |
| **RTC slow memory** (`s_consecutive_crashes`, `s_pre_boot_fail`, `s_wifi_try_count`, magic guard) | ✅ — RTC powered separately from CPU | ⚠️  retained on brownout; lost on full power loss | ✅ — this is *why* the counters exist | ✅ | ✅ | ✅ | ❌ — fresh RTC at first power-on |
| **PSRAM photo retry queue** (heap-backed FIFO) | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| **In-RAM counters** (motion / reed / photo_seq / capture / burst) | ❌ — reset to 0 | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| **MQTT retained state** (`<id>/state/*`) | ✅ — broker-side | ✅ | ✅ | ✅ | ✅ | ✅ (broker doesn't know) | ✅ (broker doesn't know) — see [stale retained](#stale-retained-state) |
| **SD card** (`/photos/*`, `/audio/*`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ — separate physical media |
| **Coredump partition** | ✅ until next panic overwrites | ✅ | ✅ — *this* is the panic backtrace ship | ✅ | ✅ | ✅ | ❌ — erased |

Legend: ✅ survives, ❌ lost, ⚠️  conditional.

**Brownout vs full power loss for RTC slow memory:** ESP32-S3 RTC SRAM is on
the same supply rail as the LDO; a clean brownout drops VDD below CPU
threshold but keeps RTC alive for a few hundred ms. A long full disconnect
(battery pulled, USB unplugged) clears it. In practice the field unit's
INA226-monitored solar input rarely produces clean brownouts — see
`diag.c::diag_capture_boot` for the magic-guard check that distinguishes
"counter is real" from "RTC was reset to garbage".

## Counter semantics (motion / reed / photo / etc.)

PIR motion, reed event count, photo seq, capture count, audio burst count —
all reset to 0 every boot. They're published as retained MQTT with HA
`state_class: total_increasing`, which is **deliberately** preserved so the
HA recorder treats reboot reset as a counter rollover (the long-term
statistics math handles this correctly — daily/weekly sums stay accurate).

**Operator-visible effect:** HA dashboard cards that read the current
sensor value will jump from e.g. `847 → 1` after a reboot. **This is not
data loss** — the recorder kept all the events. It's a UI artifact of
choosing "instantaneous current value" as the card display.

We chose not to persist these counters into NVS because:

- NVS write cost amortized over the field unit's lifetime: each counter
  write costs ~10 ms + one flash sector erase cycle per ~32 writes. A
  counter that bumps once per minute would burn ~200k writes/year —
  inside the rated lifetime but uncomfortably close.
- The interesting metric (total events over time) is recovered correctly
  by HA's recorder from the `total_increasing` deltas.
- The "operator confusion after reboot" is solved by reading the
  recorder's long-term statistics card, which doesn't reset.

If a future use case needs monotonic counters (e.g. operator-facing UI
that can't tolerate the jump), the right design is a periodic NVS batch
write (1×/h max + on graceful shutdown / cmd/reboot), not write-per-event.

## NVS schema evolution

The `cb_cfg` namespace is schema-driven by `SCHEMA[]` in
[main/app_config.c](main/app_config.c). Rules when modifying the schema:

| Change | What happens to existing fielded boards |
| --- | --- |
| **Add new key** | Default applied on first boot; operator can override via MQTT. Cost: 1 NVS entry slot. |
| **Remove key** | Old entry stays in NVS as an orphan (no leak — capped slot count). Cleaned up by next full `nvs_flash_erase` (factory wipe) or by explicitly writing an `nvs_erase_key` in app_config_init. Currently we choose silent orphan. |
| **Rename key** | Old value abandoned, new key gets schema default on first boot. NO migration. Document the rename in the config schema / changelog. If the rename is forced by the **15-char NVS limit**, that earlier name was silently failing every nvs_set anyway (exactly how two keys were lost before the limit check existed). |
| **Change default** | Affects only factory-fresh boards + operators who never explicitly set the key. Existing operator overrides take precedence. |
| **Shrink range (min/max)** | `load_from_nvs` now clamps out-of-range NVS to schema default + logs `ESP_LOGW` — operator's setting silently reverts but with a serial breadcrumb. HA discovery's `min/max` will disagree with the operator-set NVS value until they cycle through HA UI. |
| **Change type** (T_INT → T_FLOAT etc.) | `nvs_get_*` returns `ESP_ERR_NVS_TYPE_MISMATCH`; `load_from_nvs` logs `ESP_LOGW` and falls to schema default. Operator's setting is lost. |

**Hard constraint: NVS key names must be ≤ 15 ASCII chars.** The IDF limit
is `NVS_KEY_NAME_MAX_SIZE - 1 = 15`. Any longer and `nvs_set_*` returns
`ESP_ERR_NVS_KEY_TOO_LONG`, the setter cache stays at the prior value,
and the operator's MQTT toggle silently snaps back to default. `app_config_init`
logs `ESP_LOGE` for any over-limit key at boot — caught in dev, but
relying on serial-log monitoring is weaker than catching at compile time.

If you add a new key, also add an HIL coverage line in
[tests/hil/test_persistence.py](tests/hil/test_persistence.py) — that test
flashes a custom value through cfg(), erases the broker's retained copy,
reboots, and asserts the device's post-boot republish carries the custom
value (only possible if NVS persist actually worked).

## OTA + rollback

[partitions.csv](partitions.csv) reserves **`ota_0` and `ota_1`** as the two
app slots (3.5 MB each) plus a `coredump` scratch partition. **There is no
`factory` slot.** This matters for first-OTA recovery: if a freshly-flashed
board (only `ota_0` populated, `ota_1` empty) is taken through an OTA that
hangs before `esp_ota_mark_app_valid_cancel_rollback()` runs in [main/diag.c::diag_boot_succeeded](main/diag.c), the
rollback path has nowhere to fall back to and **the board bricks** — needs
serial reflash to recover. The first successful OTA populates `ota_1` and
unlocks the safety net for every subsequent upgrade.

This is acceptable risk for hobby because every board does a bench-validated
OTA before going to field. **Don't deploy a board straight from factory
flash to field without running at least one OTA cycle locally first.**

### Anti-rollback flavors

Two distinct mechanisms with different threat models:

1. **App-validated rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`,
   on by default in `sdkconfig.defaults`). The bootloader requires
   the new app to call `esp_ota_mark_app_valid_cancel_rollback()`
   within a grace period or it auto-reverts to the previous slot.
   This catches "OTA installed but the app crashes before it can
   acknowledge". Implemented in [diag.c:diag_boot_succeeded()](main/diag.c) — gated on selftest
   completion, not a wall-clock timer.

2. **Anti-rollback eFuse** (`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y`,
   present in `sdkconfig.defaults.production` only — and that
   production overlay is **designed but not yet deployed**; no board
   has burned these fuses, the fielded unit runs the `secureboot_test` profile). Burns a
   one-way fuse on first boot of each app's `secure_version`; the
   bootloader refuses to boot any app with a *lower* version. This
   catches "attacker pushes a known-vulnerable old build". **eFuse
   burns are permanent** — `secure_version` must be monotonically
   non-decreasing forever. Bench builds with the default
   `sdkconfig.defaults` don't burn fuses; production builds must
   keep their `secure_version` strictly increasing or fielded
   boards refuse to upgrade.

A second-layer **app-level downgrade gate** lives in
[ota.c::parse_build_time](main/ota.c) which refuses to install a build with an older
`__DATE__/__TIME__` than the running one. This is a *softer* check
(can be bypassed by a dirty bench build with a fudged clock) but
fast enough that it catches the common case of "stale OTA artifact
on the server".

## Stale retained state

MQTT retained state lives at the broker, not the device. Two implications:

1. **Factory NVS erase doesn't clear retained.** The device boots with
   schema defaults, but `<id>/state/cfg/*` on the broker still holds the
   pre-erase values. The device republishes its (default) values on
   first MQTT connect, so the discrepancy resolves on its own — but for
   the gap between erase and reconnect, HA dashboards show stale data.
2. **Renamed/removed keys leave zombie retained topics.** The current
   firmware actively cleans the two known cases (`capture_led_enabled`
   → `cap_led_en`, `reed_debounce_ms` → `reed_db_ms`) by publishing
   empty retained payloads to both the old discovery topic and the
   old state topic on every boot — see `DEPRECATED_DISC[]` in
   `app_config_publish_discovery()`. Add an entry there for any
   future rename.

A failed OTA leaves a retained `state/ota=error`; the next successful
check publishes `state/ota=up-to-date` (also retained via `mqtt_pub_retained`)
which overwrites it — cleanup is automatic.

## Photo sequencing + dedup

`photo_seq` resets to 1 every boot. HA archiver dedup
(the author's HA archiver package) uses `input_text` +
`restore_state` to remember the last seen seq and skip retained
replays. After a device reboot the operator sees `last_seq` go
1, 2, 3, … — including down from the pre-reboot value, which is
counter-intuitive but **correct** for the dedup logic (seq=1 ≠
cached last_seq=N → archive; cache becomes 1 → subsequent seq=2 ≠
1 → archive).

If a future change persists `photo_seq` to NVS, it MUST be batched
(1×/h max, not per-photo) or the flash wear will be unacceptable
within a few years.

## Reconfigurable WiFi + graded reset

WiFi credentials are **reconfigurable at runtime**, not hard-compiled.
`secrets.h` (`WIFI_SSID`/`WIFI_PASSWORD`) is the compile-time **default** —
the recoverable floor *when it holds real creds*. Overrides live in the
`wifi_cfg` NVS namespace as a **known-good** set plus an optional
**candidate** under trial. `wifi_store_get_effective()` (wifi_store.c)
resolves which to use each boot: candidate → known-good → compile default.

**Unprovisioned first-boot → AP, not a doomed STA attempt.** If the compile
default is the `secrets.h.example` placeholder (`your-…`) or blank — a
clean-clone build never given real creds — there is no usable STA target, so
`wifi_store_have_sta_target()` returns false and the board boots **directly
into the AP provisioning portal** (`cb-<suffix>`, `/wifi`) rather
than failing to associate and waiting out the 600 s SoftAP trigger. This is
*not* sticky AP-only: submitting creds on `/wifi` enters the candidate ladder
and the next boot is STA. A board pre-provisioned with a **real** `WIFI_SSID`
(fleet flashing) boots STA-first, unchanged. (To test the AP-first path on the
bench, build with blank/placeholder WiFi creds — a real bench SSID STA-firsts.)

**WiFi creds are deliberately NOT in the `app_config` schema** — every
schema key is auto-published to HA discovery + echoed retained to
`<id>/state/cfg/<key>`, which would leak the WPA2 password to the broker.
The password is never published on any topic (the only WiFi topic,
`<id>/state/wifi`, carries status + the SSID at most).

### Verify-before-commit (anti-brick)

A new set (via `cmd/wifi` or the SoftAP form) is staged as a **candidate**
and the board reboots to try it. It is **promoted** to known-good only
after the control plane is proven — IP **and** MQTT connected, held stable
for `WIFI_CAND_VERIFY_S` (default 45 s; the same "can still receive a
corrective OTA" bar the OTA mark-valid uses). If it doesn't reach MQTT
within `WIFI_CAND_VERIFY_TIMEOUT_S` (default 240 s) it **auto-reverts** to
known-good and reboots. A candidate that associates but then panics is
backstopped by an RTC attempt counter (`WIFI_CAND_MAX_TRIES`, default 3),
dropped at boot before WiFi init. Three-tier fall-through: candidate →
known-good → compile default. **A wrong SSID/password can therefore never
strand the OTA-only field board** — it recovers with no physical access.

### SoftAP recovery fallback

If the station can't get an IP for `WIFI_SOFTAP_TRIGGER_S` (default 600 s)
with no candidate pending, the board raises a time-limited WPA2 SoftAP
`cb-<suffix>` (fixed default passphrase `chytrabudka` unless the
operator set a per-box AP password; the `/config` page warns whenever the
default is in use) serving a `/wifi` form at `http://172.31.4.1/wifi`.
Submitted creds enter the same candidate flow, so a typo just relaunches the
AP after the verify window. The AP defeats modem-sleep, so it is bounded by
`WIFI_SOFTAP_MAX_S` (default 600 s) → reboot to retry the station. If the
home AP returns while the fallback is up, it's torn down automatically.
(Security note: the default passphrase is public on purpose — adequate for a
time-limited, max-1-client recovery AP under this project's field threat
model; set a per-box AP password via `/config` to harden it.)

### AP credentials, full AP-only mode, and the /config page

The SoftAP/AP credentials are operator-settable (NVS `wifi_cfg`: `ap_ssid` /
`ap_pass`; empty = the default `cb-<suffix>` / `chytrabudka`)
via `cmd/wifi {"ap_ssid","ap_pass"}` or the web page. A sticky **full AP-only
mode** (`wifi_cfg` `ap_only`) makes the box boot as an access point only — no
station, so **no MQTT / OTA / remote recovery**; it is exited via the `/config`
toggle or a BOOT-button factory reset (which clears the whole `wifi_cfg`
namespace, `ap_only` included). Enable via `cmd/wifi {"ap_only":true}` or
`/config`. Allowed on any board (operator's risk acceptance).

The local **`/config`** web page renders the entire `app_config` schema
(editable) plus action buttons and WiFi/AP config from the same schema HA
discovery uses — so AP-only mode is actually usable with just a browser. Gated
like `/wifi`: open while the AP is up (WPA2 is the gate), else authenticated
HTTPS only.

NOTE (PMF / 802.11w): IDF v6 forces the AP PMF-capable (`pmf_cfg.capable` is
deprecated); we only set `required=false`. Compliant clients (phones) complete
the SA-Query and associate fine. The bench RTL8188EUS/aircrack USB dongle has a
broken PMF impl (advertises PMF, never answers SA-Query → reason-209 disassoc
before DHCP) — a dongle bug, not ours; it blocked over-the-air bench validation
of the AP-only + `/config` paths.

### Reset channels

A wedged config (`mode_override=3` Safe → no audio/camera) or a lost
network is now recoverable via graded resets:

1. **WiFi-only** → `cmd/wifi {"reset":true}`: erase `wifi_cfg`, reboot to
   the compile-time default (then SoftAP if that also fails).
2. **Config-only** → `cmd/cfg_reset`: `app_config_reset_defaults()` wipes
   the `cb_cfg` namespace back to schema defaults + reboots. WiFi + TLS
   untouched. (Also the bench-only `POST /debug/cfg/mode_override` under
   `CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS`, and `cmd/cfg/mode_override 0`.)
3. **Full factory** → `cmd/factory_reset`: config defaults + `tls_store`
   erase (forces re-enroll) + `wifi_cfg` erase, in that order (WiFi last,
   so a mid-sequence power loss still leaves WiFi to receive another
   command), then reboot.
4. **GPIO long-press** → hold the XIAO **BOOT button (GPIO0)** ≥10 s at
   runtime = full factory reset (`CONFIG_CHYTRA_BUDKA_BOOT_BUTTON_RESET`,
   default y). GPIO0 is read only after boot (strap already latched, so no
   download-mode collision) and doesn't conflict with the D0–D7 pin map.
   The onboard LED blinks while held (past ~3 s) and goes solid on commit.
   Bench/serviceable only — the BOOT button is on the bare PCB and is
   typically not exposed through a sealed field enclosure; the SoftAP
   fallback is what recovers a sealed unit without access.

`idf.py erase-flash` on the bench still wipes everything; WiFi then falls
back to the `secrets.h` default on next boot (no SoftAP needed if that
network is in range).
