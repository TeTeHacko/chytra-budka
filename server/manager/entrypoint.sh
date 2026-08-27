#!/bin/sh
# Migrate, then serve. Single process, single replica (see server/README.md).
set -eu
alembic upgrade head
exec uvicorn budka_manager.main:app --host 0.0.0.0 --port 8080 --no-access-log
