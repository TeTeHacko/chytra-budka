#!/bin/sh
# Wrapper around the stock eclipse-mosquitto entrypoint: adds a background
# watcher that SIGHUPs the broker when the deployed TLS cert changes (mosquitto
# 2.x reloads listener certificates on SIGHUP). PID 1 is mosquitto after exec.
#
# CERT path is overridable so the prod/behind-HAProxy deploy can watch its
# internal sub-CA broker cert (/mosquitto/pki/broker.pem) instead of the
# self-contained stack's Let's Encrypt cert. Default keeps base behaviour.
CERT="${MOSQ_WATCH_CERT:-/certs/le/fullchain.pem}"

watch_certs() {
  last="$(md5sum "$CERT" 2>/dev/null || true)"
  while :; do
    sleep 60
    cur="$(md5sum "$CERT" 2>/dev/null || true)"
    if [ -n "$cur" ] && [ "$cur" != "$last" ]; then
      echo "cert-reload: $CERT changed, sending SIGHUP to mosquitto"
      kill -HUP 1 || true
      last="$cur"
    fi
  done
}

watch_certs &
exec /docker-entrypoint.sh mosquitto -c /mosquitto/config/mosquitto.conf
