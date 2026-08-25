#!/bin/sh
# Throwaway dev PKI for local stack development — mirrors the production
# hierarchy (root CA -> budka sub-CA -> device leaf) so the mTLS listener and
# the manager's enrollment signer can be exercised without the real Ansible CA.
#
# Produces in $BUDKA_SECRETS_DIR (default ./secrets):
#   dev_root_ca.pem/.key     throwaway root
#   sub_ca_budka.pem/.key    throwaway sub-CA (same filenames the manager mounts)
#   ca_chain.pem             sub + root (mosquitto cafile)
#   dev_device.pem/.key      test device client cert, CN=cb-abc123
#
# NEVER use these files in production; the real sub-CA lives in the Ansible
# vault (~/example.com/proxmox.example.com/ssl/).
set -eu

cd "$(dirname "$0")/.."
SECRETS="${BUDKA_SECRETS_DIR:-./secrets}"
mkdir -p "$SECRETS"
ABS_SECRETS="$(cd "$SECRETS" && pwd)"

if [ -s "$SECRETS/ca_chain.pem" ] && [ "${1:-}" != "--force" ]; then
  echo "dev-pki: $SECRETS/ca_chain.pem already exists (use --force to regenerate)"
  exit 0
fi

docker run --rm -v "$ABS_SECRETS:/out" -w /out "$(docker build -q ./toolbox)" sh -eu -c '
    umask 077

    # root CA
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
        -keyout dev_root_ca.key -out dev_root_ca.pem -days 3650 \
        -subj "/O=dev-pki/CN=budka dev root CA" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign"

    # sub-CA (same constraints as the production sub_ca_budka)
    openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
        -keyout sub_ca_budka.key -out sub_ca.csr \
        -subj "/O=dev-pki/CN=budka dev sub CA"
    cat > sub_ca.ext <<EOF
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign,digitalSignature
extendedKeyUsage=critical,serverAuth,clientAuth
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid
EOF
    openssl x509 -req -in sub_ca.csr -CA dev_root_ca.pem -CAkey dev_root_ca.key \
        -CAcreateserial -out sub_ca_budka.pem -days 3650 -extfile sub_ca.ext

    # test device leaf (client+server, CN = bare device id)
    openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
        -keyout dev_device.key -out dev_device.csr -subj "/CN=cb-abc123"
    cat > dev_device.ext <<EOF
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth,clientAuth
subjectAltName=DNS:cb-abc123,DNS:cb-abc123.local
EOF
    openssl x509 -req -in dev_device.csr -CA sub_ca_budka.pem -CAkey sub_ca_budka.key \
        -CAcreateserial -out dev_device.pem -days 90 -extfile dev_device.ext

    cat sub_ca_budka.pem dev_root_ca.pem > ca_chain.pem
    chmod 644 dev_root_ca.pem sub_ca_budka.pem dev_device.pem ca_chain.pem
    rm -f sub_ca.csr sub_ca.ext dev_device.csr dev_device.ext dev_root_ca.srl sub_ca_budka.srl
'

echo "dev-pki: wrote throwaway chain into $SECRETS"
echo "  test client:  mosquitto_pub --cafile <server cert CA> --cert dev_device.pem --key dev_device.key"
