#!/usr/bin/env bash
# tools/sign_with_yubikey.sh — sign Secure Boot v2 app images with the
# RSA-3072 signing key held in a YubiKey-gated age vault.
#
# Usage: sign_with_yubikey.sh [build_dir]            (default: build)
#        CB_SIGN_BOOTLOADER=1 sign_with_yubikey.sh build-prod   (full Secure Boot)
#
# Why this shape (see firmware/HTTPS.md / signing notes):
#   ESP32-S3 Secure Boot v2 needs RSA-3072 *RSA-PSS*. The YubiKey 5.1.2 cannot
#   produce that on-card (PIV holds RSA-3072 only on fw >= 5.7; the OpenPGP
#   Signature key does PKCS#1 v1.5, not PSS). So the private key lives in an
#   *age* vault that a YubiKey unlocks with a physical touch, and espsecure
#   does the PSS signing in software. The plaintext key only ever exists in
#   tmpfs (/dev/shm) for the duration of one signing run, then is shredded.
#
# Workflow:
#   1. Locate the unsigned app .bin in <build_dir> (and bootloader.bin only if
#      CB_SIGN_BOOTLOADER=1, i.e. a full Secure Boot v2 build).
#   2. age -d the vault PEM -> YubiKey TOUCH -> plaintext key in /dev/shm.
#   3. espsecure sign-data --version 2 --keyfile <shm-pem>  (RSA-3072 PSS).
#   4. espsecure verify-signature against the public key (sanity).
#   5. Shred the tmpfs key copy (also on any error / Ctrl-C, via trap).
#
# Successor (when a YubiKey >= 5.7 arrives): import the SAME PEM onto PIV slot
# 9c and switch to on-card signing with `espsecure sign-data --hsm` + ykcs11.
# Same key / same public-key trust anchor, so nothing downstream changes.
#
# Prereqs:
#   - ESP-IDF v6.0.1 sourced (provides `espsecure`)
#   - `age` + the YubiKey age identity set up (tools/setup_signing_vault.sh)
#   - YubiKey holding the age identity plugged in
#
# Tested against: ESP-IDF v6.0.1 (espsecure v5.3.dev3), age 1.x + age-plugin-yubikey.

set -euo pipefail

BUILD_DIR="${1:-build}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
KEYDIR="${HOME}/.config/chytra-budka/keys"

KEY_AGE="${CB_KEY_AGE:-$KEYDIR/cb_secure_boot_signing_key.pem.age}"
PUB_KEY="${CB_PUB_KEY:-$KEYDIR/cb_secure_boot_signing_key_pub.pem}"
# age-plugin-yubikey identity stub (AGE-PLUGIN-YUBIKEY-...). If absent, we fall
# back to the passphrase recipient (age prompts) — the break-glass path.
AGE_IDENTITY="${CB_AGE_IDENTITY:-$KEYDIR/cb_age_yubikey_identity.txt}"

die() {
  echo "ERROR: $*" >&2
  exit 1
}

[ -d "$FIRMWARE_DIR/$BUILD_DIR" ] || die "build dir not found: $FIRMWARE_DIR/$BUILD_DIR"
[ -f "$KEY_AGE" ] || die "vault key not found at $KEY_AGE (run tools/setup_signing_vault.sh)."
[ -f "$PUB_KEY" ] || die "public key not found at $PUB_KEY."
command -v age >/dev/null 2>&1 || die "age not on PATH (install 'age')."
for cmd in espsecure openssl; do
  command -v "$cmd" >/dev/null 2>&1 || die "$cmd not on PATH (source ESP-IDF v6.0.1)."
done

# --- Unlock the signing key into tmpfs (RAM) only -----------------------------
# The plaintext signing key must NEVER touch persistent disk — `shred -u` is
# unreliable on journaling/CoW filesystems, so a /tmp fallback would silently
# break the "RAM-only" guarantee this script's header promises. If /dev/shm
# isn't a writable tmpfs, ABORT rather than fall back to disk. (Override with
# CB_SIGN_TMPFS=/path if your tmpfs lives elsewhere.)
SHM_DIR="${CB_SIGN_TMPFS:-/dev/shm}"
if [ ! -d "$SHM_DIR" ] || [ ! -w "$SHM_DIR" ]; then
  echo "ERROR: $SHM_DIR is not a writable tmpfs — refusing to write the" >&2
  echo "       plaintext signing key to persistent disk. Mount a tmpfs or" >&2
  echo "       set CB_SIGN_TMPFS to one, then re-run." >&2
  exit 1
fi
PEM_PLAIN="$(
  umask 077
  mktemp "${SHM_DIR}/cb_sign_key.XXXXXX"
)"

cleanup() {
  if [ -f "$PEM_PLAIN" ]; then
    shred -u "$PEM_PLAIN" 2>/dev/null || rm -f "$PEM_PLAIN"
  fi
}
trap cleanup EXIT INT TERM

echo "→ Unlocking signing key from vault: $KEY_AGE"
if [ -f "$AGE_IDENTITY" ]; then
  echo "  (touch your YubiKey when it blinks)"
  age -d -i "$AGE_IDENTITY" -o "$PEM_PLAIN" "$KEY_AGE" \
    || die "age decrypt failed (YubiKey present? touched in time?)."
else
  echo "  No YubiKey identity at $AGE_IDENTITY — falling back to recovery passphrase."
  age -d -o "$PEM_PLAIN" "$KEY_AGE" \
    || die "age decrypt failed (wrong passphrase?)."
fi
[ -s "$PEM_PLAIN" ] || die "decrypted key is empty."
chmod 600 "$PEM_PLAIN"

cd "$FIRMWARE_DIR/$BUILD_DIR"

sign_one() {
  local bin="$1"
  if [ ! -f "$bin" ]; then
    echo "skip: $bin (not built)"
    return 0
  fi
  echo "→ Signing $bin (RSA-3072 PSS, software sign of vault-unlocked key)"

  local signed="${bin%.bin}_signed.bin"
  espsecure sign-data \
    --version 2 \
    --keyfile "$PEM_PLAIN" \
    --output "$signed" \
    "$bin"

  # Verify the SIGNED artifact against the public trust anchor BEFORE
  # overwriting the original. With set -e, a verify failure aborts here and
  # leaves the unsigned $bin intact (clean rollback) instead of replacing it
  # with a signed-but-unverified blob.
  espsecure verify-signature \
    --version 2 \
    --keyfile "$PUB_KEY" \
    "$signed"

  # Verified — replace unsigned with signed in-place so idf.py flash /
  # ota_upload.sh pick the right file.
  mv -f "$signed" "$bin"

  echo "✓ Signed + verified: $bin"
}

# Soft signed-OTA (no hardware Secure Boot) only needs the APP signed — the
# bootloader is not verified, so we don't touch it. For a full Secure Boot v2
# build (which also signs + verifies the bootloader), set CB_SIGN_BOOTLOADER=1.
if [ "${CB_SIGN_BOOTLOADER:-0}" = "1" ]; then
  sign_one bootloader/bootloader.bin
fi
sign_one chytra-budka.bin

echo
echo "App signature applied + verified (RSA-3072 PSS, key from YubiKey-gated vault)."
echo "You can now run:  idf.py -B $BUILD_DIR flash      (or firmware/tools/flash_safe.sh)"
