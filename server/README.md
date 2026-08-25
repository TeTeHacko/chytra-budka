# Chytra budka — server stack

Self-contained docker-compose stack for running and managing the birdhouses
**independently of Home Assistant**: MQTT broker (mTLS), OTA artifact server,
certificate enrollment, management web UI, photo archive, vision watcher and
notifications. Designed to run on the public internet — devices sit behind NAT
and initiate every connection (MQTT, OTA pulls, audio/video push), so no VPN or
inbound route to the devices is needed.

Status: **deployed — the author's fleet runs against this stack** (edge +
broker + media/metrics containers + manager with MQTT consumer, device
registry, photo archiver, REST/WS API and the Preact web UI; fleet migrated
2026-07). Next: device-push streams and the vision watcher.

Dev tip: `scripts/fake-device.py` simulates a firmware device against the stack
broker (retained discovery/state, config echoes with silent-reject keys, photo
pairs) — the UI at `https://budka.<dom>` comes alive without any hardware.

![Device grid of the management UI: six boards with camera thumbnails, RSSI, temperature and firmware version](../images/manager-ui.jpg)
*The device grid — live boards with last capture, signal, power profile and
firmware version. (`cb-ffff99` is `scripts/fake-device.py`; the camera
thumbnails were swapped for CC0 kittens before publishing — the real boxes
point into the author's flat.)*

## Layout

| Piece | Purpose |
|---|---|
| `nginx/` | TLS edge: `budka.<dom>` (UI/API/ingest/audio), `ota.<dom>` (static firmware + Basic auth), ACME webroot, cert-mtime auto-reload |
| `mosquitto/` | Broker: `8883` device mTLS (cert CN = identity, pattern ACLs), `8884` ops TLS+password, `1883` internal service accounts. ACLs versioned here |
| `certbot` | Single ACME client for all three hostnames (HTTP-01 webroot); deploy hook copies certs into the shared volume, services reload themselves |
| `manager/` | FastAPI: HTTPS enrollment (TOFU approvals + key continuity, signs with the sub-CA, bare-CN leaves with serverAuth+clientAuth), OTA upload API with atomic publish + per-version archive. SQLAlchemy async + Alembic (`CB_DB_URL`: SQLite default, MariaDB/PG supported). Approvals: `docker compose exec manager python -m budka_manager.cli pending\|approve\|deny` |
| `relay/`, `cbprom/` | Stack configs for the audio relay and the MQTT→Prometheus bridge. **Sources still live in `../relay/` and `../metrics-bridge/`** (compose builds from there) so the legacy systemd/Ansible deployments stay redeployable during the migration window; the planned `git mv` into `server/` happens at decommission |
| `mediamtx/` | RTSP server: relay pushes converted audio, external BirdNET-Go reads `rtsp://<host>:${RTSP_PORT}/chytra-budka`; HLS enabled for the future in-UI player |
| `toolbox/` | Helper image (openssl, mosquitto-clients, htpasswd) for init/verification scripts |
| `scripts/` | `init-secrets.sh`, `dev-pki.sh` (throwaway dev CA chain), `issue-le-cert.sh`, cert deploy/ensure hooks |

## Quickstart (local dev)

```sh
cp .env.example .env          # set BUDKA_DOMAIN (any name works locally)
./scripts/init-secrets.sh     # service passwords, OTA htpasswd, API tokens
./scripts/dev-pki.sh          # throwaway root+sub CA + test device cert
docker compose up -d --build
```

Then add `127.0.0.1 budka.<your BUDKA_DOMAIN>` to `/etc/hosts`, open
`https://budka.<dom>`, accept the self-signed placeholder certificate and log
in with the operator password from `secrets/operator_password`. To see devices
without hardware, run `scripts/fake-device.py` (see the dev tip above).

Verify the broker end to end (both commands were run against a fresh stack;
the server cert is the self-signed placeholder until Let's Encrypt runs, hence
the extracted `--cafile` + `--insecure`):

```sh
# 1) service listener up? (same check the container healthcheck runs)
docker compose exec mosquitto sh -c \
  'mosquitto_sub -p 1883 -u svc-health -P "$(cat /mosquitto/secrets/svc_health_pass)" -t "\$SYS/broker/uptime" -C 1 -W 5'

# 2) device mTLS on :8883 with the dev-pki client cert
docker compose exec nginx cat /certs/le/fullchain.pem > /tmp/budka-server-cert.pem
docker run --rm --network budka_default -v "$PWD/secrets:/s:ro" \
  -v /tmp/budka-server-cert.pem:/srv.pem:ro eclipse-mosquitto:2 \
  mosquitto_pub -h mosquitto -p 8883 --cafile /srv.pem --insecure \
  --cert /s/dev_device.pem --key /s/dev_device.key \
  -t 'cb-abc123/state/availability' -m online
```

## Production (Portainer / any docker host)

1. DNS: point `budka.`, `ota.`, `mqtt.` at the host; forward 80/443/8883 (8884
   optionally LAN-only via `MQTT_OPS_BIND`).
2. Provision `BUDKA_SECRETS_DIR` on the host: run `init-secrets.sh`
   (`OTA_USER`/`OTA_PASSWORD` env to keep the existing fleet OTA credentials),
   copy the real PKI chain: `cat root_ca.pem sub_ca_budka.pem > ca_chain.pem`.
3. `docker compose up -d`, then `./scripts/issue-le-cert.sh --staging` first,
   re-run without `--staging` once happy (LE rate limits).
4. Devices connect to `mqtt.<dom>:8883` with their enrollment cert; the ops
   listener `8884` serves operator tooling (`firmware/tools/ota_upload.sh`, an
   mqttui/mosquitto client) and optionally HA.

Secrets are plain files under `BUDKA_SECRETS_DIR` (never in git, never in
`.env`); in k8s they map 1:1 to a Secret, volumes map to PVCs, nginx maps to
Ingress + cert-manager.
