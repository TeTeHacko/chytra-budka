#!/bin/sh
# First-time Let's Encrypt issuance for budka.<dom> + ota.<dom> + mqtt.<dom>
# (single cert, 3 SANs, HTTP-01 via the nginx webroot). Renewals are handled
# by the certbot service loop; this only needs to run once per deployment.
#
# Usage: ./scripts/issue-le-cert.sh [--staging]
# Requires: stack up (nginx serving :80), DNS for all 3 names pointing here,
# BUDKA_DOMAIN and LE_EMAIL set in .env.
set -eu

cd "$(dirname "$0")/.."
[ -f .env ] && {
  set -a
  # shellcheck disable=SC1091  # optional operator overrides, not in the repo
  . ./.env
  set +a
}

: "${BUDKA_DOMAIN:?BUDKA_DOMAIN not set (in .env)}"
: "${LE_EMAIL:?LE_EMAIL not set (in .env)}"

STAGING=""
[ "${1:-}" = "--staging" ] && STAGING="--staging"

docker compose run --rm certbot certonly \
  --webroot -w /srv/acme \
  --cert-name budka \
  -d "budka.$BUDKA_DOMAIN" -d "ota.$BUDKA_DOMAIN" -d "mqtt.$BUDKA_DOMAIN" \
  --email "$LE_EMAIL" --agree-tos --no-eff-email \
  --deploy-hook /scripts/deploy-certs.sh \
  ${STAGING:+"$STAGING"}

echo "issued. nginx and mosquitto will pick the cert up within ~60 s (mtime watch)."
