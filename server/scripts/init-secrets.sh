#!/bin/sh
# One-time secrets bootstrap for the budka stack. Idempotent: existing files
# are never overwritten (delete a file to regenerate it).
#
# Produces in $BUDKA_SECRETS_DIR (default ./secrets):
#   mosquitto_passwd     hashed passwords for the service accounts
#   svc_<name>_pass      plaintext per-service password files (0600, for mounts)
#   ota_htpasswd         HTTP Basic auth for ota.<domain> (nginx auth_basic)
#   ota_token            Bearer token for POST /api/v1/ota/upload
#   ingest_token         Bearer token for POST /ingest/stream/<id>
#
# The PKI material (ca_chain.pem + sub-CA key/cert for the manager) is NOT
# generated here: use scripts/dev-pki.sh for a throwaway dev chain, or copy the
# real root+sub chain from the Ansible PKI for production.
#
# OTA credentials: set OTA_USER / OTA_PASSWORD env vars to keep an existing
# fleet contract (defaults: chytra-budka / random).
set -eu

cd "$(dirname "$0")/.."
SECRETS="${BUDKA_SECRETS_DIR:-./secrets}"
mkdir -p "$SECRETS"
chmod 700 "$SECRETS"

# mosquitto_passwd.plain below holds every service password in CLEARTEXT while
# the hashing container runs. Under `set -e` any failure in that step (image
# pull, daemon down, no docker group) aborts the script *before* the cleanup
# line, silently leaving the plaintext on disk with nothing on screen to say
# it is there. Clean it up on every exit path instead of only the happy one.
trap 'rm -f "$SECRETS/mosquitto_passwd.plain"' EXIT HUP INT TERM

rand_hex() { tr -dc 'a-f0-9' </dev/urandom | head -c "${1:-48}"; }

# --- per-service MQTT passwords ---------------------------------------------
SERVICES="manager cbprom ha ops health"
changed=0
for svc in $SERVICES; do
  f="$SECRETS/svc_${svc}_pass"
  if [ ! -s "$f" ]; then
    umask 077
    rand_hex 48 >"$f"
    echo "generated $f"
    changed=1
  fi
done

if [ ! -s "$SECRETS/mosquitto_passwd" ] || [ "$changed" = 1 ]; then
  echo "building $SECRETS/mosquitto_passwd"
  : >"$SECRETS/mosquitto_passwd.plain"
  for svc in $SERVICES; do
    printf 'svc-%s:%s\n' "$svc" "$(cat "$SECRETS/svc_${svc}_pass")" >>"$SECRETS/mosquitto_passwd.plain"
  done
  docker run --rm -v "$(cd "$SECRETS" && pwd):/out" eclipse-mosquitto:2 \
    sh -c 'cp /out/mosquitto_passwd.plain /tmp/pw && mosquitto_passwd -U /tmp/pw && cp /tmp/pw /out/mosquitto_passwd && chmod 644 /out/mosquitto_passwd'
  rm -f "$SECRETS/mosquitto_passwd.plain" # happy path; the EXIT trap covers the rest
fi

# --- OTA basic auth -----------------------------------------------------------
if [ ! -s "$SECRETS/ota_htpasswd" ]; then
  OTA_USER="${OTA_USER:-chytra-budka}"
  if [ -z "${OTA_PASSWORD:-}" ]; then
    OTA_PASSWORD="$(rand_hex 32)"
    echo "NOTE: generated random OTA password for user '$OTA_USER':"
    echo "      $OTA_PASSWORD"
    echo "      (must match OTA_PASSWORD in firmware secrets.h)"
  fi
  hash="$(docker run --rm httpd:2-alpine htpasswd -nbB "$OTA_USER" "$OTA_PASSWORD")"
  umask 077
  printf '%s\n' "$hash" >"$SECRETS/ota_htpasswd"
  chmod 644 "$SECRETS/ota_htpasswd"
  echo "generated $SECRETS/ota_htpasswd (user $OTA_USER)"
fi

# --- operator web login (CB_AUTH_MODE=password) + session signing key ---------
if [ ! -s "$SECRETS/operator_password" ]; then
  umask 077
  if [ -n "${OPERATOR_PASSWORD:-}" ]; then
    printf '%s' "$OPERATOR_PASSWORD" >"$SECRETS/operator_password"
    echo "generated $SECRETS/operator_password (from OPERATOR_PASSWORD env)"
  else
    rand_hex 32 >"$SECRETS/operator_password"
    echo "NOTE: generated random operator web password:"
    echo "      $(cat "$SECRETS/operator_password")"
  fi
fi

# --- API bearer tokens --------------------------------------------------------
# relay_token = the device-side RELAY_AUTH (firmware secrets.h) for audio POSTs.
for tok in ota_token ingest_token relay_token session_secret; do
  f="$SECRETS/$tok"
  if [ ! -s "$f" ]; then
    umask 077
    rand_hex 64 >"$f"
    echo "generated $f"
  fi
done

# --- PKI presence check -------------------------------------------------------
if [ ! -s "$SECRETS/ca_chain.pem" ]; then
  echo ""
  echo "MISSING: $SECRETS/ca_chain.pem (root + sub-CA concat, mosquitto mTLS trust)"
  echo "  dev:  ./scripts/dev-pki.sh"
  echo "  prod: cat ansible-root-ca.pem sub_ca_budka.pem > $SECRETS/ca_chain.pem"
fi

echo "done."
