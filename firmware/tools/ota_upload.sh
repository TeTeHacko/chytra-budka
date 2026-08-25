#!/usr/bin/env bash
# tools/ota_upload.sh — build, optionally sign, and upload firmware to OTA server.
#
# Usage:
#   ota_upload.sh [--sign] [--build-dir DIR] [--dry-run]
#
# Default workflow:
#   1. Build firmware (idf.py build) if .bin is stale or missing.
#   2. Optionally sign with YubiKey (--sign → calls sign_with_yubikey.sh).
#   3. Generate version.json manifest (version, sha256, size, timestamp).
#   4. Upload chytra-budka.bin + version.json to the manager's OTA API
#      (cb.example.com), which publishes both atomically.
#
# Prerequisites:
#   - get_idf sourced (provides idf.py)
#   - server/secrets/ota_upload_token (bearer token for the manager API)
#   - For --sign: YubiKey plugged in, setup_keys.sh completed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
# Default to the FIELD build dir: this script deploys to the fleet, and the
# fleet runs the field profile (0x10000 partition, DEBUG_ENDPOINTS=n, signed).
# The old default 'build' is the BENCH profile (0x8000, unsigned, debug
# endpoints) — deploying that was a foot-gun caught only by the later
# signature guard. A deployable-profile guard below double-checks the sentinel.
BUILD_DIR="${FIRMWARE_DIR}/build-field"
SIGN=false
DRY_RUN=false

# Publishing moved from rsync-to-zelena to the manager's upload API when the
# fleet cut over to the standalone stack (2026-07-27). ota.example.com no longer
# serves this fleet — every board's ota_url points at cb.example.com.
REPO_ROOT="$(cd "${FIRMWARE_DIR}/.." && pwd)"
OTA_PUBLIC_URL="${CB_OTA_PUBLIC_URL:-https://cb.example.com/ota/chytra-budka}"
APP_NAME="chytra-budka"
FORCE=false
RUN_HIL=true
# Staged rollout: trigger cmd/ota one device at a time and verify each OTA-
# enabled board comes back on the new version before triggering the next. A bad
# image then strands at most one board instead of the whole fleet (the old
# behaviour fanned out to every device at once). --no-canary reverts to that.
CANARY=true
BAKE_SECS=180      # match the firmware mark-valid window
CANARY_TIMEOUT=600 # how long to wait for a board to report the new version
# (OTA download over a weak field link + reboot + reconnect
#  can exceed 6 min; 360 s false-aborted a healthy v0.1.0 bench)

while [ $# -gt 0 ]; do
  case "$1" in
    --sign)
      SIGN=true
      shift
      ;;
    --no-hil)
      RUN_HIL=false
      shift
      ;;
    --no-canary)
      CANARY=false
      shift
      ;;
    --bake-secs)
      BAKE_SECS="$2"
      shift 2
      ;;
    --canary-timeout)
      CANARY_TIMEOUT="$2"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=true
      shift
      ;;
    --force)
      FORCE=true
      shift
      ;;
    -h | --help)
      echo "Usage: ota_upload.sh [--sign] [--build-dir DIR] [--dry-run] [--force] [--no-hil]"
      echo
      echo "  --force   skip pre-flight guards (dirty tree, non-main branch)."
      echo "            Use when you know what you're doing — these guards"
      echo "            exist because an 'oops' OTA hits all field devices"
      echo "            within OTA_CHECK_PERIOD_MS minutes."
      echo "  --no-hil  skip the mandatory HIL gate (build+flash bench, run the"
      echo "            reset→AP→provision→STA lifecycle). EMERGENCY ONLY — you"
      echo "            lose the connected-path check that catches field-only"
      echo "            crashes (see 32ec19a). The gate provisions the bench onto"
      echo "            a station WiFi via tests/hil/run.sh, which sources"
      echo "            SSID+PSK from NetworkManager; CB_PROVISION_SSID/"
      echo "            CB_PROVISION_PSK override it."
      echo "  --no-canary       fan out cmd/ota to ALL devices at once (no staged"
      echo "                    per-board health gate). Default is staged."
      echo "  --bake-secs N     canary bake time after a board reports the new"
      echo "                    version (default ${BAKE_SECS})."
      echo "  --canary-timeout N  how long to wait for a board to take the OTA"
      echo "                    before aborting the rollout (default ${CANARY_TIMEOUT})."
      exit 0
      ;;
    *)
      echo "ERROR: unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

