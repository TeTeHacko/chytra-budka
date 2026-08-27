# HIL — hardware-in-the-loop tests

Host-side tests against a real **bench** board (`ex01`), driven as one
**end-to-end lifecycle** that mirrors a deploy: factory-reset → AP/onboarding
suite → provision onto the station LAN → connected suite. This is the gate
`tools/ota_upload.sh` runs before every field OTA — a connected-path regression
is caught here, not in the field (a boot-check alone once let a build through
that ran fine on the bench and `task_wdt`'d in the field's STA-only path;
hence the full lifecycle).

## The lifecycle (enforced order)

```
reset_board   (autouse)    factory-reset over MQTT → board boots unprovisioned → AP
  ↓
@ap_mode      tests         complete AP/onboarding suite (joins the SoftAP)
  ↓
provisioned_sta (fixture)   POST station creds on /wifi → reboot → STA → MQTT
                            online → wait TLS re-enroll (HTTPS up) → discover IP
  ↓
@sta_mode     tests         complete connected suite (everything not @ap_mode)
```

`pytest_collection_modifyitems` (in `conftest.py`) enforces the order:
`@ap_mode` → the `test_provision_ap_to_sta` transition → everything else
(unmarked tests default to the STA phase).

## Quick start

```bash
cd firmware/tests/hil
python -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt

# Bench must be up; default targets /dev/esp32-aabbccddee01.
./run.sh                        # the deploy gate's selection (-m "not manual")
./run.sh test_persistence.py    # one module
./run.sh -k reconnect -x        # anything else is passed straight to pytest
```

**Use `run.sh`, not bare `pytest`.** Every session factory-resets the bench, so
the run has to be able to re-provision it onto the station LAN afterwards.
`run.sh` sources those credentials from NetworkManager (connection **Wi-Fi
IoT** by default; the PSK is only ever assigned to a variable, never printed)
and fails loudly if it can't get them. Bare `pytest` relies on
`CB_PROVISION_SSID` + `CB_PROVISION_PSK` already being exported — if they
aren't (typical in scripts, CI steps, or non-interactive shells), the suite
skips provisioning, leaves the board sitting in its SoftAP, and every later
test fails with `no route to host`: a missing variable that reads exactly like
dead hardware.

Never print these values — the transcript is persisted and a leak forces a PSK
rotation. Don't `echo $CB_PROVISION_PSK` to "check it": even `${VAR:-x}` expands
and leaks it. Use `[ -n "$VAR" ]`.

Without the creds the provision phase + STA suite skip; the AP phase still runs.
Needs `nmcli` (AP join / wifi restore) and `nmap` (post-provision IP discovery)
on the host — the bench provisions onto the **IoT-Network** network. The host's
Wi-Fi NIC may be named other than `wlan0` (e.g. `wls17`); `_join_ap` lets nmcli
pick the Wi-Fi device, and an Ethernet link keeps the host online while its
Wi-Fi briefly joins the bench AP. Set `CB_HIL_NO_RESET=1` for an ad-hoc run that
shouldn't wipe the board.

## As the deploy gate

`tools/ota_upload.sh` builds + flashes the **bench profile of the same commit**
(`erase-flash`), then runs `./run.sh -m "not manual"` and **refuses to upload to
the field unless it passes**. `--no-hil` overrides (emergencies only). The gate
goes through `run.sh` rather than resolving credentials its own way, so the gate
and a hand-run suite provision onto the same network.

Budget **~26 min** for the gate. It is dominated by things that are slow for a
reason: the bad-WiFi-candidate auto-revert (~270 s — the anti-brick acceptance
test), the config-reset reboot, and the TWDT hang (must outlast 30 s + the
slowest subscribed task, ~52 s). Two families of test are deliberately opt-in
because they characterise hardware rather than guard a release — see the
environment table (`CB_SWEEP`, `CB_OTA_STAGED`, `CB_HIL_ALLOW_PRUNE`,
`CB_AUDIO_WIRED`).

> **Bench HW ≠ field HW.** The bench lacks sensors the field has
> (`battery`/`ina226`/`reed`/`uart_servo` read `false` in selftest), so a
> field-hardware-specific fault is **not** reproducible here — that needs the
> field coredump. This gate catches everything reproducible on the bench, which
> is the large majority of regressions. A weak bench→AP RSSI also makes HTTP
> reads slow/flaky — keep the bench in range of the station AP.

Run only the manual-interaction tests (PIR wave, audio clap):
```bash
pytest -m manual
```

Skip manual tests (the default `addopts` already does this implicitly
because `-m manual` is opt-in only):
```bash
pytest -m "not manual"
```

## Environment overrides

