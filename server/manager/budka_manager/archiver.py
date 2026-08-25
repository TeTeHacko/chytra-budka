"""Photo archiver: joins `event/photo` (retained JSON meta) with `image/photo`
(retained raw JPEG) by **seq**, writes the archive in the exact layout
tools/timelapse.py parses, and records metadata for the gallery.

Join strategy (in order):
  1. EXIF UserComment JSON `{"seq":N,...}` embedded by firmware jpeg_stamp.c
     matched against the pending event meta's seq — the reliable path.
  2. size match (event meta carries the byte size),
  3. freshest pending meta younger than 5 s,
  4. no meta at all → hold the bytes 3 s (retained-replay cross-topic order is
     unspecified), then archive with trigger "unk".

Dedup (retained replay, firmware reconnect republish): per-device content hash,
persisted in the devices row so restarts don't re-archive the retained frame.
Deliberately NOT the seq — the firmware restarts seq at 1 on every boot, so a
seq match is not evidence of a duplicate, while a replay is byte-identical and
the hash catches it exactly. The HA package needed an input_text + 800 ms sleep
for this; owning both subscriptions makes it exact.

Filename contract (do not change — tools/timelapse.py NAME_RE):
    <root>/<mactail>/<YYYYMMDD>/<HHMMSS>_<seq>_<trigger>.jpg
"""

from __future__ import annotations

import asyncio
import hashlib
import io
import json
import logging
import os
import re
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path
from typing import TYPE_CHECKING, Any

from PIL import Image
from sqlalchemy import delete, select

from .models import Device, Photo, utcnow

if TYPE_CHECKING:
    from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker

    from .bus import Bus
    from .registry import Registry
    from .settings import Settings

log = logging.getLogger("budka.archiver")

TRIGGER_RE = re.compile(r"[^a-z]")
NAME_RE = re.compile(r"^(\d{2})(\d{2})(\d{2})_(\d+)_([a-z]+)\.jpg$")
EXIF_IFD = 0x8769
USER_COMMENT = 0x9286
META_MAX_AGE_S = 300.0
META_FRESH_S = 5.0
HOLD_BYTES_S = 3.0


@dataclass
class _Pending:
    meta: dict[str, Any]
    ts: float = field(default_factory=time.monotonic)


