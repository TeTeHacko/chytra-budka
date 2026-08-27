from __future__ import annotations

from fastapi import APIRouter, Request
from sqlalchemy import text

from ..db import session_factory

router = APIRouter()


@router.get("/healthz")
async def healthz(request: Request):
    async with session_factory()() as session:
        await session.execute(text("SELECT 1"))
    return {
        "status": "ok",
        "enroll": request.app.state.enroll_service is not None,
    }
