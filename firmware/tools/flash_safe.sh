#!/usr/bin/env bash
# tools/flash_safe.sh — MAC-allowlist guard for idf.py flash.
#
# Refuses to flash any device that isn't reached through a MAC-pinned
# udev symlink (/dev/esp32-<mac>) AND whose MAC isn't on the allowlist
# in tools/devices.txt.
#
# Why a wrapper at all
# --------------------
# Production eFuse burns are irreversible. A distracted operator
# running `idf.py -p /dev/ttyACM0 flash` against the wrong physical
# board can brick a field unit with a stale signing key (or, in the
# pre-production stage, push an unsigned dev build to a board that
# was supposed to stay OTA-only). This wrapper makes the safe path
# the default and the unsafe path explicit (use raw `idf.py flash`
# only when you genuinely want it).
#
# Why the symlink (not esptool read-mac)
# --------------------------------------
# `esptool.py read-mac` puts the chip into bootloader mode, which
# interrupts the running firmware. We don't want a pre-flight check
# to itself disrupt the device. Per memory/bench_boards.md the udev
# rule pins /dev/esp32-<mac> to a specific MAC, so the symlink name
# IS the safety boundary — extract the MAC suffix and check the
# allowlist without touching the chip.
#
# Usage
# -----
#   tools/flash_safe.sh -p /dev/esp32-<mac> [flash | flash monitor | ...]
#
# Example:
#   tools/flash_safe.sh -p /dev/esp32-aabbccddee01 flash

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ALLOWLIST="$SCRIPT_DIR/devices.txt"

# Find the -p|--port value in the args and pass everything through to idf.py.
PORT=""
ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    -p | --port)
      PORT="${2:-}"
      ARGS+=("$1" "$2")
      shift 2
      ;;
    *)
      ARGS+=("$1")
      shift
      ;;
  esac
done

if [ -z "$PORT" ]; then
  echo "ERROR: -p <port> is required. Use the /dev/esp32-<mac> symlink, not a raw ttyACMn." >&2
  exit 1
fi

# Symlink convention: /dev/esp32-<12-hex-mac>
BASE=$(basename "$PORT")
case "$BASE" in
  esp32-*)
    MAC=${BASE#esp32-}
    MAC=$(echo "$MAC" | tr '[:upper:]' '[:lower:]')
    ;;
  *)
    cat >&2 <<EOF
ERROR: $PORT is not a MAC-pinned udev symlink (expected /dev/esp32-<mac>).
       The symlink IS the safety boundary — raw /dev/ttyACMn can shuffle
       across replug/USB-hub re-enumerate and reach the wrong board.
       See memory/bench_boards.md for the udev rule that creates these.
EOF
    exit 1
    ;;
esac

if [ ! -f "$ALLOWLIST" ]; then
  echo "ERROR: allowlist $ALLOWLIST not found." >&2
  exit 1
fi

# Allowlist match: ${MAC} as a whole field at start of line (comments OK after whitespace).
if ! grep -qiE "^${MAC}([[:space:]]|$)" "$ALLOWLIST"; then
  cat >&2 <<EOF
REFUSING: MAC $MAC is not in $ALLOWLIST.
       Add it explicitly only if this is the right target.
       Field / OTA-only boards must stay off the allowlist so a
       distracted USB flash can't downgrade or unsign them.
EOF
  exit 1
fi

# idf.py resolves the project (and its build/ config) from the CWD. Don't
# trust the caller's directory: run from the repo/worktree root and idf.py
# finds no CMakeLists.txt, "builds" nothing, prints a one-line notice and
# exits 0 — leaving the board on whatever stale image it had. That silent
# no-op flash burned ~an hour once (a board kept running an old build while
# every "flash" reported success). Always run from the firmware project root
# next to this script, and fail loudly if it isn't one.
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)" # tools/ -> firmware/
cd "$PROJECT_DIR" || {
  echo "ERROR: cannot cd to $PROJECT_DIR" >&2
  exit 1
}
if [ ! -f CMakeLists.txt ]; then
  echo "ERROR: no CMakeLists.txt in $PROJECT_DIR — not an ESP-IDF project dir." >&2
  exit 1
fi
if [ ! -d build ]; then
  cat >&2 <<EOF
ERROR: no build/ in $PROJECT_DIR.
       Run firmware/tools/build.sh <profile> first — a bare idf.py flash would
       reconfigure with the wrong (2 MB) flash defaults and die. See CLAUDE.md.
EOF
  exit 1
fi

echo "flash_safe: $MAC on allowlist; flashing from $PROJECT_DIR with idf.py ${ARGS[*]}"
exec idf.py "${ARGS[@]}"
