#!/usr/bin/env bash
# server/scripts/issue-client-cert.sh <cn> [days]
#
# Issue an mTLS CLIENT certificate off the budka sub-CA for something that is
# not a device — today that means the HIL runner, which needs to talk to the
# fleet broker on 8883 (the only broker port that is exposed) without opening
# anything new and without borrowing a device's identity.
#
# Devices get their certificates through enrollment; this bypasses that on
# purpose, because /api/v1/enroll only mints CNs shaped like cb-<6hex> and the
# runner is not a device. Same reason provision-prod-pki.sh signs the broker
# cert directly.
#
#   CN          = <cn>, bare — mosquitto's use_identity_as_username turns it
#                 into the MQTT username the ACL matches on.
#   SAN         = <cn>.lan — the sub-CA's nameConstraints permit .lan, and a
#                 client cert with no dNSName at all trips up verifiers that
#                 fall back to checking the CN against the constraint.
#   EKU         = clientAuth only. Nothing here should be able to impersonate
#                 a server.
#
# The ACL still decides what the identity may touch: give it the narrowest
# scope that works (see server/mosquitto/acl-devices.conf).
#
# Outputs into $BUDKA_SECRETS_DIR (default ./secrets): <cn>.pem, <cn>.key
set -euo pipefail

CN="${1:?usage: issue-client-cert.sh <cn> [days]}"
DAYS="${2:-825}"
SECRETS="${BUDKA_SECRETS_DIR:-$(cd "$(dirname "$0")/.." && pwd)/secrets}"

[ -s "$SECRETS/sub_ca_budka.pem" ] || {
  echo "MISSING $SECRETS/sub_ca_budka.pem" >&2
  exit 1
}
[ -s "$SECRETS/sub_ca_budka.key" ] || {
  echo "MISSING $SECRETS/sub_ca_budka.key" >&2
  exit 1
}

case "$CN" in
  cb-*)
    echo "ERROR: '$CN' looks like a device id — those enroll, they are not issued here." >&2
    exit 2
    ;;
  *) ;;
esac

out_crt="$SECRETS/${CN}.pem"
out_key="$SECRETS/${CN}.key"
if [ -s "$out_crt" ] && [ -s "$out_key" ]; then
  echo "= ${out_crt} already exists (delete it to re-issue)"
  openssl x509 -in "$out_crt" -noout -subject -enddate
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cat >"$tmp/ext" <<EOF
basicConstraints = critical, CA:FALSE
keyUsage         = critical, digitalSignature, keyEncipherment
extendedKeyUsage = critical, clientAuth
subjectAltName   = DNS:${CN}.lan
subjectKeyIdentifier = hash
EOF

openssl ecparam -name prime256v1 -genkey -noout -out "$tmp/k.pem" 2>/dev/null
openssl req -new -key "$tmp/k.pem" -subj "/CN=${CN}" -out "$tmp/csr.pem" 2>/dev/null
openssl x509 -req -in "$tmp/csr.pem" \
  -CA "$SECRETS/sub_ca_budka.pem" -CAkey "$SECRETS/sub_ca_budka.key" \
  -CAcreateserial -days "$DAYS" -sha256 \
  -extfile "$tmp/ext" -out "$tmp/crt.pem" 2>/dev/null

# Device-equivalent check: the leaf must validate against the sub-CA alone.
if ! openssl verify -partial_chain -CAfile "$SECRETS/sub_ca_budka.pem" "$tmp/crt.pem" >/dev/null 2>&1; then
  echo "ERROR: issued cert does not verify against the sub-CA" >&2
  openssl verify -partial_chain -CAfile "$SECRETS/sub_ca_budka.pem" "$tmp/crt.pem" >&2 || true
  exit 3
fi

install -m 0600 "$tmp/k.pem" "$out_key"
install -m 0644 "$tmp/crt.pem" "$out_crt"
echo "+ ${out_crt}"
openssl x509 -in "$out_crt" -noout -subject -ext extendedKeyUsage,subjectAltName -enddate \
  | sed 's/^/    /'
