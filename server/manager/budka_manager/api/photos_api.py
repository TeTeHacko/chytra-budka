"""Gallery API — served from the archive volume + photos table."""

from __future__ import annotations

import re

from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import FileResponse, Response
from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import AsyncSession

from ..db import get_session
from ..models import Photo
from ..settings import get_settings

router = APIRouter(prefix="/api")

SAFE_SEG = re.compile(r"^[A-Za-z0-9._-]+$")


@router.get("/devices/{device_id}/photos")
async def device_photos(
    device_id: str,
    day: str | None = None,
    limit: int = 100,
    before_id: int | None = None,
    session: AsyncSession = Depends(get_session),
):
    q = select(Photo).where(Photo.device_id == device_id)
    if day:
        q = q.where(Photo.day == day)
    if before_id:
        q = q.where(Photo.id < before_id)
    q = q.order_by(Photo.id.desc()).limit(min(limit, 500))
    rows = (await session.scalars(q)).all()
    days = (
        await session.scalars(
            select(Photo.day)
            .where(Photo.device_id == device_id)
            .group_by(Photo.day)
            .order_by(Photo.day.desc())
        )
    ).all()
    return {
        "days": list(days),
        "photos": [_row(p) for p in rows],
    }


@router.get("/gallery")
async def gallery(
    day: str | None = None,
    device: str | None = None,
    trigger: str | None = None,
    limit: int = 200,
    session: AsyncSession = Depends(get_session),
):
    q = select(Photo)
    if day:
        q = q.where(Photo.day == day)
    if device:
        q = q.where(Photo.device_id == device)
    if trigger:
        q = q.where(Photo.trigger == trigger)
    rows = (await session.scalars(q.order_by(Photo.id.desc()).limit(min(limit, 500)))).all()
    days = (
        await session.scalars(
            select(Photo.day).group_by(Photo.day).order_by(Photo.day.desc()).limit(60)
        )
    ).all()
    counts = {
        t: c
        for t, c in (
            await session.execute(select(Photo.trigger, func.count()).group_by(Photo.trigger))
        ).all()
    }
    return {"days": list(days), "triggers": counts, "photos": [_row(p) for p in rows]}


@router.get("/photos/{mactail}/{day}/{name}")
async def photo_file(mactail: str, day: str, name: str):
    for seg in (mactail, day, name):
        if not SAFE_SEG.match(seg) or ".." in seg:
            raise HTTPException(400, "bad path")
    path = get_settings().archive_root / mactail / day / name
    if not path.is_file():
        raise HTTPException(404, "not found")
    return FileResponse(
        path,
        media_type="image/jpeg",
        headers={
            "Cache-Control": "public, max-age=31536000, immutable",
        },
    )


@router.get("/devices/{device_id}/photo/latest")
async def latest_photo(device_id: str, session: AsyncSession = Depends(get_session)):
    row = await session.scalar(
        select(Photo).where(Photo.device_id == device_id).order_by(Photo.id.desc()).limit(1)
    )
    if row is None:
        raise HTTPException(404, "no photo archived yet")
    path = get_settings().archive_root / row.rel_path
    if not path.is_file():
        raise HTTPException(404, "file missing")
    return FileResponse(path, media_type="image/jpeg", headers={"Cache-Control": "no-store"})


def _row(p: Photo) -> dict:
    return {
        "id": p.id,
        "device_id": p.device_id,
        "seq": p.seq,
        "day": p.day,
        "trigger": p.trigger,
        "size": p.size,
        "url": f"/api/photos/{p.rel_path}",
        "created_at": p.created_at.isoformat(timespec="seconds"),
    }


@router.get("/events")
async def events_stub() -> Response:
    return Response(status_code=204)
