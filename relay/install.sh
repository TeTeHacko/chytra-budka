#!/usr/bin/env bash
# install.sh — deploy chytra-budka audio relay on server-host (or similar).
#
# Idempotent: rerun safely after pulling new relay.py.
#
# Steps:
#   1. ensure 'birdnet' system user exists
#   2. /opt/chytra-budka-relay  ← venv + relay.py + relay.toml
#   3. /etc/chytra-budka/relay.env (0600) holds RELAY_AUTH_TOKEN
#   4. install + enable systemd unit
#
# Run as root:  sudo ./install.sh

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "must run as root" >&2
  exit 1
fi

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST_DIR="/opt/chytra-budka-relay"
ETC_DIR="/etc/chytra-budka"
ENV_FILE="$ETC_DIR/relay.env"
TOML_FILE="$ETC_DIR/relay.toml"
SERVICE_FILE="/etc/systemd/system/chytra-budka-relay.service"
USER="mqtt-user"
GROUP="mqtt-user"

# 1. user
if ! id -u "$USER" >/dev/null 2>&1; then
  echo "→ creating system user $USER"
  useradd --system --no-create-home --shell /usr/sbin/nologin "$USER"
fi

# 2. venv + code
mkdir -p "$DEST_DIR"
install -m 0644 -o root -g root "$SRC_DIR/relay.py" "$DEST_DIR/relay.py"
install -m 0644 -o root -g root "$SRC_DIR/requirements.txt" "$DEST_DIR/requirements.txt"

if [[ ! -x "$DEST_DIR/.venv/bin/python" ]]; then
  echo "→ creating venv"
  python3 -m venv "$DEST_DIR/.venv"
fi
"$DEST_DIR/.venv/bin/pip" install --quiet --upgrade pip
"$DEST_DIR/.venv/bin/pip" install --quiet -r "$DEST_DIR/requirements.txt"
chown -R "$USER:$GROUP" "$DEST_DIR/.venv"

# 3. config + token
mkdir -p "$ETC_DIR"
chmod 0750 "$ETC_DIR"
chown root:"$GROUP" "$ETC_DIR"

install -m 0640 -o root -g "$GROUP" "$SRC_DIR/relay.toml" "$TOML_FILE"

if [[ ! -f "$ENV_FILE" ]]; then
  echo "→ generating $ENV_FILE with random token (save it for the ESP32!)"
  TOKEN="$(openssl rand -hex 32)"
  umask 077
  cat >"$ENV_FILE" <<EOF
RELAY_AUTH_TOKEN=$TOKEN
EOF
  chown root:"$GROUP" "$ENV_FILE"
  chmod 0640 "$ENV_FILE"
  echo
  echo "  RELAY_AUTH_TOKEN written to $ENV_FILE"
  echo "  → put the same value into firmware/main/secrets.h as RELAY_AUTH"
  echo
else
  echo "→ $ENV_FILE already present (keeping)"
fi

# 4. systemd unit
install -m 0644 -o root -g root "$SRC_DIR/chytra-budka-relay.service" "$SERVICE_FILE"
systemctl daemon-reload
systemctl enable chytra-budka-relay.service

if systemctl is-active --quiet chytra-budka-relay.service; then
  echo "→ restarting service"
  systemctl restart chytra-budka-relay.service
else
  echo "→ starting service"
  systemctl start chytra-budka-relay.service
fi

sleep 1
systemctl --no-pager --lines=10 status chytra-budka-relay.service || true

echo
echo "done. health:  curl -fsS http://127.0.0.1:8765/health"
