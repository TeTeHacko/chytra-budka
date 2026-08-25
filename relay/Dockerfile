# Container build for the audio relay — used by server/docker-compose.yml
# (build context points here). The systemd/install.sh deployment keeps working
# from this same directory until the stack migration decommissions it; the
# planned `git mv relay/ server/relay/` happens at that point, not before.
FROM python:3.13-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir "aiohttp>=3.9,<4" "prometheus-client>=0.20,<1"

WORKDIR /app
COPY relay.py ./

# RELAY_AUTH_TOKEN comes from a mounted secret file, not an env value.
COPY docker-entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 8765
HEALTHCHECK --interval=30s --timeout=5s --retries=3 CMD \
    python -c "import urllib.request,sys; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:8765/health', timeout=4).status==200 else 1)"

ENTRYPOINT ["/entrypoint.sh"]
