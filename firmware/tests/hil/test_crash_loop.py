"""
test_crash_loop — drive a forced TWDT panic and verify the diag pipeline.

The single most important regression catch in the HIL suite: prove
the panic → coredump → reboot → diag publish chain is intact end to
end. A silent break here means a field unit crashes invisibly and
the operator sees only "device went offline" with no reset_reason
to diagnose against.

Mechanism: GET `/debug/hang?ms=55000&confirm=yes` blocks the main
loop without feeding TWDT (configured at 30 s, see ESP_TASK_WDT_*).
The main task shares the TWDT with several other tasks, so a hung
main loop is caught at 30 s + the slowest task's reset interval
(~37 s on the bench), not 30 s — see the comment at the trigger for
why the hang is 55 s, not ~35 s. The watchdog panics → coredump
partition holds the backtrace → reboot. On next boot
`diag_publish_boot()` ships a
retained `diag/boot` JSON with the new reset_reason +
consecutive_crashes counter.

Pre-flight skip: if the bench is already mid-crash-chain
(consecutive_crashes >= 2) we skip rather than push the counter
further — let the 180 s clean-runtime timer in main.cpp clear the
RTC slot first.
"""

from __future__ import annotations

import json
import time

import pytest

# TWDT panic = 30 s (CONFIG_ESP_TASK_WDT_TIMEOUT_S); coredump write
# + reboot + boot + WiFi connect + MQTT connect + retained republish
# ≈ 30 s on a clean run. Broker-side LWT eats another 90 s window
# while the board is rebooting, so wait_for(availability=online) on
# the retained replay needs ~120 s headroom.
REBOOT_WINDOW = 120.0


def _parse_diag_boot(payload: bytes) -> dict:
    """diag/boot is JSON like {"reset":"task_wdt","consecutive_crashes":1,...}."""
    return json.loads(payload.decode())


def _current_consecutive_crashes(mqtt_rec, bench_id: str) -> int | None:
    """Read the retained diag/boot payload, return consecutive_crashes or None."""
    raw = mqtt_rec.latest(f"{bench_id}/diag/boot")
    if raw is None:
        return None
    try:
        return int(_parse_diag_boot(raw).get("consecutive_crashes", 0))
    except (json.JSONDecodeError, ValueError, TypeError):
        return None


