#!/bin/sh
# certs-init one-shot: make sure /certs/le/{fullchain,privkey}.pem exist so
# nginx and mosquitto can start before the first real Let's Encrypt issuance.
# Generates a self-signed multi-SAN placeholder; certbot's deploy hook
# (scripts/deploy-certs.sh) overwrites it with the real cert later and the
# services reload themselves on mtime change.
set -eu

DIR=/certs/le
DOM="${BUDKA_DOMAIN:?BUDKA_DOMAIN not set}"

if [ -s "$DIR/fullchain.pem" ] && [ -s "$DIR/privkey.pem" ]; then
  echo "certs-init: $DIR already populated, nothing to do"
  exit 0
fi

echo "certs-init: generating self-signed placeholder for *.$DOM"
mkdir -p "$DIR"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout "$DIR/privkey.pem" -out "$DIR/fullchain.pem" \
  -days 30 -nodes -subj "/CN=placeholder.$DOM" \
  -addext "subjectAltName=DNS:budka.$DOM,DNS:ota.$DOM,DNS:mqtt.$DOM"

# mosquitto runs as uid/gid 1883; nginx master is root.
chown 0:1883 "$DIR/privkey.pem" "$DIR/fullchain.pem"
chmod 640 "$DIR/privkey.pem"
chmod 644 "$DIR/fullchain.pem"
echo "certs-init: done"
