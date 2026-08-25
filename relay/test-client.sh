#!/usr/bin/env bash
# test-client.sh — simulate ESP32 audio stream against relay using ALSA mic
#
# Usage:  ./test-client.sh [stream_name] [relay_url]
#
# Requires:
#   - ffmpeg with alsa input
#   - curl
#   - RELAY_AUTH_TOKEN env var (or sourced from ~/.config/chytra-budka/relay-token)

set -euo pipefail

STREAM="${1:-test-stream}"
RELAY_URL="${2:-http://localhost:8765}"

if [[ -z "${RELAY_AUTH_TOKEN:-}" ]] && [[ -f ~/.config/chytra-budka/relay-token ]]; then
  RELAY_AUTH_TOKEN="$(cat ~/.config/chytra-budka/relay-token)"
fi

if [[ -z "${RELAY_AUTH_TOKEN:-}" ]]; then
  echo "RELAY_AUTH_TOKEN not set" >&2
  exit 1
fi

# Pick first available capture device or override via $ALSA_DEV
ALSA_DEV="${ALSA_DEV:-default}"

echo "→ streaming from $ALSA_DEV to $RELAY_URL/audio/$STREAM"
echo "  Ctrl-C to stop"

ffmpeg -hide_banner -loglevel warning \
  -f alsa -ar 48000 -ac 1 -i "$ALSA_DEV" \
  -c:a pcm_s16le -f s16le - 2>/dev/null \
  | curl -X POST \
    -H "Authorization: Bearer $RELAY_AUTH_TOKEN" \
    -H "Content-Type: audio/L16; rate=48000; channels=1" \
    -H "Transfer-Encoding: chunked" \
    --data-binary @- \
    -N \
    "$RELAY_URL/audio/$STREAM"
