"""
test_dual_core — verify the post-refactor task → core pinning policy
documented in firmware/main/core_assignment.h.

Polls GET /debug/cores (debug-build only) and asserts that the three
pinned tasks land on the expected cores with the expected priorities.
Designed to catch accidental drift: a future PR that flips
xTaskCreatePinnedToCore → xTaskCreate, or bumps a priority, fails here
on the bench instead of shipping a regression into the field.

When CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is enabled, the test also
asserts both cores still have nonzero idle headroom (neither core is
100 %-saturated). Without that Kconfig the runtime_pct field is null
and the percentage asserts are skipped — pinning + priority asserts
still run.

Requires:
  CONFIG_FREERTOS_USE_TRACE_FACILITY=y (default after dual-core refactor)
  CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS=y (default y on bench, n on field)
"""

from __future__ import annotations

import threading
import time

import httpx
import pytest


def _fetch_cores(http: httpx.Client) -> dict:
    r = http.get("/debug/cores")
    assert r.status_code == 200, f"GET /debug/cores: {r.status_code} {r.text!r}"
    data = r.json()
    assert "tasks" in data and isinstance(data["tasks"], list)
    assert "stats_enabled" in data and isinstance(data["stats_enabled"], bool)
    return data


def _by_name(tasks: list[dict], name: str) -> dict:
    for t in tasks:
        if t.get("name") == name:
            return t
    names = [t.get("name", "?") for t in tasks]
    pytest.fail(f"task {name!r} not found in /debug/cores response; available: {sorted(names)}")


def test_pinned_tasks_on_expected_cores(http: httpx.Client):
    """audio and cam_wrk must live on CPU1; main on CPU0."""
    data = _fetch_cores(http)
    tasks = data["tasks"]

    audio = _by_name(tasks, "audio")
    cam_wrk = _by_name(tasks, "cam_wrk")
    main = _by_name(tasks, "main")

    assert audio["core"] == 1, f"audio expected CPU1, got {audio}"
    assert audio["priority"] == 10, f"audio priority expected 10, got {audio}"

    assert cam_wrk["core"] == 1, f"cam_wrk expected CPU1, got {cam_wrk}"
    assert cam_wrk["priority"] == 5, f"cam_wrk priority expected 5, got {cam_wrk}"

    assert main["core"] == 0, f"main expected CPU0, got {main}"
    assert main["priority"] == 1, f"main priority expected 1 (lowered in app_main), got {main}"


def test_idle_headroom_when_stats_enabled(http: httpx.Client):
    """Both IDLE tasks must report nonzero runtime — i.e. neither core
    is fully saturated. Skipped when the Kconfig knob is off (default)."""
    data = _fetch_cores(http)
    if not data["stats_enabled"]:
        pytest.skip(
            "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS not enabled "
            "in this build — runtime_pct is null"
        )
    idle0 = _by_name(data["tasks"], "IDLE0")
    idle1 = _by_name(data["tasks"], "IDLE1")
    assert idle0["runtime_pct"] is not None and idle0["runtime_pct"] >= 5, (
        f"CPU0 has <5% idle headroom — supervisor or another task is hogging CPU0: {idle0}"
    )
    assert idle1["runtime_pct"] is not None and idle1["runtime_pct"] >= 5, (
        f"CPU1 has <5% idle headroom — audio or cam_wrk hogging CPU1: {idle1}"
    )


def test_pinning_survives_stream_plus_capture(http: httpx.Client):
    """Open /stream.mjpg in a background thread, trigger 3 captures via
    HTTP /capture, then re-fetch /debug/cores. The audio task must still
    be on CPU1 (regression catch: accidental repin in a future refactor)."""

    stop = threading.Event()

    def _stream_for_5s():
        try:
            # `max=5` limits the firmware-side stream window. Even if we
            # disconnect early, the firmware tears down on the next chunk.
            with httpx.stream("GET", str(http.base_url) + "/stream.mjpg?max=5", timeout=10.0) as r:
                for _ in r.iter_bytes(chunk_size=4096):
                    if stop.is_set():
                        break
        except (httpx.RequestError, httpx.HTTPError):
            pass  # disconnects are expected as we tear down

    t = threading.Thread(target=_stream_for_5s, daemon=True)
    t.start()

    # Let the stream warm up + trigger captures in parallel.
    time.sleep(0.5)
    for _ in range(3):
        try:
            http.get("/capture", timeout=10.0)
        except httpx.HTTPError:
            pass
        time.sleep(0.3)

    time.sleep(1.5)  # let the worker drain & sensor settle
    data = _fetch_cores(http)
    stop.set()
    t.join(timeout=8.0)

    audio = _by_name(data["tasks"], "audio")
    cam_wrk = _by_name(data["tasks"], "cam_wrk")
    assert audio["core"] == 1, f"audio repinned during stress: {audio}"
    assert cam_wrk["core"] == 1, f"cam_wrk repinned during stress: {cam_wrk}"
