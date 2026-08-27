#!/usr/bin/env bash
# tools/setup_keys.sh — generate / provision production signing + encryption keys.
#
# Two modes:
#   (default)    Generate RSA-3072 Secure Boot signing key on disk.
#   --yubikey    Use a YubiKey OpenPGP SIGN subkey (RSA-3072). Only the public
#                key is exported to disk; the private key never leaves the YK.
#
# Flash Encryption + NVS encryption keys are always symmetric and live on disk
# (~/.config/chytra-budka/keys, mode 700). YubiKey cannot store XTS-AES-256
# keys.
#
# Idempotent: existing keys are kept. Delete a file to regenerate it.
#
# Requires `get_idf` to be sourced (provides espsecure.py + nvs_partition_gen.py).
# For --yubikey: also requires gpg, ykman, and a YubiKey 5+ with OpenPGP applet.

set -euo pipefail

MODE="local"
GPG_KEY_ID=""

while [ $# -gt 0 ]; do
  case "$1" in
    --yubikey)
      MODE="yubikey"
      shift
      ;;
    --key-id)
      GPG_KEY_ID="$2"
      shift 2
      ;;
    -h | --help)
      cat <<'EOF'
Usage: setup_keys.sh [--yubikey [--key-id <gpg-key-id>]]

Without --yubikey: generates an on-disk RSA-3072 signing key.
With --yubikey:    extracts the public part of an RSA-3072 SIGN subkey from
                   a YubiKey OpenPGP applet to disk; the private key stays
                   on the token.

To provision a YubiKey first time:
  gpg --card-edit
  > admin
  > generate           # accept defaults; choose RSA, 3072 bits, expiry as desired
  > quit
EOF
      exit 0
      ;;
    *)
      echo "ERROR: unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

KEYDIR="${HOME}/.config/chytra-budka/keys"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if ! command -v espsecure.py >/dev/null 2>&1; then
  echo "ERROR: espsecure.py not on PATH. Run: get_idf" >&2
  exit 1
fi

mkdir -p "$KEYDIR"
chmod 700 "$KEYDIR"

# ── Secure Boot v2 — RSA-3072 signing key ────────────────────────────────
SB_PRIV_KEY="$KEYDIR/secure_boot_signing_key.pem"
SB_PUB_KEY="$KEYDIR/secure_boot_signing_key_pub.pem"

if [ "$MODE" = "yubikey" ]; then
  for cmd in gpg ykman; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
      echo "ERROR: $cmd not on PATH (needed for --yubikey)." >&2
      exit 1
    fi
  done

  if ! ykman --version >/dev/null 2>&1; then
    echo "ERROR: ykman cannot talk to a YubiKey. Plug it in and retry." >&2
    exit 1
  fi

  if ! gpg --card-status >/dev/null 2>&1; then
    echo "ERROR: gpg --card-status failed. Is the OpenPGP applet provisioned?" >&2
    echo "       Run: gpg --card-edit  →  admin  →  generate" >&2
    exit 1
  fi

  if [ -z "$GPG_KEY_ID" ]; then
    # Pick the first signing-capable key on the card.
    GPG_KEY_ID="$(gpg --list-keys --with-colons --with-keygrip 2>/dev/null \
      | awk -F: '/^pub/{key=$5} /^sub/ && $12 ~ /s/ {print key; exit}')"
    if [ -z "$GPG_KEY_ID" ]; then
      echo "ERROR: no signing-capable key found in keyring." >&2
      echo "       Pass --key-id <id> explicitly." >&2
      exit 1
    fi
    echo "→ Auto-detected GPG key: $GPG_KEY_ID"
  fi

  if [ ! -f "$SB_PUB_KEY" ]; then
    echo "→ Exporting RSA-3072 public key from YubiKey OpenPGP card…"
    # gpg --export-ssh-key emits an OpenSSH RSA pub line; convert to PEM PKCS#8.
    TMP_SSH="$(mktemp)"
    gpg --export-ssh-key "$GPG_KEY_ID" >"$TMP_SSH"
    ssh-keygen -e -m PKCS8 -f "$TMP_SSH" >"$SB_PUB_KEY"
    rm -f "$TMP_SSH"
    chmod 644 "$SB_PUB_KEY"

    # Sanity-check: must be RSA-3072.
    BITS="$(openssl rsa -pubin -in "$SB_PUB_KEY" -text -noout 2>/dev/null \
      | awk '/Public-Key:/ {gsub(/[^0-9]/,"",$2); print $2}')"
    if [ "$BITS" != "3072" ]; then
      echo "ERROR: extracted key is RSA-${BITS:-?}, expected RSA-3072." >&2
      rm -f "$SB_PUB_KEY"
      exit 1
    fi
    echo "✓ YubiKey RSA-3072 public key saved: $SB_PUB_KEY"
  else
    echo "✓ YubiKey public key already exists: $SB_PUB_KEY"
  fi

  # Stamp the GPG key id so sign_with_yubikey.sh can find the right key.
  echo "$GPG_KEY_ID" >"$KEYDIR/yubikey_gpg_id"
  chmod 600 "$KEYDIR/yubikey_gpg_id"

  # Make sure we don't accidentally have a private key on disk.
  if [ -f "$SB_PRIV_KEY" ]; then
    echo "WARNING: $SB_PRIV_KEY exists alongside YubiKey key." >&2
    echo "         Delete it to avoid in-tree signing using the disk key." >&2
  fi
