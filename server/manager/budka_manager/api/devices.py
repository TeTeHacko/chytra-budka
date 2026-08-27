"""Device list/detail, friendly names, commands, config editor endpoints."""

from __future__ import annotations

from fastapi import APIRouter, Depends, HTTPException, Request
from pydantic import BaseModel
from sqlalchemy.ext.asyncio import AsyncSession

from ..db import get_session
from ..models import Device, utcnow
from ..mqtt.client import ALLOWED_COMMANDS, EchoTimeout

router = APIRouter(prefix="/api")


def _registry(request: Request):
    return request.app.state.registry


def _mqtt(request: Request):
    return request.app.state.mqtt


@router.get("/devices")
async def list_devices(request: Request):
    registry = _registry(request)
    return [d.summary() for d in sorted(registry.devices.values(), key=lambda d: d.device_id)]


@router.get("/devices/{device_id}")
async def device_detail(device_id: str, request: Request):
    registry = _registry(request)
    dev = registry.devices.get(device_id)
    if dev is None:
        raise HTTPException(404, "unknown device")
    return {
        **dev.summary(),
        "availability": dev.availability,
        "reset_reason": dev.reset_reason,
        "fw": dev.fw,
        "ds": dev.ds,
        "selftest": dev.selftest,
        "boot": dev.boot,
        "entities": {uid: e.public() for uid, e in dev.entities.items()},
        "scalars": {k: {"value": v[0], "ts": v[1]} for k, v in dev.scalars.items()},
        "config": {k: vars(c) for k, c in dev.config_items().items()},
    }


class NamePatch(BaseModel):
    name: str | None = None
    notes: str | None = None


@router.patch("/devices/{device_id}")
async def patch_device(
    device_id: str, body: NamePatch, request: Request, session: AsyncSession = Depends(get_session)
):
    registry = _registry(request)
    if device_id not in registry.devices:
        raise HTTPException(404, "unknown device")
    device = await session.get(Device, device_id)
    if device is None:
        device = Device(device_id=device_id, approved=False)
        session.add(device)
    if body.name is not None:
        device.name = body.name or None
        registry.devices[device_id].name = device.name
    if body.notes is not None:
        device.notes = body.notes or None
    device.last_seen = utcnow()
    await session.commit()
    return {"device_id": device_id, "name": device.name, "notes": device.notes}


class CommandBody(BaseModel):
    payload: str | None = None


@router.post("/devices/{device_id}/command/{name}", status_code=202)
async def command(device_id: str, name: str, request: Request, body: CommandBody | None = None):
    if name not in ALLOWED_COMMANDS:
        raise HTTPException(422, f"unknown command {name!r}")
    registry = _registry(request)
    if device_id not in registry.devices:
        raise HTTPException(404, "unknown device")
    try:
        await _mqtt(request).command(
            device_id, name, (body.payload if body and body.payload else "")
        )
    except ConnectionError as e:
        raise HTTPException(503, str(e)) from e
    return {"sent": name}


@router.get("/devices/{device_id}/config")
async def get_config(device_id: str, request: Request):
    dev = _registry(request).devices.get(device_id)
    if dev is None:
        raise HTTPException(404, "unknown device")
    return {k: vars(c) for k, c in dev.config_items().items()}


class ConfigPut(BaseModel):
    value: str
    force: bool = False


@router.put("/devices/{device_id}/config/{key}")
async def put_config(device_id: str, key: str, body: ConfigPut, request: Request):
    dev = _registry(request).devices.get(device_id)
    if dev is None:
        raise HTTPException(404, "unknown device")
    item = dev.config_items().get(key)
    if item is None:
        raise HTTPException(404, f"unknown config key {key!r}")
    if not body.force:
        err = item.validate(body.value)
        if err is not None:
            raise HTTPException(422, detail={"error": err, "override_with": "force"})
    try:
        result = await _mqtt(request).set_cfg(device_id, key, body.value)
    except ConnectionError as e:
        raise HTTPException(503, str(e)) from e
    except EchoTimeout:
        raise HTTPException(
            504,
            detail={
                "error": "no_echo",
                "detail": "device silently rejected the value or is unreachable; value unchanged",
            },
        ) from None
    return result