def test_debug_hang_triggers_task_wdt_reset(http, mqtt_rec, bench_id):
    """`/debug/hang` → task_wdt reset → diag/boot publishes new reason.

    Captures the pre-crash diag/boot retained payload, triggers the
    hang, waits for the device to come back online, and asserts the
    new retained diag/boot has:
      - reset == "task_wdt" (or "int_wdt" — both are TWDT-class
        depending on which fires first on this build)
      - consecutive_crashes > pre-crash count

    Why both task_wdt + int_wdt accepted: the firmware configures
    Task WDT to panic + reset, but on a stuck CPU the Interrupt WDT
    can sometimes fire first if the panic handler itself takes too
    long. Either way the diag pipeline must publish *something* in
    that family — a "poweron" or "esp_restart" here would mean the
    coredump path silently rebooted via a clean restart instead of
    the panic path, which is a critical regression.
    """
    diag_topic = f"{bench_id}/diag/boot"
    avail_topic = f"{bench_id}/state/availability"

    pre_count = _current_consecutive_crashes(mqtt_rec, bench_id)
    if pre_count is None:
        pytest.skip(
            "no diag/boot retained payload yet — bench hasn't published a "
            "boot record since broker last cleared retained state; rerun "
            "after a clean boot."
        )
    if pre_count >= 2:
        pytest.skip(
            f"consecutive_crashes={pre_count} already — refusing to chain "
            "another crash. Wait 3 min for diag_boot_succeeded() to clear "
            "the RTC slot, or cmd/reboot the bench and wait 3 min."
        )

    # Snapshot the pre-crash diag/boot so we can detect the NEW publish
    # by content change, not by paho callback timing.
    pre_diag_raw = mqtt_rec.latest(diag_topic)
    assert pre_diag_raw is not None  # asserted by skip-check above

    sent_at = time.time()
    # Hang for 55 s, NOT ~35 s. The main task shares the TWDT with
    # audio/photo/ota/camera, and esp_task_wdt only re-arms the hardware
    # timer once *every* subscribed task has checked in. The main loop's
    # last check-in completes one more round just after the hang begins, so
    # a hung main loop is actually caught at 30 s + the slowest subscribed
    # task's reset interval (~37 s measured on the bench), not 30 s. A 35 s
    # hang self-releases before the panic can fire (the loop resumes and the
    # board never reboots) → the test saw a non-task_wdt reset and failed.
    # 55 s clears the real fire time with margin and stays under the 60 s
    # /debug/hang cap; the panic still lands at ~37 s so this costs no extra
    # wall-clock. Do not "optimise" this back down to 35 s.
    r = http.get("/debug/hang?ms=55000&confirm=yes", timeout=10.0)
    assert r.status_code == 200, f"/debug/hang returned {r.status_code}: {r.text[:200]}"
    assert "OK" in r.text, r.text

    # Wait for the device to come back online. The retained `online`
    # from the pre-crash boot is still on the broker until the panic
    # forces a TCP-level drop → LWT → new boot → new publish. Use
    # `since=sent_at` so we don't match the pre-crash retained value.
    mqtt_rec.wait_for(
        avail_topic,
        lambda p: p == b"online",
        timeout=REBOOT_WINDOW + 60.0,  # LWT (90 s) + reboot (30 s) + slack.
        since=sent_at,
    )

    # diag/boot republishes once per boot, immediately after MQTT connect.
    # The recorder might have logged it before or after the online event
    # depending on publish order — give it a moment to settle.
    new_diag_raw = mqtt_rec.wait_for(
        diag_topic,
        lambda p: p != pre_diag_raw,
        timeout=15.0,
        since=sent_at,
    )
    new_diag = _parse_diag_boot(new_diag_raw)

    reset = new_diag.get("reset", "")
    assert reset in ("task_wdt", "int_wdt"), (
        f"expected reset in (task_wdt, int_wdt), got {reset!r}; "
        f"full payload: {new_diag}. A 'poweron' or 'esp_restart' here "
        "means the panic path silently bypassed the watchdog — "
        "investigate the coredump pipeline."
    )

    new_count = int(new_diag.get("consecutive_crashes", 0))
    assert new_count > pre_count, (
        f"consecutive_crashes did not advance: pre={pre_count}, "
        f"post={new_count}. RTC counter may be wired wrong."
    )


def test_debug_hang_records_coredump(mqtt_rec, bench_id):
    """After a TWDT crash, diag/boot reports coredump=true with non-zero size.

    Lighter-weight than the first test; only checks the retained value
    most recently published. If the first test already crashed the
    bench this turn, the retained payload reflects that and we don't
    need to crash again.
    """
    diag_topic = f"{bench_id}/diag/boot"

    raw = mqtt_rec.latest(diag_topic)
    if raw is None:
        pytest.skip("diag/boot not yet retained on this run")
    diag = _parse_diag_boot(raw)

    if diag.get("reset") not in ("task_wdt", "int_wdt", "panic"):
        pytest.skip(
            f"latest boot reset={diag.get('reset')!r} — no recent crash to "
            "validate coredump against. Run test_debug_hang_triggers_task_wdt_reset "
            "first in the same session."
        )

    assert diag.get("coredump") is True, (
        f"coredump flag false after a {diag.get('reset')} reset; the "
        "partition write path may have silently failed."
    )
    cd_bytes = int(diag.get("coredump_bytes", 0))
    assert cd_bytes > 0, (
        f"coredump_bytes={cd_bytes} after a panic — coredump partition "
        "claims to be present but is empty."
    )
