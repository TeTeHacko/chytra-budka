# budka-manager: SPA build stage + python runtime.
FROM node:22-alpine AS web
WORKDIR /web
COPY web/package.json web/package-lock.json* ./
RUN npm install --no-audit --no-fund
COPY web/ ./
RUN npm run build

FROM python:3.13-slim

WORKDIR /app
COPY pyproject.toml ./
COPY budka_manager ./budka_manager
RUN pip install --no-cache-dir .

COPY alembic.ini ./
COPY alembic ./alembic
COPY --from=web /web/dist ./web/dist
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

VOLUME ["/data", "/ota", "/photos"]
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --retries=3 CMD \
    python -c "import urllib.request,sys; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:8080/healthz', timeout=4).status==200 else 1)"

ENTRYPOINT ["/entrypoint.sh"]
