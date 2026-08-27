#!/usr/bin/env bash
# tools/ota_rollback.sh — fast rollback: re-serve an archived SIGNED build.
#
#   ota_rollback.sh <version> [--no-canary] [--list]
#
# ota_upload.sh archives every signed bin + manifest under
# ~/.local/share/chytra-budka/ota-bin/<version>/ (CB_BIN_ARCHIVE overrides).
# This re-uploads that exact pair to the OTA host and triggers a staged
# cmd/ota — NO rebuild, NO YubiKey touch (the bin is already signed). That
# turns "roll back to the last good release" from a multi-minute build+sign+
# upload dance into seconds, which is what you want mid-incident.
#
# It does NOT bump anything: the downgrade guard in ota.c fails OPEN on the
# reproducible-build blank date, so the board accepts an older signed image.
# (That same fail-open is why an *accidental* old upload is dangerous — hence
# this is an explicit, named tool.)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
APP_NAME="chytra-budka"
BIN_ARCHIVE_ROOT="${CB_BIN_ARCHIVE:-${HOME}/.local/share/chytra-budka/ota-bin}"
CANARY=true
# Rollout order mirrors ota_upload.sh: field ex02 last, it is OTA-only.
DEVICES=(cb-ex01 cb-ex03 cb-ex04 cb-ex05 cb-ex02)
# Overridable: 360 s is tight for a 2 MB image over a WAN link on a weak signal
# (ota_upload.sh allows 600 s for the same download), and a canary that times out
# aborts the whole rollout.
BAKE_SECS="${CB_BAKE_SECS:-180}"
CANARY_TIMEOUT="${CB_CANARY_TIMEOUT:-600}"

if [ "${1:-}" = "--list" ]; then
  echo "Archived signed versions in ${BIN_ARCHIVE_ROOT}:"
  ls -1 "${BIN_ARCHIVE_ROOT}" 2>/dev/null || echo "  (none)"
  exit 0
fi

VERSION="${1:-}"
shift || true
[ "${1:-}" = "--no-canary" ] && CANARY=false
if [ -z "$VERSION" ]; then
  echo "Usage: ota_rollback.sh <version> [--no-canary] [--list]" >&2
  echo "       ota_rollback.sh --list   # show archived signed versions" >&2
  exit 2
fi

ARCHIVE="${BIN_ARCHIVE_ROOT}/${VERSION}"
BIN="${ARCHIVE}/${APP_NAME}.bin"
MANIFEST="${ARCHIVE}/version.json"
if [ ! -f "$BIN" ] || [ ! -f "$MANIFEST" ]; then
  echo "ERROR: no archived signed build for ${VERSION} at ${ARCHIVE}" >&2
  echo "       Run 'ota_rollback.sh --list' to see what's available." >&2
  exit 1
fi

# Re-verify the archived bin is validly signed before re-serving it — the
# archive could have been tampered with, and the fleet enforces the signature.
PUBKEY="${CB_SIGN_PUBKEY:-${HOME}/.config/chytra-budka/keys/cb_secure_boot_signing_key_pub.pem}"
if command -v python >/dev/null 2>&1 && [ -f "$PUBKEY" ]; then
  if ! python -m espsecure verify-signature --version 2 --keyfile "$PUBKEY" "$BIN" >/dev/null 2>&1; then
    echo "ERROR: archived ${VERSION} bin fails signature verify against ${PUBKEY}" >&2
    exit 4
  fi
  echo "→ archived ${VERSION} signature verified"
fi

echo "→ Rolling back: re-serving archived signed ${VERSION}"
sha="$(sha256sum "$BIN" | cut -d' ' -f1)"
echo "  sha256: ${sha}"

# Same server-side atomic publish as ota_upload.sh.
"${SCRIPT_DIR}/ota_publish.sh" "$BIN" "$MANIFEST"
echo "✓ ${APP_NAME}.bin now serving ${VERSION}"

