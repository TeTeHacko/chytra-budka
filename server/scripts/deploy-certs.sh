#!/bin/sh
# certbot --deploy-hook: copy the freshly issued/renewed cert into the shared
# /certs volume where nginx and mosquitto expect it. Both services watch the
# file mtime and reload themselves (nginx -s reload / SIGHUP) — no docker.sock
# access needed anywhere.
set -eu

LIVE=/etc/letsencrypt/live/budka
DIR=/certs/le

if [ ! -s "$LIVE/fullchain.pem" ]; then
  echo "deploy-certs: $LIVE/fullchain.pem missing — nothing deployed" >&2
  exit 1
fi

mkdir -p "$DIR"
cp -L "$LIVE/fullchain.pem" "$DIR/fullchain.pem.new"
cp -L "$LIVE/privkey.pem" "$DIR/privkey.pem.new"
chown 0:1883 "$DIR/fullchain.pem.new" "$DIR/privkey.pem.new"
chmod 644 "$DIR/fullchain.pem.new"
chmod 640 "$DIR/privkey.pem.new"
mv -f "$DIR/privkey.pem.new" "$DIR/privkey.pem"
mv -f "$DIR/fullchain.pem.new" "$DIR/fullchain.pem"
echo "deploy-certs: deployed $(openssl x509 -in "$DIR/fullchain.pem" -noout -enddate 2>/dev/null || echo cert)"