else
  # Local mode — generate a private key on disk.
  if [ ! -f "$SB_PRIV_KEY" ]; then
    echo "→ Generating Secure Boot v2 signing key (RSA-3072)…"
    espsecure.py generate_signing_key --version 2 --scheme rsa3072 "$SB_PRIV_KEY"
    chmod 600 "$SB_PRIV_KEY"
  else
    echo "✓ Secure Boot key already exists: $SB_PRIV_KEY"
  fi

  # Also extract the matching public key so the YubiKey overlay path stays valid
  # even when we sign locally (lets you flip overlays without re-running setup).
  if [ ! -f "$SB_PUB_KEY" ] || [ "$SB_PRIV_KEY" -nt "$SB_PUB_KEY" ]; then
    espsecure.py extract_public_key --version 2 \
      --keyfile "$SB_PRIV_KEY" "$SB_PUB_KEY"
    chmod 644 "$SB_PUB_KEY"
  fi
fi

# ── Flash Encryption — XTS-AES-256 key (always on disk; YubiKey can't help) ─
FE_KEY="$KEYDIR/flash_encryption_key.bin"
if [ ! -f "$FE_KEY" ]; then
  echo "→ Generating Flash Encryption key (XTS-AES-256)…"
  espsecure.py generate_flash_encryption_key --keylen 256 "$FE_KEY"
  chmod 600 "$FE_KEY"
else
  echo "✓ Flash Encryption key already exists: $FE_KEY"
fi

# ── NVS encryption keys (always on disk for same reason) ─────────────────
NVS_KEYS="$KEYDIR/nvs_keys.bin"
if [ ! -f "$NVS_KEYS" ]; then
  echo "→ Generating NVS encryption keys…"
  nvs_partition_gen.py generate-key --keyfile "$NVS_KEYS"
  chmod 600 "$NVS_KEYS"
else
  echo "✓ NVS keys already exist: $NVS_KEYS"
fi

# ── Symlink into firmware/keys (gitignored) ──────────────────────────────
ln -sfn "$KEYDIR" "${FIRMWARE_DIR}/keys"

echo
echo "Keys ready in ${KEYDIR}"
echo "Symlinked at ${FIRMWARE_DIR}/keys"
echo

if [ "$MODE" = "yubikey" ]; then
  cat <<EOF
Next (YubiKey signing):
  idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.production;sdkconfig.defaults.production_yubikey" \\
         -B build-prod build
  firmware/tools/sign_with_yubikey.sh build-prod
  idf.py -B build-prod flash monitor
EOF
else
  cat <<EOF
Next (local signing):
  idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.production" \\
         -B build-prod build
  idf.py -B build-prod flash monitor
EOF
fi

echo
echo "WARNING: first 'idf.py -B build-prod flash' burns eFuses irreversibly."
echo "         Read README.md → 'Production hardening' first."
