# Container build for cbprom (MQTT → Prometheus bridge) — used by
# server/docker-compose.yml (build context points here). The Ansible/systemd
# deployment on server-host keeps working from this directory until the stack
# migration decommissions it; the planned `git mv` into server/ happens then.
# enroll.py is NOT built here — its crypto core moved into the manager
# (server/manager/budka_manager/ca.py) and the legacy MQTT signer stays a
# systemd service until the fleet migrates to HTTPS enrollment.
FROM python:3.13-slim

RUN pip install --no-cache-dir "paho-mqtt>=2.1,<3" "prometheus_client>=0.20,<1"

WORKDIR /app
COPY cbprom.py ./

EXPOSE 9878
HEALTHCHECK --interval=30s --timeout=5s --retries=3 CMD \
    python -c "import urllib.request,sys; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:9878/metrics', timeout=4).status==200 else 1)"

CMD ["python", "cbprom.py", "--config", "/etc/cb-prom/config.toml"]
