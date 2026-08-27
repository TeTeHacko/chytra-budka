#!/usr/bin/env bash
# Fetch Let's Encrypt ISRG Root X1 + X2 certificates and concatenate them
# into firmware/tools/le_isrg_roots.pem for the esp_crt_bundle build step.
#
# Run once after `git clone` (the resulting .pem is gitignored to avoid
# carrying CA blobs in the repo and to make rotations explicit).
set -euo pipefail

cd "$(dirname "$0")"

OUT="le_isrg_roots.pem"
URL_X1="https://letsencrypt.org/certs/isrgrootx1.pem"
URL_X2="https://letsencrypt.org/certs/isrg-root-x2.pem"

echo ">>> fetching ISRG Root X1 from $URL_X1"
curl -fsSL "$URL_X1" -o isrgrootx1.pem

echo ">>> fetching ISRG Root X2 from $URL_X2"
curl -fsSL "$URL_X2" -o isrg-root-x2.pem

cat isrgrootx1.pem isrg-root-x2.pem >"$OUT"
rm -f isrgrootx1.pem isrg-root-x2.pem

echo ">>> verify (expect 2 BEGIN CERTIFICATE blocks):"
grep -c '^-----BEGIN CERTIFICATE-----' "$OUT"

echo ">>> wrote $OUT ($(wc -c <"$OUT") bytes)"
