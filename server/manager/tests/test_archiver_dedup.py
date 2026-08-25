"""Archiver dedup regression tests — exercising the real `_archive` write path.

Dedup must key on the JPEG bytes, never on the firmware's `seq`: seq restarts
at 1 on every boot, so a seq match is not evidence of a duplicate. A board that
rebooted, shot one frame (seq=1) and rebooted again had every later frame
silently dropped — observed on cb-ex03 during the broker migration, which
reboots the board twice.
"""

from __future__ import annotations

import types
from pathlib import Path

import pytest
from budka_manager.archiver import Archiver


class _Session:
    """Enough of an AsyncSession for _archive: it only gets/adds/commits."""

    def __init__(self, store: dict) -> None:
        self._store = store

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc) -> bool:
        return False

    async def get(self, model, pk):
        return self._store.get(pk)

    def add(self, obj) -> None:
        pk = getattr(obj, "device_id", None)
        if type(obj).__name__ == "Device" and pk:
            self._store[pk] = obj

    async def commit(self) -> None: ...


@pytest.fixture
def archiver(tmp_path: Path):
    """A real Archiver with only persistence/fan-out stubbed out."""
    a = Archiver.__new__(Archiver)
    a.settings = types.SimpleNamespace(archive_root=tmp_path)  # type: ignore[assignment]
    a._last = {}
    devices: dict = {}
    a.sessions = lambda: _Session(devices)  # type: ignore[assignment]
    a.registry = types.SimpleNamespace(  # type: ignore[assignment]
        device=lambda _id: types.SimpleNamespace(latest_photo=None)
    )
    a.bus = types.SimpleNamespace(publish=lambda *args, **kw: None)  # type: ignore[assignment]
    return a


def _shots(root: Path, device_id: str) -> list[str]:
    return sorted(p.name for p in (root / device_id.removeprefix("cb-")).rglob("*.jpg"))


@pytest.fixture
def clock(monkeypatch: pytest.MonkeyPatch):
    """Advance the archiver's wall clock between shots.

    The filename carries HHMMSS, so two frames written inside the same second
    with the same seq would land on the same name. Real reboots are minutes
    apart; step the clock so the test reflects that rather than a collision
    that cannot occur in the field.
    """
    from datetime import datetime, timedelta

    import budka_manager.archiver as mod

    state = {"now": datetime(2026, 7, 27, 21, 30, 0)}

    class _DT(datetime):
        @classmethod
        def now(cls, tz=None):
            return state["now"]

    monkeypatch.setattr(mod, "datetime", _DT)
    return lambda minutes: state.__setitem__("now", state["now"] + timedelta(minutes=minutes))


async def test_same_seq_different_bytes_is_kept(archiver, clock, tmp_path: Path) -> None:
    """The regression: seq resets to 1 on reboot, but the frame is new."""
    await archiver._archive("cb-ex03", b"first-frame", {"trigger": "pir"}, 1)
    clock(63)  # board rebooted an hour later; seq starts over at 1
    await archiver._archive("cb-ex03", b"second-frame", {"trigger": "pir"}, 1)
    assert len(_shots(tmp_path, "cb-ex03")) == 2


async def test_identical_bytes_are_deduped(archiver, tmp_path: Path) -> None:
    """A retained replay is byte-identical and must not be re-archived."""
    await archiver._archive("cb-ex03", b"same-frame", {"trigger": "pir"}, 7)
    await archiver._archive("cb-ex03", b"same-frame", {"trigger": "pir"}, 7)
    assert len(_shots(tmp_path, "cb-ex03")) == 1


async def test_seq_going_backwards_is_kept(archiver, tmp_path: Path) -> None:
    """Post-reboot seq 27 → 1 is a new photo, not a duplicate."""
    await archiver._archive("cb-ex05", b"before-reboot", {"trigger": "vad"}, 27)
    await archiver._archive("cb-ex05", b"after-reboot", {"trigger": "vad"}, 1)
    assert len(_shots(tmp_path, "cb-ex05")) == 2


async def test_dedup_is_per_device(archiver, tmp_path: Path) -> None:
    """Two boards may legitimately produce identical bytes."""
    await archiver._archive("cb-ex03", b"frame", {"trigger": "pir"}, 1)
    await archiver._archive("cb-ex04", b"frame", {"trigger": "pir"}, 1)
    assert len(_shots(tmp_path, "cb-ex03")) == 1
    assert len(_shots(tmp_path, "cb-ex04")) == 1


async def test_filename_contract(archiver, tmp_path: Path) -> None:
    """tools/timelapse.py NAME_RE: HHMMSS_<seq>_<trigger>.jpg under <tail>/<day>."""
    import re

    await archiver._archive("cb-ex03", b"frame", {"trigger": "pir"}, 3)
    day_dir = next((tmp_path / "ex03").iterdir())
    assert re.fullmatch(r"\d{8}", day_dir.name)
    name = next(day_dir.iterdir()).name
    assert re.fullmatch(r"(\d{2})(\d{2})(\d{2})_3_pir\.jpg", name), name
