"""
test_power_mode — exercise the per-tier audio behaviour: the audio active-hours
WINDOW and the resulting state/audio_active signal.

Background (no battery on the bench → SOC ladder seeds Active):
  * `audio_on_h` / `audio_off_h` gate the mic pipeline by local wall clock.
    on == off  => window DISABLED → always open (the default).
    Otherwise the predicate is cb::audio_window_open(hour, on, off).
  * The firmware publishes retained `state/audio_active` (ON/OFF) on every
    change — ON when the resolved tier (Max/Active) AND the window both want the
    mic up, OFF otherwise (window closed, or a sleeping tier Eco/Sentinel where
    audio is off by definition).

These tests pin a tier via power_profile, then open/close the window and assert
state/audio_active flips. They use Max/Active (the audio-capable, non-sleeping
tiers) so cfg round-trips stay fast — no light-sleep DTIM latency to fight.
Everything is restored in `finally` so a later test never inherits a closed
window (which would silence the mic and look like a mic regression).
"""

from __future__ import annotations

import time

try:
    from zoneinfo import ZoneInfo

    def _bench_hour() -> int:
        # Firmware TZ is Europe/Prague (main.cpp setenv TZ). Match it so the
        # window math here agrees with the device's localtime_r().
        from datetime import datetime

        return datetime.now(ZoneInfo("Europe/Prague")).hour
except Exception:  # pragma: no cover - zoneinfo always present on 3.9+

    def _bench_hour() -> int:
        return time.localtime().tm_hour


def _wait_audio_active(mqtt_rec, bench_id, want: str, *, timeout: float = 6.0):
    """Wait until retained state/audio_active equals want ('ON'/'OFF')."""
    got = mqtt_rec.wait_for(
        f"{bench_id}/state/audio_active",
        lambda p: p.decode() == want,
        timeout=timeout,
    )
    assert got is not None, (
        f"state/audio_active never reached {want!r} within {timeout}s "
        f"(latest={mqtt_rec.latest(f'{bench_id}/state/audio_active')!r})"
    )


def test_audio_window_gates_mic(mqtt_rec, cfg, bench_id):
    """Closing the active-hours window stops the mic; reopening restarts it.

    Scope: the audio active-hours WINDOW gating state/audio_active. We do NOT
    assert state/profile here — the window cannot move the ladder by
    construction (apply_power_state() only toggles audio + WiFi PS; it never
    calls enter_profile()), and the power_profile→tier mapping is covered by
    test_mode_fsm.
    """
    try:
        # Active wants audio (and never light-sleeps, so cfg round-trips are
        # fast + deterministic) → the window is the only remaining gate.
        cfg("power_profile", "active")

        # Disabled window (on == off) → always open → mic active.
        cfg("audio_on_h", 0)
        cfg("audio_off_h", 0)
        _wait_audio_active(mqtt_rec, bench_id, "ON")

        # A 1-hour window 6 h ahead of now → excludes the current hour →
        # window closed → mic torn down even though the tier still wants audio.
        closed = (_bench_hour() + 6) % 24
        cfg("audio_on_h", closed)
        cfg("audio_off_h", (closed + 1) % 24)
        _wait_audio_active(mqtt_rec, bench_id, "OFF")

        # Reopen (disable window) → mic comes back.
        cfg("audio_on_h", 0)
        cfg("audio_off_h", 0)
        _wait_audio_active(mqtt_rec, bench_id, "ON")
    finally:
        cfg("audio_on_h", 0)
        cfg("audio_off_h", 0)
        cfg("power_profile", "active")


def test_tier_gates_audio_active(mqtt_rec, cfg, bench_id):
    """Sleeping tiers idle the mic; Max/Active run it (window left open)."""
    try:
        cfg("audio_on_h", 0)  # window disabled → the tier is the only gate
        cfg("audio_off_h", 0)

        # Sentinel is a sleeping tier → audio off by definition.
        cfg("power_profile", "sentinel")
        _wait_audio_active(mqtt_rec, bench_id, "OFF")

        cfg("power_profile", "max")
        _wait_audio_active(mqtt_rec, bench_id, "ON")

        cfg("power_profile", "active")
        _wait_audio_active(mqtt_rec, bench_id, "ON")
    finally:
        # Leave an awake, audio-capable tier (not light-sleeping sentinel).
        cfg("power_profile", "active")
