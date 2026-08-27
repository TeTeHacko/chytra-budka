# Contributing to Chytrá Budka

Thanks for your interest! This is an open hardware + firmware project for a
solar-powered wildlife camera/mic box built around a Seeed XIAO ESP32-S3 Sense.
Contributions — bug reports, fixes, hardware notes, docs — are welcome.

## Licensing of contributions

By contributing you agree your work is licensed under the project's terms:

- **Code / firmware** → GPL-3.0-or-later ([`LICENSE`](LICENSE))
- **Hardware design + documentation** → CC-BY-SA-4.0 ([`LICENSE-docs`](LICENSE-docs))

Please add a `Signed-off-by:` line to your commits (`git commit -s`) to certify
the [Developer Certificate of Origin](https://developercertificate.org/).

## Repo orientation

- `firmware/` — ESP-IDF v6.0.1 firmware (the only supported toolchain;
  mbedTLS 4.0.0). See [`firmware/README.md`](firmware/README.md).
- Top-level `*.md` — hardware design ([`SCHEMATIC.md`](SCHEMATIC.md)), wiring
  ([`WIRING.md`](WIRING.md)), BOM ([`SHOPPING.md`](SHOPPING.md)), and first-boot
  bring-up ([`RUNNING.md`](RUNNING.md)).
- [`server/`](server/README.md) — self-hostable server stack (mTLS MQTT broker,
  enrollment CA, OTA store, management UI; docker compose). It builds the audio
  relay and the metrics exporter from [`relay/`](relay/README.md) and
  [`metrics-bridge/`](metrics-bridge/README.md). The author's Home Assistant
  packages and site-deployment glue aren't published.

## Building the firmware

```bash
. $IDF_PATH/export.sh                 # ESP-IDF v6.0.1
cd firmware
cp main/secrets.h.example main/secrets.h   # then fill in your WiFi/MQTT/relay values
$EDITOR main/config.h                       # set RELAY_HOST / MQTT_HOST / OTA_URL for your network
tools/build.sh bench                        # build the bench profile
tools/build.sh bench flash monitor          # append any idf.py action
```

> **Build through `tools/build.sh <profile>`.** The bench/field/signing posture
> lives in per-profile `sdkconfig.defaults.*` overlays that IDF applies only via
> an explicit `-DSDKCONFIG_DEFAULTS=…` chain, and each profile gets its own build
> dir. A plain `idf.py set-target esp32s3 && idf.py build` is *not* broken — IDF
> auto-applies `sdkconfig.defaults` plus its `.esp32s3` sibling, so you do get a
> valid 8 MB image — but it is **profile-less**: no `/debug/*` endpoints, no
> signing posture, and it writes a `sdkconfig` that then collides with the
> wrapper's. Skipping `set-target esp32s3` is the case that genuinely breaks.

> Flashing: `tools/build.sh bench flash` is fine when you have one board. Once
> you have more than one — especially any board that must stay OTA-only —
> flash through [`firmware/tools/flash_safe.sh`](firmware/tools/flash_safe.sh), which
> refuses raw `/dev/ttyACMn` and checks a MAC allowlist. That allowlist is
> operator-local and gitignored; create it with
> `cp firmware/tools/devices.txt.example firmware/tools/devices.txt`.

> After editing any `sdkconfig.defaults*`, run `idf.py reconfigure` — those
> files only seed the initial `sdkconfig`.

## Tests — please run before opening a PR

- **Native unit tests** (no hardware, host C/C++ toolchain):
  ```bash
  cd firmware/tests/native && make test
  ```
- **Hardware-in-the-loop** (optional, needs a powered-on bench board on WiFi):
  ```bash
  cd firmware/tests/hil && python -m venv .venv && . .venv/bin/activate
  pip install -r requirements.txt
  ./run.sh -m "not manual"          # NOT bare pytest — see below
  ```
  **Use `./run.sh`, not bare `pytest`.** Every HIL session factory-resets the
  bench, so the run has to re-provision it onto your WiFi afterwards. `run.sh`
  sources those credentials from NetworkManager; bare `pytest` relies on
  `CB_PROVISION_SSID` + `CB_PROVISION_PSK` already being exported, which a
  non-interactive shell will not have. (The suite now refuses to start in that
  case rather than stranding the board — but `run.sh` is still the entry point.)

  The bench is addressed via a MAC-pinned `/dev/esp32-<mac>` udev symlink (see
  [`host/README.md`](host/README.md)); add your board's MAC to the allowlist in
  `firmware/tests/hil/conftest.py`. Note this is a *separate* allowlist from
  `firmware/tools/devices.txt`, which gates USB flashing.

## Coding conventions

- Match the surrounding style. Firmware is C (C23) for `main/`, C++ for the
  platform-agnostic `components/cb_core/` library; keep that library free of
  ESP-IDF dependencies so the native tests keep building on a host.
- Robustness over breadth: a failed hardware probe with a clear operator log
  beats a quietly-broken feature. Field reliability is the priority.
- Keep comments accurate — stale comments that describe removed code are worse
  than none.

## Reporting bugs / requesting features

Open a GitHub issue. For hardware issues, include your board revision, the boot
log (serial `idf.py monitor`), and the `/selftest` JSON if reachable. For
**security** issues, see [`SECURITY.md`](SECURITY.md) instead of a public issue.
