"""
test_timelapse — verify the tlapse_min main-loop tick fires periodic
captures, that the trigger label survives the EXIF offset path, and
that a missed slot doesn't permanently desync the schedule.

Marked @manual because the smallest interval the schema allows is
1 minute and we wait for two consecutive shots to prove periodicity.
End-to-end ~130 s. Run explicitly:

    pytest -m manual firmware/tests/hil/test_timelapse.py

The test pins PIR + VAD off for the duration so a stray motion / audio
event can't take the capture mutex and bump our scheduled shot into a
retry — the main-loop code does retry on the next 1 Hz tick, but the
budget would still cut into the inter-shot gap. Original config is
restored in a finally block.
"""

from __future__ import annotations

import io
import json
import time

import httpx
import pytest
from PIL import Image


@pytest.mark.manual
def test_timelapse_fires_twice_with_one_minute_interval(
    mqtt_rec, cfg, bench_id: str, bench_ip: str
):
    # Snapshot the knobs we're about to flip so we can restore even on
    # assertion failure. Reading via mqtt_rec.latest() means we use the
    # retained state echo (truth on the device), not a stale yaml.
    prev = {}
    for key in ("tlapse_min", "pir_enabled", "vad_enabled"):
        msg = mqtt_rec.latest(f"{bench_id}/state/cfg/{key}")
        prev[key] = msg.decode() if msg else None

    try:
        # Quiet the board: only `cmd/photo` and timelapse can fire a
        # capture while PIR + VAD are off, so the mutex is uncontended
        # and the schedule advances on success exactly.
        cfg("pir_enabled", "OFF")
        cfg("vad_enabled", "OFF")
        cfg("tlapse_min", "1")
        engaged_at = time.time()

        # Helper: wait for the next timelapse-tagged event/photo whose
        # ts is after `since`. Returns the parsed JSON payload.
        def _wait_timelapse(since: float, timeout: float) -> dict:
            def _is_timelapse(payload: bytes) -> bool:
                try:
                    ev = json.loads(payload.decode())
                except (json.JSONDecodeError, AttributeError):
                    return False
                return ev.get("trigger") == "timelapse"

            payload = mqtt_rec.wait_for(
                f"{bench_id}/event/photo",
                _is_timelapse,
                timeout=timeout,
                since=since,
            )
            return json.loads(payload.decode())

        # First shot: up to 80 s. The schedule fires `interval × 60 = 60`
        # seconds after `cfg("tlapse_min", "1")` confirms, plus up to
        # one 1 Hz main-loop tick of slack.
        first = _wait_timelapse(engaged_at, timeout=80.0)
        first_seq = first.get("seq", -1)
        assert first_seq > 0

        # Second shot: another minute (+slack). 70 s gives one tick of
        # margin past the 60 s interval. Asserts the schedule is
        # genuinely periodic, not just "fired once and stopped".
        second = _wait_timelapse(since=time.time(), timeout=70.0)
        second_seq = second.get("seq", -1)
        assert second_seq > first_seq, (
            f"second timelapse shot seq {second_seq} did not advance "
            f"past first {first_seq} — capture mutex stuck?"
        )

        # Cross-validate the EXIF offset path with the timelapse trigger
        # specifically. "timelapse" is 9 chars + NUL = 10 bytes (> 4)
        # so the firmware must use the offset path in emit_ascii. If
        # the inline/offset selector regresses for >4-byte triggers,
        # this assertion catches it before deployment.
        # https + follow_redirects, matching the conftest client fixtures: an
        # enrolled board redirects :80 → :443, and this was the one place
        # still fetching over plain HTTP without following it.
        with httpx.Client(
            base_url=f"https://{bench_ip}", timeout=10.0, verify=False, follow_redirects=True
        ) as http:
            r = http.get("/last.jpg")
            r.raise_for_status()
            img = Image.open(io.BytesIO(r.content))
            exif = img.getexif()
            desc = exif.get(0x010E)  # ImageDescription
            assert desc == "timelapse", (
                f"ImageDescription on last.jpg is {desc!r}, expected "
                f"'timelapse' — offset-path encoding regression in "
                f"emit_ascii (or /last.jpg is serving a stale frame)"
            )
    finally:
        # Best-effort restore. If one cfg() call raises, still try the
        # others — losing a board with PIR pinned off for a test is
        # worse than a noisy cleanup log.
        for key, value in prev.items():
            if value is None:
                continue
            try:
                cfg(key, value)
            except Exception:
                pass