| Variable | Default | Purpose |
|---|---|---|
| `CB_BENCH_PORT` | `/dev/esp32-aabbccddee01` | udev symlink for the bench |
| `CB_BENCH_MAC` | (derived from port) | 12-hex MAC override; lets the WiFi/MQTT/HTTP tests run when the bench isn't plugged in for USB (e.g. unit deployed but reachable). Allowlist still applied. |
| `CB_BENCH_IP` | discovered by MAC (ARP sweep) post-provision | bench HTTP host; overrides discovery |
| `CB_PROVISION_SSID` / `CB_PROVISION_PSK` | `run.sh`: from NetworkManager (`IoT-Network`) | station creds the box provisions onto (AP→STA step); phase skips if unset |
| `CB_HIL_NO_RESET` | unset | `=1` skips phase 0 (don't factory-reset the board) |
| `CB_MQTT_HOST` | `cb.example.com` | broker host — the fleet broker |
| `CB_MQTT_PORT` | `8883` | broker port (mTLS) |
| `CB_MQTT_CERT` / `CB_MQTT_KEY` / `CB_MQTT_CAFILE` | `server/secrets/hil-runner.{pem,key}` + `ca_chain.pem` | client certificate for the mTLS listener. Issue with `server/scripts/issue-client-cert.sh hil-runner`. Its ACL is scoped to the **bench topic only**, so the suite cannot touch a deployed board. |
| `CB_MQTT_USER` / `CB_MQTT_PASS` | — | username/password instead, for pointing the suite at a plain local broker. Only consulted when no client certificate is present. |
| `CB_OTA_STAGED` | unset | `=1` enables `test_ota.py` (skipped by default to prevent the bench self-downgrading to a stale server image mid-run) |
| `CB_HIL_ALLOW_PRUNE` | unset | `=1` enables the destructive SD-autoprune test in `test_sd_gallery.py` (deletes oldest day buckets on the bench) |
| `CB_SWEEP` | unset | `=1` enables `test_jpeg_sweep.py`. Off by default: it is a ~19 min camera **characterisation** (framesize × quality × frames), not a release guard, and it rewrites `cam_*`/`mjpg_*` on the board. |
| `CB_AUDIO_WIRED` | unset | declares which audio-out variant is physically wired, enabling the matching `test_audio.py` cases (they self-skip otherwise) |

`bench_port` fixture hard-asserts:
1. the path is a symlink (not raw `/dev/ttyACMn`),
2. it matches `/dev/esp32-<12-hex-mac>`,
3. the MAC is on the in-source bench allowlist
   (`BENCH_ALLOWLIST` in `conftest.py`).

Field / OTA-only boards are intentionally NOT on the allowlist so a
mis-set env var can't run a test that reboots a deployed unit.

## Test modules

| File | What it covers |
|---|---|
| `test_boot_smoke.py` | `/selftest` JSON shape, `/` homepage HTML, retained MQTT topics (availability, fw_version), required-modules check, uptime advance |
| `test_http_endpoints.py` | smoke test that every documented HTTP route returns a non-5xx status |
| `test_mode_fsm.py` | force each awake power tier via `cfg("power_profile", <name>)` and verify `state/profile` transitions (hibernate excluded — it deep-sleeps the bench) |
| `test_mqtt_reconnect.py` | drop WiFi via `/debug/wifi_disconnect`, verify LWT + clean reconnect |
| `test_crash_loop.py` | `/debug/hang` → TWDT panic → next-boot `reset_reason` carries the expected sentinel |
| `test_exif.py` | full APP1 EXIF schema validation over IFD0 + ExifSubIFD tags + UserComment JSON keys/types |
| `test_dual_core.py` | task-pinning policy via `/debug/cores`: `audio`/`cam_wrk` on CPU1, `main` on CPU0; survives `stream.mjpg`+`cmd/photo` stress |
| `test_persistence.py` | NVS config round-trips across reboot; load-clamp + key-length invariants |
| `test_tls_enrollment.py` | `@state_change` — per-device CSR→sign→cert round-trip, then HTTPS active on :443 after reboot |
| `test_timelapse.py` | `@manual` — sets `tlapse_min=1`, waits up to 90 s for an `event/photo` with `trigger=timelapse`, restores prior value |
| `test_jpeg_sweep.py` | `@manual` `@sweep`, needs `CB_SWEEP=1` — characterise OV3660 across (framesize, quality) for still + MJPEG profiles; emits CSV + static HTML viewer. Bench only: the runner's certificate is scoped to the bench, so a matrix entry for a deployed board fails at the broker on purpose. |
| `test_ap_mode.py` | `@ap_mode` — SoftAP onboarding portal at `172.31.4.1` |
| `test_wifi_provision.py` / `test_provision.py` | `@ap_mode` — POST `/wifi` creds → STA promote → MQTT online (the AP→STA lifecycle gate) |
| `test_wifi_scan.py` | `/wifiscan` AP list shape |
| `test_ota.py` | `cmd/ota` against the running-app image → pending-verify → mark-valid (re-enables `ota_enabled` itself) |
| `test_web_admin_auth.py` | HTTP basic-auth gate: 401 unauth over HTTPS, 200 with creds |
| `test_sd_gallery.py` | date-tree gallery + `/photos.json` shape + `/photo` ETag/304/Range + SD autoprune config/telemetry (destructive prune test gated behind `CB_HIL_ALLOW_PRUNE`) |

Lifecycle ordering (reset → `@ap_mode` → provision → `@sta_mode`/unmarked) is
enforced by `pytest_collection_modifyitems` in `conftest.py`. `@manual` tests are
excluded from the deploy gate (`pytest -m "not manual"`).
