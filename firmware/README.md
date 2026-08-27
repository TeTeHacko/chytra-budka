# Chytrá Budka — firmware (ESP-IDF v6.0.1, mbedTLS 4.0.0)

Native ESP-IDF firmware for the XIAO ESP32-S3 Sense (N8R8) bird-box
listening post — onboard PDM mic + OV3660 camera + MQTT to Home
Assistant + per-device HTTPS self-enrollment + OTA from `ota.example.com`
(**signed OTA** — verify-on-update, RSA-3072, YubiKey-vault-signed — is
deployed fleet-wide; the field unit `ex02` additionally runs **hardware
Secure Boot v2** in a recoverable config. Only Flash Encryption + full eFuse
lockdown remain — see [Signed OTA (soft)](#signed-ota-soft--deployed-on-bench-verified)
and [Production hardening](#production-hardening-stage-e)). Supported sensors: MAX17048
fuel gauge, SHT41 T/RH (×2: internal + external bit-bang bus), INA226
solar V/I, AM312 PIR, reed magnetic door/lid contact. IR illuminator
(940 nm + AO3400 MOSFET) gated by sensor AGC. Visible capture
indicator LED for "the camera just fired" operator feedback.

Captures carry their context as **EXIF metadata** (board, time, trigger,
env) + the MQTT `event/photo` JSON — there is **no burned-in visual
overlay**: the OV3660 emits JPEG on-chip (no OSD) and the S3 has no HW JPEG
codec, so burning pixels would need a ~3.6 s software re-encode per UXGA
still. Visual date/board overlay is done downstream (the author's timelapse
tooling + HA) — cheaper and reversible, unlike burned-in pixels.

Migrated from Arduino-ESP32 / PlatformIO in May 2026 (branch
`idf-migration`).

> **Mic note.** The only mic is the Sense expansion's onboard PDM — an
> external I²S mic (INMP441) was dropped before rev3.1 because it would
> collide with the SDIO microSD pins. The PDM runs at 16 kHz (stable per
> Seeed wiki; 48 kHz had partial DMA fills).

## Layout

```
firmware/
  CMakeLists.txt              # top-level project
  partitions.csv              # 8 MB, dual-OTA + nvs_keys + spiffs
  sdkconfig.defaults          # base defaults (logging, lwIP, mbedTLS, OTA)
  sdkconfig.defaults.esp32s3  # XIAO N8R8 specific (8 MB QIO, OCT PSRAM, USB-JTAG)
  sdkconfig.defaults.linux    # host build target
  sdkconfig.defaults.signed_soft # soft signed-OTA overlay (verify-on-update, NO eFuse) — WORKING
  sdkconfig.defaults.production  # hardware Secure Boot v2 + FE + NVS Enc overlay (eFuse burn) — design-only
  components/cb_core/         # platform-agnostic library (VAD, ModeFsm, ChunkedPoster)
  main/                       # app entry + drivers (wifi, mqtt, audio, battery, ota)
  tools/
    fetch_le_roots.sh         # pulls ISRG Root X1 + X2 PEM bundle
    setup_keys.sh             # (legacy) on-disk key provisioning for the eFuse-SB path
    setup_signing_vault.sh    # one-shot YubiKey-gated age vault for the signing key
    sign_with_yubikey.sh      # sign an app image: age-unlock (YubiKey touch) → espsecure RSA-PSS
  tests/native/               # host-side unit tests (no IDF required)
```

## Quick start (development build)

```bash
# One-time per shell — source ESP-IDF v6.0.1 (the only supported toolchain;
# mbedTLS 4.0.0). Install guide:
#   https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/get-started/index.html
. ~/esp/esp-idf/export.sh      # wherever your v6.0.1 checkout lives

# Place real WiFi/MQTT credentials into firmware/main/secrets.h
cp firmware/main/secrets.h.example firmware/main/secrets.h
$EDITOR firmware/main/secrets.h

# LE root bundle — REQUIRED by the build: sdkconfig builds the mbedTLS trust
# bundle from tools/le_isrg_roots.pem alone, so outbound TLS trusts only the
# Let's Encrypt roots (swap in your own CA roots here if your OTA/error
# endpoints don't use LE). Gitignored; safe to re-run.
firmware/tools/fetch_le_roots.sh

# Running your own server stack? Also replace the embedded enrollment trust
# anchor with YOUR sub-CA certificate — the device validates its issued cert
# and the MQTTS broker against it (the shipped file is a placeholder):
#   cp server/secrets/sub_ca_budka.pem firmware/main/budka_sub_ca.pem
# (server/scripts/dev-pki.sh generates the root+sub pair.)

# Build via the profile wrapper — it applies the per-profile sdkconfig overlay
# chain and gives each profile its own build dir. (A plain `idf.py set-target
# esp32s3 && idf.py build` is not broken: IDF auto-applies sdkconfig.defaults +
# sdkconfig.defaults.esp32s3, so you get a valid 8 MB image — just one with no
# profile flags. Skipping set-target IS broken: the .esp32s3 overlay never
# applies.)
firmware/tools/build.sh bench
# Flash + monitor a bench board (append any idf.py action; field boards are
# OTA-only — flash those through firmware/tools/flash_safe.sh)
firmware/tools/build.sh bench flash monitor
```

> **`tools/build.sh <profile>` is the canonical build entry.** Profiles:
> `bench`, `field` (signed OTA → fleet), `signed`, `production`,
> `production-yubikey`, `secureboot-test` — each owns its overlay chain and
> build dir. It auto-sources ESP-IDF v6.0.1 and only re-runs `set-target` when
> the build dir is fresh or its profile changed. `tools/build.sh --help` lists
> them. The raw `idf.py …` chains below are what the wrapper runs under the hood.

> ⚠️ **sdkconfig drift gotcha.** `sdkconfig.defaults*` only seed the
> initial `sdkconfig` and are ignored on subsequent builds — IDF reads
> the cached `sdkconfig` instead. After editing any `sdkconfig.defaults*`
> file, run `idf.py reconfigure` (or `idf.py fullclean && idf.py build`)
> so the new values actually land in the binary. A silent symptom: bumping
> `CONFIG_ESP_MAIN_TASK_STACK_SIZE` in defaults and still seeing a stack
> overflow because the cached sdkconfig kept the old value.

## Native tests (cb_core)

```bash
cd firmware/tests/native
make           # builds + runs all 9 unit suites
                #   test_vad, test_vad_wrap, test_mode_fsm,
                #   test_chunked_post, test_chunked_errors,
                #   test_flac_encoder, test_sd_layout, test_csr, test_san_fp
                # plus the stream_to_relay integration runner
```

No IDF toolchain required; uses the system C++ compiler. The
`stream_to_relay` runner pushes a PCM file into a real relay over TCP
at realtime pace using the exact same `Vad` + `ChunkedPoster` classes
the ESP32 runs — see `relay/test-e2e.sh` for a one-shot host loop that
exercises both sides end-to-end.

## HIL (hardware-in-the-loop) tests

```bash
cd firmware/tests/hil
python -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
./run.sh                   # against a powered-on bench board
./run.sh -m "not manual"   # skip mark-manual interaction tests (default anyway)
```

> **`run.sh`, not bare `pytest`.** Every session factory-resets the bench (the
> autouse `reset_board` fixture), so the run must be able to re-provision it
> onto the station LAN afterwards. `run.sh` sources those credentials from
> NetworkManager; bare `pytest` needs `CB_PROVISION_SSID` + `CB_PROVISION_PSK`
> already exported, which a script/CI/agent shell will not have. The suite
> aborts with a message instead of wiping a board it can't restore — set
> `CB_HIL_NO_RESET=1` for ad-hoc runs against a board you don't want reset.

Phase A baseline runs against a powered-on bench board (no flash, no
serial — pure HTTP + MQTT). The bench MAC is asserted from the
`/dev/esp32-<mac>` udev symlink so a stray `CB_BENCH_PORT` env can't
target a field unit. Covers boot smoke, mode FSM, HTTP endpoint
surface, MQTT LWT + reconnect after `/debug/wifi_disconnect`, TWDT
crash recovery via `/debug/hang`, EXIF metadata invariants
(`test_exif.py`), TLS enrollment round-trip (`test_tls_enrollment.py`),
NVS persistence (`test_persistence.py`), timelapse cadence
(`test_timelapse.py`, marked `manual` because the smallest interval is
60 s), and the dual-core task pinning policy
(`test_dual_core.py` — polls `/debug/cores`, asserts `audio`/`cam_wrk`
land on CPU1 and `main` on CPU0, stress-tests pinning survives
`stream.mjpg`+`cmd/photo`). See
[tests/hil/README.md](tests/hil/README.md) for the bench allowlist,
env overrides, and the planned (deferred) `test_ota_dryrun` /
`test_chunked_live` modules.

## Modes

| Mode       | SOC enter | SOC leave | Audio behaviour                       |
| ---------- | --------- | --------- | ------------------------------------- |
| Boot       | —         | —         | idle (no audio capture)               |
| Triggered  | default   | < 65 %    | open stream on VAD burst, closes 30 s |
| Continuous | ≥ 65 %    | < 50 %    | always streaming                      |
| Safe       | < 30 %    | ≥ 35 %    | idle, light sleep cycles              |

If no MAX17048 is detected (`battery_ready() == false`), the FSM forces
Triggered mode and never enters Safe.

## OTA

Periodic poller (`ota.c`) wakes every 6 h, fetches `OTA_URL` over HTTPS —
the TLS bundle is built from `tools/le_isrg_roots.pem` **only** (sdkconfig
custom certificate bundle), so outbound TLS is effectively **pinned to the
Let's Encrypt ISRG X1/X2 roots**; self-hosters whose endpoints don't use LE
certs should put their own CA roots into that PEM. (Alternative for a local
dev stack with a self-signed cert: the `bench` profile sets
`CHYTRA_BUDKA_TLS_INSECURE` and skips outbound verification entirely — never
on field builds.) Host authentication aside,
the RSA-3072 **image signature** (verify-on-update) is the real integrity
control. The poller compares `esp_app_desc.version` + `app_elf_sha256` against
the running image, and applies the update + restart on mismatch.

Three layers of OTA authenticity, in increasing risk:

1. **Soft signed-OTA (verify-on-update, no eFuse)** — deployed on the bench
   (`ex01`) and the whole fleet. See below.
2. **Hardware Secure Boot v2** (eFuse key digest, **recoverable** — no Flash
   Encryption, secure-download kept) — **burned on the field `ex02`**
   (2026-05-30); verifies the bootloader + app every boot. See "Production
   hardening (Stage E)" + the author's private signing-custody runbook.
3. **Flash Encryption + full eFuse lockdown** — *still design-only* (the
   irreversible part), see "Production hardening (Stage E)".

## Signed OTA (soft) — DEPLOYED on bench, verified

`sdkconfig.defaults.signed_soft` enables `SECURE_SIGNED_APPS_NO_SECURE_BOOT` +
`SECURE_SIGNED_ON_UPDATE` (RSA-3072 scheme). The bootloader is untouched and the
board still boots any image (no eFuse, fully reversible over USB), but the
**running app verifies the RSA-PSS signature of every OTA image** before
accepting it. The trust anchor is the public key embedded in the running app's
own signature block, so once a board runs an image signed by our key, every
future OTA must be signed by the **same** key.

Signing is done off the build host by a **YubiKey-gated `age` vault** — the
RSA-3072 private key lives age-encrypted, a YubiKey touch unlocks it into tmpfs
for one signing run, and `espsecure` does the PSS sign in software. The private
key never sits on disk. Custody, recovery and the on-card-HSM successor plan
live in the author's private signing-custody runbook.

```bash
. ~/esp/esp-idf/export.sh
# one-time: stand up the vault (your PINs/touch)
firmware/tools/setup_signing_vault.sh
# build (isolated sdkconfig so the dev config stays clean), sign, flash bench
firmware/tools/build.sh signed                        # → build-signed/
firmware/tools/sign_with_yubikey.sh build-signed      # YubiKey touch
firmware/tools/flash_safe.sh -p /dev/esp32-<mac> -B build-signed flash
```

**Verified on hardware (2026-05-29, bench `ex01`):** an OTA image signed with
a *wrong* RSA-3072 key was rejected (`esp_image: signature bad` →
`esp_ota_ops: New image failed verification` → board stayed on the running
image), while the same image signed with the vault key was accepted, installed
to `ota_1`, and booted. **Both boards now run the vault-signed image via OTA**
(fleet standardized on `PARTITION_TABLE_OFFSET=0x10000`); the field `ex02`
additionally has hardware Secure Boot burned (next section).

## Production hardening (Stage E)

> **Recoverable hardware Secure Boot v2 IS deployed** on the field `ex02`
> (`sdkconfig.defaults.secureboot_test`: RSA key digest in eFuse, **no** Flash
> Encryption, secure-download mode left on → still USB-recoverable with a *signed*
> image). 🚧 What stays **DESIGNED, NOT YET DEPLOYED**
> is the *full* `sdkconfig.defaults.production` lockdown below — Flash Encryption
> (release), ROM-DL disable, NVS encryption — the **irreversible** eFuse burns.
> No board has those; treat the rest of this section as a design spec.

> ⚠️ First flash with the production overlay burns eFuses **irreversibly**.
> Bricked devices cannot be recovered — and the field board is OTA-only, so it
> could never be re-flashed. Read this section twice before running
> `idf.py -B build-prod flash`.

### 1. Generate keys (one-time, per developer machine)

```bash
get_idf
firmware/tools/setup_keys.sh
```

Keys land in `~/.config/chytra-budka/keys/` (mode 700) and are symlinked at
`firmware/keys` (gitignored). Back them up offline — losing the Secure Boot
signing key means you can no longer push OTA to deployed units.

### 2. Build with the production overlay

```bash
firmware/tools/build.sh production       # → build-prod/
```

This produces a signed bootloader and signed application image.

### 3. First flash (burns eFuses)

```bash
firmware/tools/build.sh production flash monitor
```

On first boot the bootloader will:

- Burn the Secure Boot v2 public key digest into eFuse `BLOCK_KEY0`.
- Generate a per-device Flash Encryption key in eFuse `BLOCK_KEY1` and encrypt
  bootloader, partition table, NVS, NVS keys, OTA data, and both ota_X
  partitions in place.
- Disable UART/USB ROM download mode (`CONFIG_SECURE_DISABLE_ROM_DL_MODE=y`).
- Disable JTAG.

After this, only signed images can boot, and only OTA can update the device.

### 4. Subsequent OTA updates

Just rebuild with the same command, sign happens automatically, and the new
binary is uploaded to the OTA endpoint. The device's periodic poller picks it
up within 6 h (or restart-trigger via MQTT command).

## Production hardening with YubiKey (HSM signing)

Keeping the Secure Boot v2 private key on a YubiKey 5+ OpenPGP applet means
the key material never touches the build host's disk. The flow:

```bash
# Provision the YubiKey ONCE (RSA-3072 SIGN subkey on the OpenPGP applet)
gpg --card-edit
  admin
  generate          # answer RSA, 3072 bits; PIN/Admin PIN as you like
  quit

# Export the public key + register the YK with this firmware
firmware/tools/setup_keys.sh --yubikey

# Build unsigned binaries with the YubiKey overlay
firmware/tools/build.sh production-yubikey     # → build-prod/

# Sign bootloader + app on the YubiKey (touches PIN entry)
firmware/tools/sign_with_yubikey.sh build-prod

# Flash (first time burns eFuses)
firmware/tools/build.sh production-yubikey flash monitor
```

Requirements: `gpg`, `ykman`, `opensc` (for `opensc-pkcs11.so`), YubiKey 5+
with OpenPGP applet provisioned. The signing script uses RSA-PSS with SHA-256
and 32-byte salt, matching ESP Secure Boot v2 expectations.

> 💡 The YubiKey's OpenPGP applet supports RSA-3072 (and RSA-4096); the PIV
> applet on YubiKey 5 only goes up to RSA-2048, so we deliberately use the
> OpenPGP path. Touch policy and PIN policy can be tightened via `ykman openpgp`.

## Security posture & post-quantum roadmap

| Layer                  | Algorithm                               | Status                         |
| ---------------------- | --------------------------------------- | ------------------------------ |
| Secure Boot v2         | RSA-3072 PSS + SHA-256 (HW accelerated) | classical, 128-bit equiv.      |
| Flash Encryption       | XTS-AES-256 (HW accelerated)            | **PQ-safe** (Grover → 128-bit) |
| NVS encryption         | AES-XTS-256                             | **PQ-safe**                    |
| TLS 1.3 (OTA only)     | X25519 ECDHE + AES-GCM-256              | classical, 128-bit equiv.      |
| OTA image authenticity | Secure Boot v2 signature on payload     | classical                      |

**Post-quantum exposure (deployment lifetime ~5–10 yr):**

- _RSA-3072 forging by CRQC ~2035_: real concern for OTA signature integrity.
  Mitigation is already in the architecture: signed OTA itself is the
  migration vehicle. When Espressif ships ML-DSA Secure Boot (likely
  IDF v6.x/v7.0 in 2027–2028), the field upgrade path is:
  1. Cross-build a "bridge" firmware against the existing RSA-3072 key.
  2. Push it via OTA to deployed devices.
  3. Bridge firmware burns a new ML-DSA public-key digest into the unused
     eFuse SB key slot (we already revoke slots 1+2 today, leaving slot 0
     as the production key).
- _Harvest-now-decrypt-later on TLS_: irrelevant — the OTA payload is public
  signed firmware; relay traffic is bird audio (also non-confidential by
  design). No private user data leaves the device.
- _Flash readout on physical capture_: AES-256 in eFuse is PQ-safe.

**Current PQ landscape (2026):**

- We are on **mbedTLS 4.0.0 (IDF v6.0.1)**, which exposes the PSA Crypto API
  as the _plumbing_ for future PQ algorithms but ships no PQ algorithms by
  default. (The older `mbedTLS 3.6` in IDF v5.5 had no PQ either, and that
  toolchain is no longer used here.) Upstream Mbed TLS has an experimental
  ML-KEM driver; ML-DSA is in pull-request stage.
- Hybrid PQ TLS handshakes (e.g. X25519 + ML-KEM-768) ship in OpenSSL 3.5+,
  AWS-LC, BoringSSL — not yet in mbedTLS stable.
- ESP32-S3 has no hardware acceleration for lattice operations; pure-software
  ML-KEM-768 measures ~2–4 ms keygen on the LX7 @ 240 MHz, ~3 KB working
  RAM — feasible when supported, not blocked by hardware.

**Plan:** we're on IDF v6.0.1 / mbedTLS 4.0.0 (PSA Crypto). Re-evaluate the
Espressif PQ roadmap quarterly; when image signing / Secure Boot is actually
deployed (see the status banner above), it becomes the crypto-agility
migration vehicle for ML-DSA Secure Boot once Espressif ships it.

## Security model

> Items marked _(production overlay)_ — Flash Encryption, anti-rollback eFuse
> — are **not yet deployed** (see the status banner under
> [Production hardening](#production-hardening-stage-e)).
> **Hardware Secure Boot v2 IS deployed** on the field `ex02` (recoverable,
> no Flash Encryption). WiFi, MQTT, the relay Bearer auth, per-device HTTPS
> enrollment, signed OTA, and LE cert pinning on the OTA fetch are live today.

- **WiFi:** WPA2-PSK / WPA3-SAE (config in `secrets.h`).
- **MQTT:** authenticated (user/pass in `secrets.h`); plaintext on LAN, TLS to
  remote brokers (set `mqtts://` URI in `mqtt.c` if needed).
- **Audio relay (HTTP):** plain HTTP/1.1 chunked POST with Bearer auth
  to `relay/` on the LAN. TLS is intentionally terminated upstream
  (VPN tunnel or reverse proxy) rather than on the device, so the
  firmware does not carry per-deployment client certs. The OTA path
  uses real HTTPS with the LE chain pinning described above.
- **OTA (HTTPS):** same cert pinning, plus image signature verification when
  Secure Boot is enabled.
- **At rest:** Flash Encryption (XTS-AES-256) protects firmware, NVS, and OTA
  data from offline readout. NVS encryption keys live in the dedicated
  `nvs_keys` partition (also encrypted).
- **Anti-rollback:** monotonic `CONFIG_BOOTLOADER_APP_SECURE_VERSION` prevents
  downgrade attacks once bumped.

### Anti-rollback bump cadence

`CONFIG_BOOTLOADER_APP_SECURE_VERSION` in `sdkconfig.defaults.production`
starts at `0`. It is **NOT auto-incremented** — every release ships with
whatever value is in that file at build time. Bumping policy:

- **Bump by +1** on every release that contains a fix for a security
  issue — CVE in mbedTLS / esp-mqtt / esp_https_ota / esp-tls, OR a
  Chytrá-Budka security finding (e.g. one of the S* items from a
  review pass that an attacker on the LAN could exploit).
- **Do NOT bump** on routine feature or bug-fix releases — bumping
  unnecessarily means a deployed device that's already past the new
  baseline can never be rolled back to an older known-good build for
  emergency recovery, and the eFuse anti-rollback counter has a finite
  fuse budget (32 increments on ESP32-S3).
- After bumping, document the reason in the release notes / commit
  message so a future operator understands what justified consuming
  one of the 32 slots.
- Production builds with anti-rollback enabled refuse to flash an
  image whose secure_version is lower than the device's burned
  baseline — including via OTA. Field-rollback after a bump requires
  a brand-new board.

Track the current value:

```bash
grep CONFIG_BOOTLOADER_APP_SECURE_VERSION firmware/sdkconfig.defaults.production
```

Secrets that **must not** be committed: `main/secrets.h`, anything under
`firmware/keys/`, `tools/le_isrg_roots.pem`. All listed in `.gitignore`.
