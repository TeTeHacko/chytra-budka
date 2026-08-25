#!/bin/sh
# Runs from the official nginx entrypoint before nginx starts: spawns a
# background watcher that reloads nginx whenever the deployed cert changes
# (certbot deploy hook writes into the shared /certs volume). No docker.sock,
# no cron — just an mtime/content watch.
CERT=/certs/le/fullchain.pem

watch_certs() {
  last="$(md5sum "$CERT" 2>/dev/null || true)"
  while :; do
    sleep 60
    cur="$(md5sum "$CERT" 2>/dev/null || true)"
    if [ -n "$cur" ] && [ "$cur" != "$last" ]; then
      echo "cert-reload: $CERT changed, reloading nginx"
      nginx -s reload || true
      last="$cur"
    fi
  done
}

watch_certs &
