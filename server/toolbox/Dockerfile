# Helper image for the budka stack: cert generation (openssl), MQTT test
# clients (mosquitto-clients), htpasswd (apache2-utils). Used by the certs-init
# one-shot service and by scripts/ (init-secrets.sh, dev-pki.sh, verification).
FROM alpine:3.21
RUN apk add --no-cache openssl mosquitto-clients apache2-utils