# A relative --build-dir is resolved against the firmware dir — that's where
# build.sh writes its per-profile dirs (build-field, build-prod, …). Without
# this, `--build-dir build-field` run from the repo root looks for the binary
# under <root>/build-field while build.sh wrote it to firmware/build-field, and
# the post-build BIN check fails with "binary not found". (The default is
# already absolute.)
case "$BUILD_DIR" in
  /*) ;;
  *) BUILD_DIR="${FIRMWARE_DIR}/${BUILD_DIR}" ;;
esac

# ── Source ESP-IDF if needed ──────────────────────────────────────────────
# Self-contained, mirroring tools/build.sh: this script calls idf.py (and
# build.sh, which self-sources in its own child process — that doesn't
# propagate back here). Without this, running from a shell that hasn't sourced
# IDF dies with "idf.py: command not found" at the build step.
if ! command -v idf.py >/dev/null 2>&1; then
  IDF_EXPORT="${IDF_EXPORT:-${HOME}/.espressif/v6.0.1/esp-idf/export.sh}"
  if [ ! -f "$IDF_EXPORT" ]; then
    echo "ERROR: idf.py not on PATH and IDF export not found at ${IDF_EXPORT}." >&2
    echo "       Source ESP-IDF v6.0.1 first, or set IDF_EXPORT=/path/to/export.sh" >&2
    exit 3
  fi
  echo "→ sourcing ESP-IDF: ${IDF_EXPORT}"
  # shellcheck disable=SC1090
  . "$IDF_EXPORT" >/dev/null
fi

# ── Step 0: Pre-flight guards ─────────────────────────────────────────────
# Catch the two ways "I'll just push real quick" turns into a field
# incident: pushing uncommitted code (nobody can `git checkout` what
# went out) and pushing from a feature branch (collaborators don't
# know what they're now running). --force overrides both.
if [ "$FORCE" != true ]; then
  branch="$(git -C "$FIRMWARE_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
  if [ "$branch" != "main" ]; then
    echo "ERROR: refusing to OTA from branch '${branch}' (not main)." >&2
    echo "       Push from main, or override with --force if you know" >&2
    echo "       this is a deliberate hotfix from a branch." >&2
    exit 3
  fi
  if ! git -C "$FIRMWARE_DIR" diff --quiet HEAD 2>/dev/null \
    || ! git -C "$FIRMWARE_DIR" diff --quiet --cached HEAD 2>/dev/null; then
    echo "ERROR: refusing to OTA — working tree has uncommitted changes." >&2
    echo "       Commit first so the deployed build matches a git sha," >&2
    echo "       or override with --force for an explicit dirty deploy." >&2
    git -C "$FIRMWARE_DIR" status --short >&2
    exit 3
  fi
fi

BIN="${BUILD_DIR}/${APP_NAME}.bin"

# ── Step 0b: Deployable-profile guard ─────────────────────────────────────
# Refuse to deploy a non-fleet profile. The bench profile is 0x8000 +
# DEBUG_ENDPOINTS=y + unsigned; OTA'ing it to the field bricks (partition
# offset assert) or exposes unauth /debug/* routes. The signature guard later
# catches the unsigned case, but fail early and clearly on the profile too.
# --force overrides (e.g. a deliberate bench-to-bench OTA experiment).
if [ "$FORCE" != true ] && [ -f "${BUILD_DIR}/.cb_build_profile" ]; then
  prof="$(cat "${BUILD_DIR}/.cb_build_profile" 2>/dev/null || echo unknown)"
  case "$prof" in
    field | signed | production | production-yubikey) ;;
    *)
      echo "ERROR: build dir '${BUILD_DIR}' was built for profile '${prof}', not a" >&2
      echo "       deployable fleet profile (field/signed/production*). Deploying it" >&2
      echo "       would brick the field (partition offset) or ship debug endpoints." >&2
      echo "       Build with: tools/build.sh field   (--force to override.)" >&2
      exit 3
      ;;
  esac
fi

# ── Step 1: Build if needed ───────────────────────────────────────────────
# NOTE on the find: the name tests MUST be parenthesised so -newer binds to
# all of them. Without the \( ... \) group the -o branches (*.cpp / *.h) are
# unconstrained by -newer and match unconditionally, so this always reported
# "stale" and rebuilt — which silently CLOBBERS a pre-signed .bin with a fresh
# unsigned one (the signature lives in the image, a rebuild drops it). That
# then shipped an unsigned image the signed fleet rejects. Keep the group.
# Map the build dir to its profile so BOTH the initial build and any
# stale-triggered rebuild below go through tools/build.sh (one home for the
# sdkconfig overlay chain; a bare idf.py build on a fresh dir dies at 2 MB).
case "$(basename "$BUILD_DIR")" in
  build-field) PROFILE=field ;;
  build-signed) PROFILE=signed ;;
  build-prod) PROFILE=production ;;
  build-sb) PROFILE=secureboot-test ;;
  build) PROFILE=bench ;;
  *) PROFILE="" ;;
esac
build_fw() {
  if [ -n "$PROFILE" ]; then
    "${SCRIPT_DIR}/build.sh" "$PROFILE"
  else idf.py -C "$FIRMWARE_DIR" -B "$BUILD_DIR" build; fi
}
fw_version() { # embedded app version (git describe at build time)
  python3 -c "
import json
with open('${BUILD_DIR}/project_description.json') as f:
    d = json.load(f)
    print(d.get('project_version', d.get('version', 'unknown')))
" 2>/dev/null || echo unknown
}
if [ ! -f "$BIN" ] || [ -n "$(find "$FIRMWARE_DIR/main" -newer "$BIN" \( -name '*.c' -o -name '*.cpp' -o -name '*.h' \) 2>/dev/null | head -1)" ]; then
  echo "→ Building firmware..."
  # Build through tools/build.sh so the sdkconfig overlay chain (8 MB flash,
  # signing posture, …) has ONE home. A bare `idf.py build` on a fresh dir
  # defaults to 2 MB flash and dies — exactly the trap this wrapper closes.
  # Map the build dir to its profile; unknown dirs fall back to the old call.
  build_fw
fi

if [ ! -f "$BIN" ]; then
  echo "ERROR: firmware binary not found: $BIN" >&2
  exit 1
fi

# ── Step 1b: Resolve version + refuse DIRTY/STALE *before* signing ────────
# The embedded app version is `git describe --always --dirty` at BUILD time.
# Compute it now and run the dirty/stale guard BEFORE Step 2, so we never spend
# a YubiKey touch on an image the guard would only reject afterwards (it used
# to live after signing — a wasted touch on a bad bin). --force overrides.
VERSION="$(fw_version)"

if [ "$FORCE" != true ]; then
  case "$VERSION" in
    *dirty*)
      echo "ERROR: '$BIN' was built from a DIRTY tree (version=${VERSION})." >&2
      echo "       Commit, then REBUILD, then upload — so the OTA image maps" >&2
      echo "       to a real commit. (--force to override, e.g. a deliberate" >&2
      echo "       throwaway bench image.)" >&2
      exit 5
      ;;
    *) ;;
  esac
  # VERSION is `git describe --tags --always --dirty` baked in at build time —
  # a SemVer tag (e.g. v0.1.0) when HEAD is tagged, the abbreviated HEAD sha
  # when it isn't. It must equal HEAD's describe *now*; if not, the binary was
  # built at a different commit (stale). The old check assumed a bare sha and
  # required VERSION to be a prefix of the HEAD sha — which wrongly rejected
  # every tagged build (a tag is not a prefix of the sha).
  head_desc="$(git -C "$FIRMWARE_DIR" describe --tags --always --dirty 2>/dev/null || echo)"
  if [ -n "$head_desc" ] && [ "$VERSION" != "$head_desc" ]; then
    # Stale: built at a different commit than HEAD. Common cause: a tag added
    # with no source change, so the source-mtime build trigger never fired.
    # Under --sign we build+sign fresh anyway -> auto clean-rebuild from HEAD;
    # without --sign refuse, rather than clobber a pre-signed re-ship bin.
    if [ "$SIGN" = true ]; then
      echo "→ image '${VERSION}' != HEAD '${head_desc}' — clean-rebuilding from HEAD..."
      rm -rf "${BUILD_DIR:?BUILD_DIR unset}"
      build_fw
      VERSION="$(fw_version)"
      if [ "$VERSION" != "$head_desc" ]; then
        echo "ERROR: still stale after rebuild ('${VERSION}' != '${head_desc}')." >&2
        exit 5
      fi
      echo "→ rebuilt to ${VERSION}."
    else
      echo "ERROR: image version '${VERSION}' != HEAD '${head_desc}' — STALE." >&2
      echo "       Re-run with --sign (auto-rebuilds), or rebuild manually." >&2
      echo "       (--force to override.)" >&2
      exit 5
    fi
  fi
  # Release-only: the OTA feed must carry tagged SemVer releases, not a
  # `git describe` dev string (vX.Y.Z-N-gSHA from an untagged commit). A
  # tagged HEAD describes as the bare tag; an untagged one carries the
  # `-<count>-g<sha>` suffix — reject that.
  case "$VERSION" in
    *-g[0-9a-f]*)
      echo "ERROR: '${VERSION}' is an UNTAGGED dev build — the OTA feed is" >&2
      echo "       release-only. Tag this commit a SemVer release and REBUILD:" >&2
      echo "         git tag vX.Y.Z && (cd '${FIRMWARE_DIR}' && tools/build.sh field)" >&2
      echo "       then re-run. (--force overrides — deliberate dev/canary push only.)" >&2
      exit 5
      ;;
    *) ;;
  esac
  echo "→ Version ${VERSION} matches HEAD (${head_desc}) — clean, traceable release."
fi

# ── Step 1b2: HIL gate — the reset→AP→provision→STA lifecycle MUST pass ──────
# A field deploy is gated on a green HIL run of THIS commit on the bench.
# 32ec19a shipped on a bench AP-mode boot-check alone, task_wdt'd on the field
# in the STA/connected path, and rolled back. The lifecycle (firmware/tests/hil)
# exercises AP onboarding + STA so a connected-path regression is caught HERE,
# before the YubiKey touch — not in the field. It builds + flashes the BENCH
# profile of the same commit (erase-flash, since the bench may hold a different
# partition layout), then runs the suite. --no-hil overrides (emergencies only).
#
# NOTE: a bench HIL can't catch a FIELD-HARDWARE-specific fault (sensors the
# bench lacks) — that needs the field coredump. This gate catches everything
# reproducible on the bench, which is the large majority of regressions.
if [ "$RUN_HIL" = true ]; then
  echo "→ HIL gate: build + flash bench, run reset→AP→provision→STA lifecycle"
  BENCH_PORT="${CB_BENCH_PORT:-/dev/esp32-aabbccddee01}"
  if ! (cd "$FIRMWARE_DIR" && "${SCRIPT_DIR}/build.sh" bench); then
    echo "ERROR: HIL gate — bench-profile build failed." >&2
    exit 7
  fi
  if ! (cd "$FIRMWARE_DIR" && "${SCRIPT_DIR}/flash_safe.sh" -p "$BENCH_PORT" -B build erase-flash flash); then
    echo "ERROR: HIL gate — bench flash failed (is the bench on ${BENCH_PORT}?)." >&2
    exit 7
  fi
  # tests/hil/run.sh owns the station credentials the bench re-provisions onto:
  # it takes them from NetworkManager (the PSK is only ever assigned to a
  # variable, never printed) and fails loudly if it can't. This used to be ~30
  # lines of NM probing here that picked the ACTIVE wireless link — which is a
  # different rule from the one a hand-run suite uses, so the gate and a manual
  # run could provision onto different networks. One resolver, one behaviour.
  # Explicit CB_PROVISION_SSID/CB_PROVISION_PSK still win.
  if (cd "${FIRMWARE_DIR}/tests/hil" && ./run.sh -m "not manual"); then
    echo "✓ HIL gate green — proceeding to sign + deploy"
    echo "  NOTE: the gate flashed the BENCH profile (0x8000 partition table,"
    echo "        DEBUG_ENDPOINTS=y). The fleet runs the FIELD profile (0x10000,"
    echo "        debug off) — so a partition-layout or debug-coupled regression"
    echo "        is NOT covered here. The staged canary below is the backstop."
  else
    echo "ERROR: HIL gate FAILED — refusing to deploy to the field." >&2
    echo "       Fix the regression and re-run. (--no-hil overrides — emergency only.)" >&2
    exit 7
  fi
else
  echo "⚠ HIL gate SKIPPED (--no-hil) — deploying to the field WITHOUT the"
  echo "  AP/STA lifecycle check. This is how 32ec19a's field crash shipped."
fi

# ── Step 1c: Confirm-before-touch ────────────────────────────────────────
# A YubiKey touch authorises signing *whatever bytes are in the build dir at
# that instant* — it proves presence, not that anyone vetted the image. An
# agent assembles that dir, so a touch can land on a stale/wrong build (exactly
# how a stale image reached the whole fleet on 2026-05-30). Before unlocking the
# signing key, print the precise identity of what's about to be signed + shipped
# and require an interactive "yes". Fail CLOSED with no terminal to confirm at
# (e.g. a headless/agent shell — which can't drive the PIN/touch anyway, so this
# is just an earlier, clearer stop). --force does NOT skip this: signing the
# fleet is always deliberate.
if [ "$SIGN" = true ]; then
  head_full="$(git -C "$FIRMWARE_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
  subject="$(git -C "$FIRMWARE_DIR" log -1 --pretty=%s 2>/dev/null || echo '?')"
  unsigned_sha="$(sha256sum "$BIN" | cut -d' ' -f1)"
  cat >&2 <<CONFIRM
────────────────────────────────────────────────────────────────────
  ABOUT TO SIGN (YubiKey touch) + DEPLOY TO THE FLEET
    build dir : ${BUILD_DIR}
    version   : ${VERSION}
    HEAD      : ${head_full}
                ${subject}
    bin       : ${BIN}
    size      : $(stat -c%s "$BIN") bytes
    sha256    : ${unsigned_sha}  (unsigned image — signature appended next)
    target    : ${OTA_PUBLIC_URL}/  → all field devices
────────────────────────────────────────────────────────────────────
CONFIRM
  if [ ! -e /dev/tty ]; then
    echo "ERROR: no controlling terminal to confirm at — refusing to sign." >&2
    echo "       Run ota_upload.sh --sign from an interactive shell." >&2
    exit 6
  fi
  printf "Type 'yes' to sign + deploy this exact image: " >&2
  read -r _confirm </dev/tty || _confirm=""
  if [ "$_confirm" != "yes" ]; then
    echo "Aborted — not signing (you typed '${_confirm}', expected 'yes')." >&2
    exit 6
  fi
fi

# ── Step 2: Sign (optional) ──────────────────────────────────────────────
if [ "$SIGN" = true ]; then
  echo "→ Signing with YubiKey..."
  "${SCRIPT_DIR}/sign_with_yubikey.sh" "$(basename "$BUILD_DIR")"
fi

# ── Step 2b: Refuse to ship an unsigned image to the signed fleet ─────────
# Both fielded boards enforce the signature (field ex02 via hardware Secure
# Boot, bench ex01 via soft signed-OTA verify-on-update), so an unsigned
# .bin is dead on arrival — and worse, easy to push by accident (forgot --sign,
# or a stray rebuild dropped the signature). Verify the image carries a valid
# RSA-3072 signature against our public trust anchor before uploading; abort
# loudly otherwise. PUBKEY can be overridden via env for key rotation.
PUBKEY="${CB_SIGN_PUBKEY:-${HOME}/.config/chytra-budka/keys/cb_secure_boot_signing_key_pub.pem}"
SIGNED_OK=false
if [ -f "$PUBKEY" ]; then
  if ! python -m espsecure verify-signature --version 2 --keyfile "$PUBKEY" "$BIN" \
    >/dev/null 2>&1; then
    echo "ERROR: '$BIN' is NOT validly signed for the fleet trust anchor." >&2
    echo "       Re-run with --sign (build + YubiKey-vault sign + upload)," >&2
    echo "       or sign first via tools/sign_with_yubikey.sh. Refusing to" >&2
    echo "       upload — the signed fleet would reject this image." >&2
    exit 4
  fi
  SIGNED_OK=true
  echo "→ Signature verified against $(basename "$PUBKEY")"
else
  # Fail CLOSED to match the device posture: without the trust anchor we
  # cannot prove the image is signed, and shipping an unsigned/garbage bin
  # the signed fleet rejects (or a server-state mismatch) is worse than
  # stopping here. Point CB_SIGN_PUBKEY at the pubkey to proceed.
  echo "ERROR: trust-anchor pubkey not found ($PUBKEY) — cannot verify the" >&2
  echo "       image is signed before upload. Set CB_SIGN_PUBKEY to the" >&2
  echo "       public key (or restore the default path). Refusing to upload." >&2
  exit 4
fi

# ── Step 3: Generate version manifest ────────────────────────────────────
# VERSION + the dirty/stale guard were already resolved in Step 1b (before any
# signing touch). SHA256 here is of the FINAL (signed) image that ships.
SHA256="$(sha256sum "$BIN" | cut -d' ' -f1)"
SIZE="$(stat -c%s "$BIN")"
TIMESTAMP="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

MANIFEST="${BUILD_DIR}/version.json"
cat >"$MANIFEST" <<MANIFEST_EOF
{
  "app": "${APP_NAME}",
  "version": "${VERSION}",
  "sha256": "${SHA256}",
  "size": ${SIZE},
  "signed": ${SIGNED_OK},
  "timestamp": "${TIMESTAMP}"
}
MANIFEST_EOF

echo "→ Manifest:"
cat "$MANIFEST"
echo

# ── Step 4: Upload ───────────────────────────────────────────────────────
if [ "$DRY_RUN" = true ]; then
  echo "[DRY RUN] Would upload:"
  echo "  $BIN + $MANIFEST → ${CB_OTA_UPLOAD_URL:-https://cb.example.com/api/v1/ota/upload}"
  exit 0
fi

"${SCRIPT_DIR}/ota_publish.sh" "$BIN" "$MANIFEST"

echo
echo "✓ Uploaded ${APP_NAME}.bin (${SIZE} bytes, ${VERSION})"
echo "  SHA256: ${SHA256}"
echo "  URL:    ${OTA_PUBLIC_URL}/${APP_NAME}.bin"

# Archive the SIGNED bin + manifest per version so a rollback is a re-upload of
# an exact prior release, not a rebuild+re-sign (the YubiKey dance). The signed
# bin is the public artifact — safe to keep locally. tools/ota_rollback.sh
# <version> re-serves it. (The ELF, archived below, is the SECRET one.)
BIN_ARCHIVE="${CB_BIN_ARCHIVE:-${HOME}/.local/share/chytra-budka/ota-bin}/${VERSION}"
mkdir -p "$BIN_ARCHIVE"
cp -f "$BIN" "$BIN_ARCHIVE/${APP_NAME}.bin"
cp -f "$MANIFEST" "$BIN_ARCHIVE/version.json"
echo "  signed bin archived → ${BIN_ARCHIVE}/ (rollback: tools/ota_rollback.sh ${VERSION})"

# ── Step 4b: Archive the ELF locally for future coredump symbolization ───
# A field coredump records the app's ELF-SHA256, and esp-coredump is fail-closed:
# without the byte-identical .elf (debug symbols) it refuses to produce a
# backtrace. The shipped .bin is stripped, so archive the .elf here, keyed by
# version, so any future panic on this image can be decoded.
#   LOCAL ONLY — the ELF embeds secrets.h values (WiFi/MQTT/HTTP creds) in
#   .rodata, so it must NEVER be rsync'd to the public OTA host. Override the
#   location with CB_ELF_ARCHIVE.
ELF="${BUILD_DIR}/${APP_NAME}.elf"
ELF_ARCHIVE="${CB_ELF_ARCHIVE:-${HOME}/.local/share/chytra-budka/ota-elf}/${VERSION}"
if [ -f "$ELF" ]; then
  mkdir -p "$ELF_ARCHIVE"
  cp -f "$ELF" "$ELF_ARCHIVE/"
  [ -f "${BUILD_DIR}/${APP_NAME}.map" ] && cp -f "${BUILD_DIR}/${APP_NAME}.map" "$ELF_ARCHIVE/"
  elf_sha="$(sha256sum "$ELF" | cut -d' ' -f1)"
  {
    echo "version    : ${VERSION}"
    echo "bin_sha256 : ${SHA256}"
    echo "elf_sha256 : ${elf_sha}"
    echo "size       : ${SIZE}"
  } >"${ELF_ARCHIVE}/info.txt"
  echo "  ELF archived → ${ELF_ARCHIVE}/${APP_NAME}.elf"
  echo "  elf_sha256 (coredump match key): ${elf_sha}"
else
  echo "WARNING: ${ELF} not found — a future coredump for ${VERSION} can't be symbolized." >&2
fi

# ── Step 5: Trigger immediate OTA poll on known devices via MQTT ─────────
# Each device subscribes to <device_id>/cmd/ota and reacts by waking
# the OTA poll task. Without this the boards wait up to 5 min for the
# next periodic check. Credentials come from secrets.h / config.h —
# `secrets.h` is gitignored so they never end up in logs or commits.
if command -v mosquitto_pub >/dev/null 2>&1; then
  CONFIG="${FIRMWARE_DIR}/main/config.h"
  MQTT_HOST=$(grep -E '^#define[[:space:]]+MQTT_HOST' "$CONFIG" | sed 's/.*"\(.*\)".*/\1/')
  MQTT_PORT=$(grep -E '^#define[[:space:]]+MQTT_PORT' "$CONFIG" | grep -oE '[0-9]+$')
  # The fleet broker is mTLS: the canary authenticates with the operator
  # certificate, not the old shared username/password (which the 8883
  # listener does not accept at all). --cafile pins the broker to our chain.
  OPS_CERT="${CB_MQTT_CERT:-${REPO_ROOT}/server/secrets/ops-fleet.pem}"
  OPS_KEY="${CB_MQTT_KEY:-${REPO_ROOT}/server/secrets/ops-fleet.key}"
  OPS_CA="${CB_MQTT_CAFILE:-${REPO_ROOT}/server/secrets/ca_chain.pem}"

  # Known device IDs, in ROLLOUT ORDER — the canary gate below aborts the
  # rollout on the first board that doesn't come back healthy, so the order
  # is the blast-radius order:
  #   1. bench ex01 — normally ota_enabled=OFF (it is USB-flashed, and the
  #      HIL gate already flashed this very build onto it), so it is skipped
  #      gracefully; kept here so a bench with OTA switched on still gets it.
  #   2. the camera-only boards — recoverable (on the USB allowlist).
  #   3. field ex02 LAST: it is OTA-only, so it is the one board that must
  #      never be the canary.
  # Could move to a tools/devices.txt if the fleet grows past 5-10 boxes.
  DEVICES=(cb-ex01 cb-ex03 cb-ex04 cb-ex05 cb-ex02)

  if [ -n "$MQTT_HOST" ] && [ -r "$OPS_CERT" ] && [ -r "$OPS_KEY" ] && [ -r "$OPS_CA" ]; then
    MQ_TLS=(-h "$MQTT_HOST" -p "${MQTT_PORT:-8883}" --cafile "$OPS_CA" --cert "$OPS_CERT" --key "$OPS_KEY")
    mq_pub() { mosquitto_pub "${MQ_TLS[@]}" -q 1 -t "$1" -m "$2" 2>/dev/null; }
    # Read one retained message (5 s timeout), empty on miss.
    mq_read() { mosquitto_sub "${MQ_TLS[@]}" -C 1 -W 5 -t "$1" 2>/dev/null; }
    dev_fw() { mq_read "${1}/state/fw_version" | sed -n 's/.*"version":"\([^"]*\)".*/\1/p'; }

    # Wait for a board to come back on the new VERSION (online), then bake.
    # Returns 0 healthy, 1 timed out / went bad → caller aborts the rollout.
    canary_wait() {
      local dev="$1" deadline=$(($(date +%s) + CANARY_TIMEOUT))
      echo "  ⏳ ${dev}: waiting up to ${CANARY_TIMEOUT}s for ${VERSION}…"
      while [ "$(date +%s)" -lt "$deadline" ]; do
        if [ "$(dev_fw "$dev")" = "$VERSION" ] \
          && [ "$(mq_read "${dev}/state/availability")" = "online" ]; then
          echo "  ✓ ${dev}: on ${VERSION} — baking ${BAKE_SECS}s (mark-valid window)…"
          sleep "$BAKE_SECS"
          if [ "$(dev_fw "$dev")" = "$VERSION" ] \
            && [ "$(mq_read "${dev}/state/availability")" = "online" ]; then
            echo "  ✓ ${dev}: still healthy after bake — canary OK"
            return 0
          fi
          echo "  ✗ ${dev}: dropped/rolled back during bake" >&2
          return 1
        fi
        sleep 15
      done
      echo "  ✗ ${dev}: never reported ${VERSION} within ${CANARY_TIMEOUT}s" >&2
      return 1
    }

    echo
    if [ "$CANARY" = true ]; then
      echo "→ Staged rollout via MQTT (broker ${MQTT_HOST}); canary-first, abort on failure"
    else
      echo "→ Fan-out cmd/ota to ALL devices at once (--no-canary) — no health gate"
    fi
    SKIPPED_OFFLINE=()
    SKIPPED_DISABLED=()
    UPDATED=()
    for dev in "${DEVICES[@]}"; do
      # Is the box even there? cmd/* is NOT retained, so publishing to an
      # absent device is a no-op the broker accepts happily — nothing
      # queues, nothing is delivered. Check BEFORE triggering, or the gate
      # waits out its whole timeout for a version flip that was never
      # requested and then reports it as a failed image. That is what
      # happened on 2026-07-28: cb-ex03 had browned out ~1.5 h earlier
      # (confirmed later from cb_online in Mimir and a "boot after
      # brownout" event), the retained ota_enabled=ON was stale from when
      # it was still alive, and a healthy v0.10.0 looked like a bad build
      # for 10 minutes. An unplugged board is not evidence about the image.
      if [ "$(mq_read "${dev}/state/availability")" != "online" ]; then
        echo "  – ${dev}: OFFLINE (last will says so) — not triggering."
        echo "      Nothing is wrong with the image; this box is not reachable."
        SKIPPED_OFFLINE+=("$dev")
        continue
      fi
      if ! mq_pub "${dev}/cmd/ota" ""; then
        echo "  ✗ ${dev}/cmd/ota — publish failed" >&2
        continue
      fi
      echo "  ✓ ${dev}/cmd/ota triggered"
      [ "$CANARY" = true ] || continue
      # A board with ota_enabled=OFF (e.g. a paused field unit) ignores
      # cmd/ota — no version flip will ever come, so don't gate on it.
      if [ "$(mq_read "${dev}/state/cfg/ota_enabled")" != "ON" ]; then
        echo "  – ${dev}: ota_enabled≠ON, ignores the trigger — skipping health gate"
        SKIPPED_DISABLED+=("$dev")
        continue
      fi
      if ! canary_wait "$dev"; then
        echo "ERROR: ${dev} WAS online, took the trigger, and did not come" >&2
        echo "       back healthy on ${VERSION} — this one is about the image." >&2
        echo "       ABORTING rollout; remaining devices NOT triggered." >&2
        echo "       (It rolls back on its own if it never marked the image valid;" >&2
        echo "        firmware/tools/ota_rollback.sh <prev-version> forces it.)" >&2
        exit 8
      fi
      UPDATED+=("$dev")
    done
    # Say what actually happened. "rollout complete" over a list of boards
    # that were never triggered is how a partial deploy gets mistaken for a
    # finished one.
    if [ ${#SKIPPED_OFFLINE[@]} -eq 0 ] && [ ${#SKIPPED_DISABLED[@]} -eq 0 ]; then
      echo "✓ rollout complete — ${#UPDATED[@]} device(s) on ${VERSION}"
    else
      echo "⚠ rollout PARTIAL — ${#UPDATED[@]} device(s) on ${VERSION}"
      [ ${#SKIPPED_OFFLINE[@]} -gt 0 ] \
        && echo "  offline, still on the old image: ${SKIPPED_OFFLINE[*]}"
      [ ${#SKIPPED_DISABLED[@]} -gt 0 ] \
        && echo "  ota_enabled≠ON, ignored the trigger: ${SKIPPED_DISABLED[*]}"
      echo "  The image is published and archived, so finishing later costs no"
      echo "  rebuild and no YubiKey touch — power the board back up and run:"
      echo "    firmware/tools/ota_rollback.sh ${VERSION}"
      echo "  (that re-serves this exact archived build and re-runs the staged gate)"
    fi
  else
    echo "(skipping MQTT trigger — no operator certificate at ${OPS_CERT};"
    echo " issue one with server/scripts/issue-client-cert.sh ops-fleet)"
  fi
else
  echo "(skipping MQTT trigger — mosquitto_pub not installed)"
fi
