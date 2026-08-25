#!/usr/bin/env bash
# tools/setup_signing_vault.sh — one-shot setup of the YubiKey-gated age vault
# that holds the ESP32-S3 RSA-3072 firmware signing key.
#
# Run this ONCE (you, not an agent — it needs your PINs and a physical touch).
# It is safe to re-run: it refuses to clobber an existing vault unless --force.
#
# What it does:
#   1. Installs `age` + `age-plugin-yubikey` if missing (sudo pacman).
#   2. Resets the PIV applet on the signing YubiKey (5.1.2, serial below) to
#      clear the burned PIN — PIV holds nothing we need.
#   3. Generates a PIV-backed age identity with touch-policy=always
#      (every signature needs a physical tap).
#   4. Encrypts the signing key to that YubiKey   -> cb_…key.pem.age   (daily)
#      and to a recovery passphrase you choose    -> cb_…key.recovery.age (offline break-glass).
#   5. Round-trip verifies BOTH before shredding the plaintext key.
#   6. Reminds you to change the default PIV PIN/PUK and stash the recovery copy.
#
# After this, sign with: firmware/tools/sign_with_yubikey.sh
#
# NOTE on key custody: the plaintext key briefly exists only in RAM during this
# run; the only at-rest copies afterward are the two .age files. Keep the
# recovery .age + its passphrase OFFLINE — they are the "every token died" leg.

set -euo pipefail

# --- config -------------------------------------------------------------------
SIGNING_SERIAL="${CB_SIGNING_SERIAL:-00000000}" # the 5.1.2 NFC signing token
TOUCH_POLICY="${CB_TOUCH_POLICY:-always}"       # tap required every decrypt
PIN_POLICY="${CB_PIN_POLICY:-once}"             # PIN once per session
SLOT="${CB_AGE_SLOT:-1}"                        # PIV retired slot 1 (0x82)

KEYDIR="${HOME}/.config/chytra-budka/keys"
PEM="$KEYDIR/cb_secure_boot_signing_key.pem"
KEY_AGE="$KEYDIR/cb_secure_boot_signing_key.pem.age"
KEY_RECOVERY="$KEYDIR/cb_secure_boot_signing_key.recovery.age"
IDENTITY="$KEYDIR/cb_age_yubikey_identity.txt"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

die() {
  echo "ERROR: $*" >&2
  exit 1
}
hr() { printf '%s\n' "------------------------------------------------------------"; }

umask 077

# --- preconditions ------------------------------------------------------------
[ -f "$PEM" ] || die "plaintext signing key not found at $PEM (generate it first)."
if [ -f "$KEY_AGE" ] && [ "$FORCE" -ne 1 ]; then
  die "vault already exists at $KEY_AGE — re-run with --force to rebuild it."
fi

# 1. tooling
need_pkgs=()
command -v age >/dev/null 2>&1 || need_pkgs+=(age)
command -v age-plugin-yubikey >/dev/null 2>&1 || need_pkgs+=(age-plugin-yubikey)
if [ "${#need_pkgs[@]}" -gt 0 ]; then
  echo "Installing: ${need_pkgs[*]} (sudo)"
  sudo pacman -S --needed "${need_pkgs[@]}"
fi
command -v ykman >/dev/null 2>&1 || die "ykman not found (yubikey-manager)."

# verify the signing token is the one connected
hr
echo "Connected YubiKeys:"
ykman list || true
hr
ykman list 2>/dev/null | grep -q "Serial: ${SIGNING_SERIAL}\b" \
  || die "signing token (serial ${SIGNING_SERIAL}) not connected. Plug ONLY it in."

# 2. reset PIV (clears the burned PIN; PIV holds nothing we use)
echo
echo ">>> About to RESET the PIV applet on YubiKey ${SIGNING_SERIAL}."
echo "    This wipes PIV keys/certs and restores default PIN(123456)/PUK(12345678)."
echo "    (Your FIDO/OpenPGP/OTP applets are untouched.)"
read -r -p "    Type RESET to continue: " confirm
[ "$confirm" = "RESET" ] || die "aborted."
ykman --device "$SIGNING_SERIAL" piv reset --force

# 3. generate the PIV-backed age identity (touch every decrypt)
echo
echo ">>> Generating age identity on PIV slot ${SLOT} (touch-policy=${TOUCH_POLICY})."
echo "    Use the default PIN 123456 / default management key when prompted;"
echo "    you'll lock them down in step 6."
# tee so any prompt/recipient the plugin prints stays visible; pipefail (set
# above) still surfaces a generate failure through the pipe.
age-plugin-yubikey --generate \
  --serial "$SIGNING_SERIAL" \
  --slot "$SLOT" \
  --touch-policy "$TOUCH_POLICY" \
  --pin-policy "$PIN_POLICY" \
  --name "chytra-budka fw-signing" \
  | tee "$IDENTITY"
chmod 600 "$IDENTITY"

RECIPIENT="$(grep -oE 'age1yubikey1[0-9a-z]+' "$IDENTITY" | head -1)"
[ -n "$RECIPIENT" ] || die "could not parse recipient from $IDENTITY."
echo "    Recipient: $RECIPIENT"

# 4a. encrypt to the YubiKey (daily driver)
echo
echo ">>> Encrypting signing key to the YubiKey recipient -> $KEY_AGE"
age -r "$RECIPIENT" -o "$KEY_AGE" "$PEM"

# 4b. encrypt to a recovery passphrase (offline break-glass)
echo
echo ">>> Now choose a STRONG recovery passphrase (Diceware-style)."
echo "    This is the ONLY way to recover the key if every YubiKey dies."
age -p -o "$KEY_RECOVERY" "$PEM"

# 5. round-trip verify BOTH before shredding the plaintext
echo
echo ">>> Verifying YubiKey copy (touch when it blinks)…"
age -d -i "$IDENTITY" "$KEY_AGE" | cmp -s - "$PEM" \
  || die "YubiKey round-trip FAILED — NOT shredding plaintext. Investigate."
echo "    YubiKey copy OK."

echo ">>> Verifying recovery copy (re-enter the recovery passphrase)…"
age -d "$KEY_RECOVERY" | cmp -s - "$PEM" \
  || die "recovery round-trip FAILED — NOT shredding plaintext. Investigate."
echo "    Recovery copy OK."

# 5b. shred the plaintext now that both copies verified
shred -u "$PEM" 2>/dev/null || rm -f "$PEM"
echo
echo "✓ Plaintext key shredded. At-rest copies: $KEY_AGE (YubiKey) + $KEY_RECOVERY (passphrase)."

# 6. lockdown reminders
hr
cat <<EOF
DONE. Two things to finish by hand (your PINs — I never see them):

  1. Change the default PIV PIN + PUK on the signing token:
       ykman --device ${SIGNING_SERIAL} piv access change-pin     # old 123456
       ykman --device ${SIGNING_SERIAL} piv access change-puk     # old 12345678
     (optional, recommended) protect a random management key under the PIN:
       ykman --device ${SIGNING_SERIAL} piv access change-management-key --generate --protect

  2. Move the recovery copy OFFLINE:
       $KEY_RECOVERY  + its passphrase  -> USB/paper in a safe (separate places).
     Then it is fine to keep only $KEY_AGE on this workstation.

Sign firmware with:  firmware/tools/sign_with_yubikey.sh
EOF
hr
