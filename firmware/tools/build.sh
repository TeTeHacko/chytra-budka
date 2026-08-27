#!/usr/bin/env bash
# tools/build.sh — one source of truth for the firmware build profiles.
#
# What this wrapper is for: the bench/field/signing posture lives in per-profile
# sdkconfig overlay files, and IDF applies those only through an explicit
# `-DSDKCONFIG_DEFAULTS=…;…;…` chain at set-target time. That chain used to live
# only in the docs (and in people's heads). This wrapper encodes every profile
# so nobody has to retype (or misremember) it, and gives each profile its own
# build dir so their configs can't overwrite each other.
#
# What a plain `idf.py build` actually does — the header here used to claim it
# "NEVER WORKS" and falls back to 2 MB flash; that is WRONG and was corrected
# after someone followed CONTRIBUTING.md and hit no such failure. Once the
# target is set, IDF auto-applies `sdkconfig.defaults` AND its `.<target>`
# sibling (esp-idf/tools/cmake/kconfig.cmake appends `${file}.${idf_target}`),
# and those two carry the 8 MB flash, PSRAM and 0x10000 partition offset. So:
#
#   idf.py set-target esp32s3 && idf.py build   → a VALID 8 MB image, but
#                                                 profile-less: no bench
#                                                 /debug/* endpoints, no field
#                                                 signing posture, and it
#                                                 writes ./sdkconfig + build/
#                                                 which then collide with this
#                                                 wrapper's per-profile state.
#   idf.py build  (set-target SKIPPED)          → target stays the IDF default,
#                                                 the .esp32s3 overlay is never
#                                                 applied, and THAT is the
#                                                 config that really breaks.
#
# Use the wrapper. Just don't expect a bare build to fail loudly — it won't;
# it will quietly hand you an image without your profile's flags.
#
# Usage:
#   tools/build.sh <profile> [idf.py args...]
#
#   tools/build.sh field                 # build the deployable field/OTA image
#   tools/build.sh bench                  # bench debug image
#   tools/build.sh field menuconfig       # tweak that profile's config
#   tools/build.sh bench size-components  # any idf.py action, forwarded
#
# Two profiles only (overlay chain is always sdkconfig.defaults + .esp32s3 + below):
#   field   .signed_soft .field   → build-field/  THE signed OTA image → fleet
#   bench   .bench                 → build/        the one debug image (/debug/* on)
#
# Both build the SAME code — incl. BLE + the memory diet, folded into
# .esp32s3 so it's one canonical build — so bench and field differ only by the
# /debug/* endpoints (plus, inherently, the OTA signature and poll cadence).
# The Secure-Boot/production overlays (.signed, .production, .production_yubikey,
# .secureboot_test) are intentionally NOT wired here; they sit on disk for the
# documented secure-boot path (firmware/README.md) — build manually via
# idf.py -DSDKCONFIG_DEFAULTS=… if ever needed.
#
# It auto-sources ESP-IDF if idf.py isn't on PATH (override with IDF_EXPORT=…),
# and only runs `set-target` (the slow fullclean) when the build dir is fresh
# or its profile changed — otherwise it's a fast incremental build.
#
# NOTE: flashing a *field* board through `idf.py flash` here would bypass the
# MAC allowlist — use tools/flash_safe.sh for that. Signed OTA → tools/ota_upload.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BASE="sdkconfig.defaults;sdkconfig.defaults.esp32s3"

usage() {
  sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

[ $# -ge 1 ] || usage 1
PROFILE="$1"
shift

case "$PROFILE" in
  field)
    OVERLAYS="${BASE};sdkconfig.defaults.signed_soft;sdkconfig.defaults.field"
    BUILD_DIR="build-field"
    ;;
  bench)
    OVERLAYS="${BASE};sdkconfig.defaults.bench"
    BUILD_DIR="build"
    ;;
  -h | --help | help) usage 0 ;;
  *)
    echo "ERROR: unknown profile '${PROFILE}'. Known: field bench." >&2
    echo "(The Secure-Boot overlays .production*/.signed/.secureboot_test are intentionally" >&2
    echo " not wired here — build them manually via idf.py -DSDKCONFIG_DEFAULTS=…; see firmware/README.md.)" >&2
    exit 2
    ;;
esac

# ── Source ESP-IDF if needed ──────────────────────────────────────────────
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

cd "$FW_DIR"

# ── Reconfigure only when needed ──────────────────────────────────────────
# set-target triggers a fullclean (slow). Do it only when the dir is fresh or
# was last built for a DIFFERENT profile — a sentinel records which profile a
# build dir holds, so switching profiles (or recovering a dir left in a bad
# config, like a stray profile-less `idf.py build` in this tree) auto-heals.
SENTINEL="${BUILD_DIR}/.cb_build_profile"
NEED_CONFIGURE=0
if [ ! -f "${BUILD_DIR}/sdkconfig" ] || [ ! -f "$SENTINEL" ] \
  || [ "$(cat "$SENTINEL" 2>/dev/null || true)" != "$PROFILE" ]; then
  NEED_CONFIGURE=1
fi

# Default action is `build`; otherwise forward whatever idf.py actions/flags
# the caller passed (menuconfig, size, flash, monitor, -p PORT, …).
ACTIONS=("$@")
[ ${#ACTIONS[@]} -gt 0 ] || ACTIONS=(build)

echo "→ profile=${PROFILE}  dir=${BUILD_DIR}  actions=${ACTIONS[*]}"
echo "  overlays: ${OVERLAYS//;/ + }"

if [ "$NEED_CONFIGURE" = 1 ]; then
  echo "→ (re)configuring build dir (set-target esp32s3)…"
  idf.py -DSDKCONFIG_DEFAULTS="$OVERLAYS" -DSDKCONFIG="${BUILD_DIR}/sdkconfig" \
    -B "$BUILD_DIR" set-target esp32s3 "${ACTIONS[@]}"
  echo "$PROFILE" >"$SENTINEL"
else
  idf.py -DSDKCONFIG_DEFAULTS="$OVERLAYS" -DSDKCONFIG="${BUILD_DIR}/sdkconfig" \
    -B "$BUILD_DIR" "${ACTIONS[@]}"
fi