# Staged cmd/ota — same canary-first/abort posture as ota_upload.sh.
CONFIG="${FIRMWARE_DIR}/main/config.h"
REPO_ROOT="$(cd "${FIRMWARE_DIR}/.." && pwd)"
MQTT_HOST=$(grep -E '^#define[[:space:]]+MQTT_HOST' "$CONFIG" | sed 's/.*"\(.*\)".*/\1/')
MQTT_PORT=$(grep -E '^#define[[:space:]]+MQTT_PORT' "$CONFIG" | grep -oE '[0-9]+$')
# mTLS operator identity, same as ota_upload.sh — the fleet listener does not
# take a username/password at all.
OPS_CERT="${CB_MQTT_CERT:-${REPO_ROOT}/server/secrets/ops-fleet.pem}"
OPS_KEY="${CB_MQTT_KEY:-${REPO_ROOT}/server/secrets/ops-fleet.key}"
OPS_CA="${CB_MQTT_CAFILE:-${REPO_ROOT}/server/secrets/ca_chain.pem}"
if ! command -v mosquitto_pub >/dev/null 2>&1 || [ -z "$MQTT_HOST" ] || [ ! -r "$OPS_CERT" ]; then
  echo "(no mosquitto_pub / MQTT creds — boards pick up ${VERSION} at their next poll)"
  exit 0
fi
mq_pub() { mosquitto_pub -h "$MQTT_HOST" -p "${MQTT_PORT:-8883}" --cafile "$OPS_CA" --cert "$OPS_CERT" --key "$OPS_KEY" -q 1 -t "$1" -m "$2" 2>/dev/null; }
mq_read() { mosquitto_sub -h "$MQTT_HOST" -p "${MQTT_PORT:-8883}" --cafile "$OPS_CA" --cert "$OPS_CERT" --key "$OPS_KEY" -C 1 -W 5 -t "$1" 2>/dev/null; }
dev_fw() { mq_read "${1}/state/fw_version" | sed -n 's/.*"version":"\([^"]*\)".*/\1/p'; }

SKIPPED_OFFLINE=()
SKIPPED_DISABLED=()
for dev in "${DEVICES[@]}"; do
  # cmd/* is not retained: publishing at an offline board delivers nothing, so
  # the gate below would wait out its full timeout for a version flip nobody
  # was asked for, then report it as a failed image. Check reachability first
  # (same fix as ota_upload.sh — see the note there for the incident).
  if [ "$(mq_read "${dev}/state/availability")" != "online" ]; then
    echo "  – ${dev}: OFFLINE — not triggering (not an image problem)"
    SKIPPED_OFFLINE+=("$dev")
    continue
  fi
  mq_pub "${dev}/cmd/ota" "" && echo "  ✓ ${dev}/cmd/ota triggered"
  [ "$CANARY" = true ] || continue
  [ "$(mq_read "${dev}/state/cfg/ota_enabled")" = "ON" ] || {
    echo "  – ${dev}: ota_enabled≠ON — skip gate"
    SKIPPED_DISABLED+=("$dev")
    continue
  }
  deadline=$(($(date +%s) + CANARY_TIMEOUT))
  ok=false
  while [ "$(date +%s)" -lt "$deadline" ]; do
    if [ "$(dev_fw "$dev")" = "$VERSION" ] && [ "$(mq_read "${dev}/state/availability")" = "online" ]; then
      sleep "$BAKE_SECS"
      [ "$(dev_fw "$dev")" = "$VERSION" ] && ok=true
      break
    fi
    sleep 15
  done
  if [ "$ok" != true ]; then
    echo "ERROR: ${dev} was online, took the trigger, and didn't settle on" >&2
    echo "       ${VERSION} — aborting rollout" >&2
    exit 8
  fi
  echo "  ✓ ${dev}: now on ${VERSION}"
done
if [ ${#SKIPPED_OFFLINE[@]} -eq 0 ] && [ ${#SKIPPED_DISABLED[@]} -eq 0 ]; then
  echo "✓ rollout complete — every device is on ${VERSION}"
  exit 0
fi
# Keep the two reasons apart. They are not the same news: an offline board is
# still running the OLD image and someone has to go look at it, whereas an
# ota_enabled=OFF board (the USB-flashed bench) was deliberately excluded and is
# usually already on this build by another route. Lumping them into one "skipped"
# list makes a healthy exclusion look like an outstanding problem, and vice versa.
if [ ${#SKIPPED_OFFLINE[@]} -gt 0 ]; then
  echo "⚠ rollout PARTIAL — offline, STILL ON THE OLD IMAGE: ${SKIPPED_OFFLINE[*]}"
  echo "  Power/network problem on those boxes, not an image problem. Re-run this"
  echo "  same command once they are back — no rebuild, no YubiKey touch."
else
  echo "✓ rollout complete for every reachable device on ${VERSION}"
fi
[ ${#SKIPPED_DISABLED[@]} -gt 0 ] \
  && echo "  (ota_enabled=OFF, excluded on purpose: ${SKIPPED_DISABLED[*]})"
exit 0
