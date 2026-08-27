"""ORM models — phase 1 subset (devices, enrollment). Later phases add
photos, watches, detections, events, notifications, settings.

Portability rules: no dialect-specific types (JSON stored as TEXT), naive
UTC datetimes (SQLite has no tz), string enums as short varchars.
"""

from __future__ import annotations

from datetime import UTC, datetime

from sqlalchemy import DateTime, ForeignKey, Integer, String, Text
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column


def utcnow() -> datetime:
    return datetime.now(UTC).replace(tzinfo=None)


class Base(DeclarativeBase):
    pass


class Device(Base):
    __tablename__ = "devices"

    device_id: Mapped[str] = mapped_column(String(16), primary_key=True)  # cb-<6hex>
    name: Mapped[str | None] = mapped_column(String(64))
    notes: Mapped[str | None] = mapped_column(Text)
    approved: Mapped[bool] = mapped_column(default=False)
    # SHA-256 of the enrolled key's SubjectPublicKeyInfo — key-continuity anchor.
    pubkey_fp: Mapped[str | None] = mapped_column(String(64))
    first_seen: Mapped[datetime] = mapped_column(DateTime, default=utcnow)
    last_seen: Mapped[datetime] = mapped_column(DateTime, default=utcnow)
    # Archiver dedup anchors (retained-replay guard across restarts).
    last_photo_seq: Mapped[int | None] = mapped_column(Integer)
    last_photo_sha: Mapped[str | None] = mapped_column(String(64))


class Photo(Base):
    __tablename__ = "photos"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(16), index=True)
    seq: Mapped[int | None] = mapped_column(Integer)
    day: Mapped[str] = mapped_column(String(8), index=True)  # YYYYMMDD
    rel_path: Mapped[str] = mapped_column(String(255), unique=True)
    size: Mapped[int] = mapped_column(Integer)
    trigger: Mapped[str] = mapped_column(String(16))
    meta_json: Mapped[str] = mapped_column(Text, default="{}")
    created_at: Mapped[datetime] = mapped_column(DateTime, default=utcnow, index=True)


class EnrollRequest(Base):
    __tablename__ = "enroll_requests"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(16), ForeignKey("devices.device_id"), index=True)
    cn: Mapped[str] = mapped_column(String(255))
    pubkey_fp: Mapped[str] = mapped_column(String(64))
    csr_pem: Mapped[str] = mapped_column(Text)
    # pending | approved | denied | superseded
    status: Mapped[str] = mapped_column(String(16), default="pending", index=True)
    reason: Mapped[str | None] = mapped_column(String(64))  # new-device | rekey
    created_at: Mapped[datetime] = mapped_column(DateTime, default=utcnow)
    decided_at: Mapped[datetime | None] = mapped_column(DateTime)
    decided_by: Mapped[str | None] = mapped_column(String(64))


class EnrollLedger(Base):
    __tablename__ = "enroll_ledger"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(16), index=True)
    cn: Mapped[str] = mapped_column(String(255))
    serial_hex: Mapped[str] = mapped_column(String(40))
    not_after: Mapped[datetime] = mapped_column(DateTime)
    issued_at: Mapped[datetime] = mapped_column(DateTime, default=utcnow, index=True)
