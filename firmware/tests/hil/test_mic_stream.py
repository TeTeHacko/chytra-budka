"""
test_mic_stream — /mic.wav must never busy-spin the httpd task.

Regression guard for the v0.4.4 `task_wdt` root cause (coredump-confirmed):
mic_wav_get's empty-ring wait used `vTaskDelay(pdMS_TO_TICKS(8))`, which rounds
to 0 ticks at CONFIG_FREERTOS_HZ=100. `vTaskDelay(0)` does NOT yield to lower-
priority tasks, so when the audio ring stayed empty (notably with BLE on, which
starves the i2s DMA) the loop busy-spun at the httpd-task priority, starved the
main loop + IDLE0 on CPU0, and the task watchdog rebooted the board ~30 s later.

Fixed in two layers:
  * cb_delay_ms() floors every short wait at one tick so it always yields
    (firmware/main/cb_time.h), and
  * a no-audio stall cap ends the stream instead of holding the socket (and the
    single s_mic_busy slot) open for the full max_secs.

This test asserts the invariant that survives both: /mic.wav always RETURNS
within a bound, and the board does not reboot while it runs. It can't force the
BLE-starved empty-ring condition on the bench deterministically (that's the
manual BLE-on repro in the PR), but a spin OR a reboot here would still fail it.
"""

from __future__ import annotations

import time


def _offline_since(mqtt_rec, bench_id: str, since: float) -> bool:
    """True if a state/availability == 'offline' was recorded since `since`.

    A task_wdt reboot drops the MQTT LWT, so the broker publishes 'offline'
    on the availability topic — that transition is our reboot tripwire.
    """
    topic = f"{bench_id}/state/availability"
    with mqtt_rec._lock:
        return any(
            t == topic and ts >= since and p == b"offline" for (t, p, ts) in mqtt_rec._messages
        )


def test_mic_stream_returns_and_board_stays_up(http, mqtt_rec, bench_id):
    """Hammer short /mic.wav fetches; each must return fast and not reboot."""
    start = time.time()

    for _ in range(3):
        t0 = time.time()
        r = http.get("/mic.wav", params={"max": 2}, timeout=20.0)
        elapsed = time.time() - t0

        # Acceptable outcomes: 200 (streamed PCM), 500 (mic not capturing yet),
        # 503 (another stream already active). Anything 5xx-other or a read
        # timeout means the handler hung.
        assert r.status_code in (200, 500, 503), (
            f"/mic.wav returned {r.status_code}: {r.text[:200]}"
        )
        # With max=2 s and a 2 s stall cap, a healthy return is well under 15 s.
        # The pre-fix busy-spin would instead read-timeout or reboot the box.
        assert elapsed < 15.0, f"/mic.wav took {elapsed:.1f}s (expected <15s)"

    # Board must still serve HTTP afterwards …
    r = http.get("/", timeout=10.0)
    assert r.status_code == 200, f"board not serving after /mic.wav stress: {r.status_code}"
    # … and must not have dropped offline (rebooted) at any point in the test.
    assert not _offline_since(mqtt_rec, bench_id, start), (
        "board went 'offline' during /mic.wav stress — watchdog reboot regression"
    )