class Archiver:
    def __init__(
        self,
        settings: Settings,
        bus: Bus,
        registry: Registry,
        sessions: async_sessionmaker[AsyncSession],
    ) -> None:
        self.settings = settings
        self.bus = bus
        self.registry = registry
        self.sessions = sessions
        self._pending_meta: dict[str, _Pending] = {}
        self._held_bytes: dict[str, tuple[bytes, asyncio.TimerHandle]] = {}
        self._last: dict[str, tuple[int | None, str | None]] = {}  # dev -> (seq, sha)
        self._lock = asyncio.Lock()

    # --- MQTT entry points ---

    async def on_event_photo(self, device_id: str, payload: bytes) -> None:
        try:
            meta = json.loads(payload)
        except ValueError:
            log.warning("[%s] unparseable event/photo", device_id)
            return
        if not isinstance(meta, dict):
            return
        async with self._lock:
            self._pending_meta[device_id] = _Pending(meta)
            held = self._held_bytes.pop(device_id, None)
        if held is not None:
            held[1].cancel()
            await self.on_image_photo(device_id, held[0])

    async def on_image_photo(self, device_id: str, data: bytes) -> None:
        if not data:
            return
        exif_meta = _exif_usercomment(data)
        seq = exif_meta.get("seq") if exif_meta else None

        async with self._lock:
            pending = self._pending_meta.get(device_id)
            if pending is not None and time.monotonic() - pending.ts > META_MAX_AGE_S:
                self._pending_meta.pop(device_id, None)
                pending = None

            meta: dict[str, Any] | None = None
            if pending is not None:
                pm = pending.meta
                if (
                    (seq is not None and pm.get("seq") == seq)
                    or pm.get("size") == len(data)
                    or time.monotonic() - pending.ts < META_FRESH_S
                ):
                    meta = pm
                if meta is not None:
                    self._pending_meta.pop(device_id, None)

            if meta is None and seq is None:
                # No way to label this frame yet — hold briefly for the meta.
                old = self._held_bytes.pop(device_id, None)
                if old is not None:
                    old[1].cancel()
                loop = asyncio.get_running_loop()
                handle = loop.call_later(
                    HOLD_BYTES_S,
                    lambda: asyncio.ensure_future(self._flush_held(device_id)),
                )
                self._held_bytes[device_id] = (data, handle)
                return

        await self._archive(device_id, data, meta, seq)

    async def _flush_held(self, device_id: str) -> None:
        async with self._lock:
            held = self._held_bytes.pop(device_id, None)
        if held is not None:
            await self._archive(device_id, held[0], None, None)

    # --- the write path ---

    async def _archive(
        self, device_id: str, data: bytes, meta: dict[str, Any] | None, exif_seq: int | None
    ) -> None:
        seq = None
        if meta is not None and isinstance(meta.get("seq"), int):
            seq = meta["seq"]
        elif exif_seq is not None:
            seq = exif_seq

        sha = hashlib.sha256(data).hexdigest()
        _last_seq, last_sha = await self._last_for(device_id)
        # Content hash only. A retained replay is byte-identical, so the hash
        # catches it; matching on seq as well used to discard genuinely new
        # photos, because the firmware restarts seq at 1 on every boot. A board
        # whose last archived photo was seq=1 (it rebooted, shot one frame,
        # rebooted again) had every following shot silently dropped — observed
        # on cb-ex03 during the broker migration, which reboots twice.
        if sha == last_sha:
            return  # retained replay / reconnect republish

        trigger = "unk"
        if meta is not None:
            trigger = TRIGGER_RE.sub("", str(meta.get("trigger", "")).lower()) or "unk"

        now = datetime.now()  # local time — the filename contract is local
        day = now.strftime("%Y%m%d")
        mactail = device_id.removeprefix("cb-")
        rel = Path(mactail) / day / f"{now:%H%M%S}_{seq if seq is not None else 0}_{trigger}.jpg"
        path = self.settings.archive_root / rel

        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".jpg.part")
        with tmp.open("wb") as f:
            f.write(data)  # verbatim — EXIF passes through untouched
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)

        photo_meta = {
            "seq": seq,
            "trigger": trigger,
            "size": len(data),
            "cap": (meta or {}).get("cap"),
            "ts": now.isoformat(timespec="seconds"),
            "url": f"/api/photos/{rel.as_posix()}",
        }
        async with self.sessions() as session:
            device = await session.get(Device, device_id)
            if device is None:
                device = Device(device_id=device_id, approved=False)
                session.add(device)
            device.last_seen = utcnow()
            device.last_photo_seq = seq
            device.last_photo_sha = sha
            session.add(
                Photo(
                    device_id=device_id,
                    seq=seq,
                    day=day,
                    rel_path=rel.as_posix(),
                    size=len(data),
                    trigger=trigger,
                    meta_json=json.dumps(meta or {}),
                )
            )
            await session.commit()

        self._last[device_id] = (seq, sha)
        self.registry.device(device_id).latest_photo = photo_meta
        self.bus.publish("photo", id=device_id, meta=photo_meta)
        log.info("[%s] archived %s (%d B)", device_id, rel, len(data))

    async def _last_for(self, device_id: str) -> tuple[int | None, str | None]:
        if device_id in self._last:
            return self._last[device_id]
        async with self.sessions() as session:
            device = await session.get(Device, device_id)
        pair = (device.last_photo_seq, device.last_photo_sha) if device else (None, None)
        self._last[device_id] = pair
        return pair

    # --- maintenance tasks ---

    async def startup_scan(self) -> None:
        """Index files present on disk but missing from the DB (e.g. a volume
        migrated from the HA archive)."""
        root = self.settings.archive_root
        if not root.is_dir():
            return
        async with self.sessions() as session:
            known = {row for row in (await session.scalars(select(Photo.rel_path))).all()}
            added = 0
            for f in sorted(root.glob("*/*/*.jpg")):
                rel = f.relative_to(root).as_posix()
                if rel in known:
                    continue
                m = NAME_RE.match(f.name)
                if m is None or not re.match(r"^\d{8}$", f.parent.name):
                    continue
                session.add(
                    Photo(
                        device_id=f"cb-{f.parent.parent.name}",
                        seq=int(m.group(4)),
                        day=f.parent.name,
                        rel_path=rel,
                        size=f.stat().st_size,
                        trigger=m.group(5),
                        meta_json="{}",
                    )
                )
                added += 1
            if added:
                await session.commit()
                log.info("startup scan indexed %d existing photos", added)

    async def retention_loop(self) -> None:
        """Nightly prune at 03:30 (same schedule the HA package used)."""
        while True:
            now = datetime.now()
            nxt = now.replace(hour=3, minute=30, second=0, microsecond=0)
            if nxt <= now:
                nxt += timedelta(days=1)
            await asyncio.sleep((nxt - now).total_seconds())
            try:
                await self.prune()
            except Exception:
                log.exception("retention prune failed")

    async def prune(self) -> None:
        cutoff = time.time() - self.settings.retention_days * 86400
        cutoff_day = datetime.fromtimestamp(cutoff).strftime("%Y%m%d")
        root = self.settings.archive_root
        removed = 0
        for f in root.glob("*/*/*.jpg"):
            if f.stat().st_mtime < cutoff:
                f.unlink(missing_ok=True)
                removed += 1
        for d in root.glob("*/*"):
            if d.is_dir() and not any(d.iterdir()):
                d.rmdir()
        async with self.sessions() as session:
            await session.execute(delete(Photo).where(Photo.day < cutoff_day))
            await session.commit()
        log.info(
            "retention prune: removed %d files older than %d days",
            removed,
            self.settings.retention_days,
        )


def _exif_usercomment(data: bytes) -> dict[str, Any] | None:
    """Extract the JSON the firmware embeds in EXIF UserComment
    (jpeg_stamp.c: b'ASCII\\0\\0\\0' + JSON)."""
    try:
        img = Image.open(io.BytesIO(data))
        exif = img.getexif()
        comment = exif.get_ifd(EXIF_IFD).get(USER_COMMENT)
    except Exception:
        return None
    if not comment:
        return None
    if isinstance(comment, bytes):
        comment = comment[8:] if len(comment) > 8 else comment
        try:
            comment = comment.decode("ascii", errors="replace")
        except Exception:
            return None
    try:
        v = json.loads(str(comment).strip("\x00"))
        return v if isinstance(v, dict) else None
    except ValueError:
        return None
