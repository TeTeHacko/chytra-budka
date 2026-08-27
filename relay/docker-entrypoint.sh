#!/bin/sh
set -eu
if [ -z "${RELAY_AUTH_TOKEN:-}" ] && [ -r /secrets/relay_token ]; then
  RELAY_AUTH_TOKEN="$(cat /secrets/relay_token)"
  export RELAY_AUTH_TOKEN
fi
exec python relay.py --config "${RELAY_CONFIG:-/etc/chytra-budka/relay.toml}"
