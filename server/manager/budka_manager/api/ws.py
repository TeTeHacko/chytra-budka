"""WebSocket: pushes bus events (device patches, photos) to the SPA."""

from __future__ import annotations

import logging

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from ..auth import check_session
from ..settings import get_settings

log = logging.getLogger("budka.ws")

router = APIRouter()


@router.websocket("/api/ws")
async def ws(websocket: WebSocket):
    settings = get_settings()
    if settings.auth_mode != "off":
        sub = check_session(settings, websocket.cookies.get("budka_session"))
        if sub is None:
            await websocket.close(code=4401)
            return
    await websocket.accept()
    bus = websocket.app.state.bus
    registry = websocket.app.state.registry
    await websocket.send_json(
        {
            "t": "hello",
            "devices": sorted(registry.devices.keys()),
        }
    )
    try:
        async with bus.subscribe() as queue:
            while True:
                ev = await queue.get()
                await websocket.send_json({"t": ev.kind, **ev.data})
    except WebSocketDisconnect:
        pass
    except Exception:
        log.debug("ws closed", exc_info=True)
