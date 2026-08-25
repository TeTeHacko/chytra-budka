#!/usr/bin/env bash
# tools/ota_publish.sh — publish a firmware image + manifest to the OTA server.
#
#   ota_publish.sh <bin> <manifest>
#
# One multipart POST to the manager's upload API. The server verifies the
# sha256 in the manifest against the bytes, fsyncs, and renames both files into
# place together, so a device polling mid-publish can never fetch a manifest
# that disagrees with the image beside it. (The old rsync-to-zelena path needed
# two transfers plus an ssh mv to approximate that.)
#
# Shared by ota_upload.sh and ota_rollback.sh so the endpoint, the auth and the
# failure handling exist once.
#
# Env:
#   CB_OTA_UPLOAD_URL  default https://cb.example.com/api/v1/ota/upload
#   CB_OTA_TOKEN_FILE  default <repo>/server/secrets/ota_upload_token
set -euo pipefail

BIN="${1:?usage: ota_publish.sh <bin> <manifest>}"
MANIFEST="${2:?usage: ota_publish.sh <bin> <manifest>}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
UPLOAD_URL="${CB_OTA_UPLOAD_URL:-https://cb.example.com/api/v1/ota/upload}"
TOKEN_FILE="${CB_OTA_TOKEN_FILE:-${REPO_ROOT}/server/secrets/ota_upload_token}"

for f in "$BIN" "$MANIFEST"; do
  [ -r "$f" ] || {
    echo "ERROR: cannot read ${f}" >&2
    exit 2
  }
done

if [ ! -r "$TOKEN_FILE" ]; then
  cat >&2 <<-EOF
	ERROR: no OTA upload token at ${TOKEN_FILE}.
	       It is the bearer token the manager checks (swarm secret
	       budka_ota_token_up). Point CB_OTA_TOKEN_FILE at it, or see
	       server/DEPLOY-prod.md for how it was provisioned.
	EOF
  exit 5
fi

echo "→ publishing to ${UPLOAD_URL}"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT

# The token goes in via -H @file (process substitution) rather than on the
# command line, so it never shows up in ps output.
code="$(curl -sS -o "$log" -w '%{http_code}' \
  -H @<(printf 'Authorization: Bearer %s\n' "$(cat "$TOKEN_FILE")") \
  -F "bin=@${BIN};type=application/octet-stream" \
  -F "manifest=@${MANIFEST};type=application/json" \
  "$UPLOAD_URL")" || code=000

case "$code" in
  200 | 201)
    echo "  server: $(cat "$log")"
    ;;
  *)
    echo "ERROR: upload failed (HTTP ${code})" >&2
    # Redact anything token-shaped before echoing a server error.
    sed -e 's/[A-Za-z0-9._-]\{32,\}/<redacted>/g' "$log" >&2
    exit 6
    ;;
esac
