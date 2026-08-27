"""FastAPI app factory + lifespan (MQTT consumer, archiver tasks)."""

from __future__ import annotations

import asyncio
import contextlib
import logging
from pathlib import Path

from fastapi import Depends, FastAPI
from fastapi.staticfiles import StaticFiles
from sqlalchemy import select

from .. import __version__
from ..archiver import Archiver
from ..auth import require_auth
from ..bus import Bus
from ..ca import CAConfig, load_ca
from ..db import session_factory
from ..enrollment import EnrollmentService
from ..models import Device
from ..mqtt.client import MqttService
from ..registry import Registry
from ..settings import get_settings
from . import auth_api, devices, enroll, health, ota_api, photos_api, ws

log = logging.getLogger("budka.api")

SPA_DIST = Path(__file__).resolve().parent.parent.parent / "web" / "dist"


@contextlib.asynccontextmanager
async def _lifespan(app: FastAPI):
    settings = get_settings()
    bus = Bus()
    registry = Registry()
    archiver = Archiver(settings, bus, registry, session_factory())
    mqtt = MqttService(settings, registry, bus, archiver)
    app.state.bus = bus
    app.state.registry = registry
    app.state.archiver = archiver
    app.state.mqtt = mqtt

    # Friendly names + known devices survive restarts in the DB.
    async with session_factory()() as session:
        for row in (await session.scalars(select(Device))).all():
            dev = registry.device(row.device_id)
            dev.name = row.name

    await archiver.startup_scan()
    tasks = [
        asyncio.create_task(mqtt.run(), name="mqtt"),
        asyncio.create_task(archiver.retention_loop(), name="retention"),
    ]
    try:
        yield
    finally:
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


def create_app() -> FastAPI:
    settings = get_settings()
    app = FastAPI(title="budka-manager", version=__version__, lifespan=_lifespan)

    if settings.enroll_mode == "https":
        ca_cfg = CAConfig(
            cert_path=settings.ca_cert_file,
            key_path=settings.ca_key_file,
            validity_days=settings.enroll_validity_days,
            permitted_suffixes=settings.enroll_suffix_tuple,
        )
        bundle = load_ca(ca_cfg)
        app.state.enroll_service = EnrollmentService(
            ca_cfg,
            bundle,
            max_per_hour=settings.enroll_max_per_hour,
            trusted_devices=settings.enroll_trusted_tuple,
        )
        log.info(
            "enrollment CA loaded: %s (valid until %s)",
            ca_cfg.cert_path,
            bundle.cert.not_valid_after_utc.isoformat(),
        )
    else:
        app.state.enroll_service = None
        log.warning("enrollment disabled (CB_ENROLL_MODE=%s)", settings.enroll_mode)

    if settings.auth_mode == "off":
        log.warning("operator auth is OFF — development only")

    # Unauthenticated / self-authenticating: health, device enrollment (CSR +
    # TOFU), OTA upload (bearer token), login endpoints.
    app.include_router(health.router)
    app.include_router(enroll.router)
    app.include_router(ota_api.router)
    app.include_router(auth_api.router)
    # Operator session required:
    operator = [Depends(require_auth)]
    app.include_router(devices.router, dependencies=operator)
    app.include_router(photos_api.router, dependencies=operator)
    app.include_router(ws.router)  # WS checks the session cookie itself

    if SPA_DIST.is_dir():
        app.mount("/", StaticFiles(directory=SPA_DIST, html=True), name="spa")
    return app
