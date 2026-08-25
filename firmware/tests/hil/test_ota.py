"""HIL: OTA update cycle — the field-safety gate.

The field board is OTA-only: a bad OTA that bricks it is unrecoverable. This
exercises the full path on the BENCH (same ESP32-S3 HW) so a field OTA is
de-risked first: trigger a poll, and if a newer signed build is on the server,
confirm the box downloads it, reboots, comes back online on the NEW version,
and STAYS there past the 180 s mark-valid window (i.e. the bootloader did NOT
roll it back).

Anti-brick chain this verifies end to end:
  download → reboot → PENDING_VERIFY → 180 s uptime + MQTT → mark-valid.
A new image that fails to boot or reach the broker never marks valid and the
bootloader rolls back to the old image — so this test passing on the bench
means the field can't be bricked by a non-booting/non-connecting image. (The
residual risk is anti-rollback/secure_version fusing on the field: once the new
image marks valid it can't be rolled back, so a LATENT bug surviving 180 s would
be stuck — which is exactly why this bench gate + the rest of the HIL suite
must be green before pushing to the field.)

Prereq: a newer signed build on the OTA server (tools/ota_upload.sh --sign).
Without one, cmd/ota is a no-op and the test SKIPS. SLOW (~5 min) + state_change.
"""

from __future__ import annotations

import json
import os
import time

import pytest


def _online(b: bytes) -> bool:
    return b.strip() == b"online"


def _offline(b: bytes) -> bool:
    return b.strip() == b"offline"


def _fw_id(payload: bytes) -> str:
    d = json.loads(payload.decode())
    return d.get("sha") or d.get("version") or ""


@pytest.mark.state_change
def test_ota_cycle_and_mark_valid(mqtt_rec, bench_id: str, request) -> None:
    # This test only makes sense when a newer SIGNED build has been deliberately
    # staged on the OTA server (tools/ota_upload.sh --sign). Otherwise triggering
    # cmd/ota is unsafe here: our version guard fails OPEN (reproducible builds
    # blank __DATE__, and git-hash versions don't order), so a poll against a
    # server that only holds an OLDER image will happily DOWNGRADE the bench —
    # silently swapping the build-under-test for a stale one and breaking the
    # rest of the suite. Gate on an explicit opt-in so the default run can never
    # downgrade itself; set CB_OTA_STAGED=1 after staging a newer signed build.
    if not os.environ.get("CB_OTA_STAGED"):
        pytest.skip(
            "OTA cycle test needs a freshly-staged NEWER signed build on the "
            "server (run tools/ota_upload.sh --sign, then CB_OTA_STAGED=1). "
            "Skipping by default — triggering cmd/ota against the stale server "
            "image would downgrade the bench (version guard fails open)."
        )

    avail = f"{bench_id}/state/availability"
    fw = f"{bench_id}/state/fw_version"

    mqtt_rec.wait_for(avail, _online, timeout=60.0)
    ver_before = _fw_id(mqtt_rec.wait_for(fw, timeout=10.0))

    # The lifecycle pins ota_enabled OFF (so the bench can't OTA itself away
    # mid-run); this test deliberately exercises OTA, so turn it back ON first.
    mqtt_rec._client.publish(f"{bench_id}/cmd/cfg/ota_enabled", "ON", qos=1)
    try:
        mqtt_rec.wait_for(
            f"{bench_id}/state/cfg/ota_enabled", lambda p: p.strip() == b"ON", timeout=10.0
        )
    except TimeoutError:
        pass  # older fw without the echo; cmd/ota below still tells us

    # CRITICAL: restore the lifecycle's OFF pin no matter how this test exits
    # (pass, assert, or — the common case here — pytest.skip on a no-op poll).
    # This is the ONLY test that flips ota_enabled ON; if it leaks ON, the bench
    # self-OTAs to whatever stale image sits on ota.example.com once mark-valid lifts
    # the pending-verify gate, and the entire rest of the suite then runs on the
    # WRONG build (debug routes 404, missing features → timeouts). addfinalizer
    # runs on teardown even after pytest.skip().
    def _restore_ota_off():
        mqtt_rec._client.publish(f"{bench_id}/cmd/cfg/ota_enabled", "OFF", qos=1)
        try:
            mqtt_rec.wait_for(
                f"{bench_id}/state/cfg/ota_enabled", lambda p: p.strip() == b"OFF", timeout=10.0
            )
        except TimeoutError:
            pass

    request.addfinalizer(_restore_ota_off)

    t0 = time.time()
    mqtt_rec._client.publish(f"{bench_id}/cmd/ota", "1", qos=1)

    # A newer build → box goes offline (download + reboot). No update → it stays
    # online and cmd/ota is a no-op.
    try:
        mqtt_rec.wait_for(avail, _offline, timeout=90.0, since=t0)
    except TimeoutError:
        latest = mqtt_rec.latest(avail)
        assert latest is not None and _online(latest), "box went away on a no-op OTA poll"
        pytest.skip(
            "no newer build on the OTA server — cmd/ota was a no-op. Upload a "
            "signed build (tools/ota_upload.sh --sign) to exercise the full cycle."
        )

    went_off = time.time()
    # Download + reboot + WiFi/MQTT reconnect. Generous on a weak link.
    online = mqtt_rec.wait_for(avail, _online, timeout=240.0, since=went_off + 1.0)
    assert _online(online), "box did not come back online after OTA — rollback or stuck"

    ver_after = _fw_id(mqtt_rec.wait_for(fw, timeout=30.0, since=went_off))
    print(
        f"OTA: {ver_before!r} -> {ver_after!r} (recovered online "
        f"{time.time() - went_off:.0f}s after going offline)"
    )
    assert ver_after and ver_after != ver_before, (
        f"version did not change after OTA ({ver_before!r}); the new image may "
        "have rolled back immediately"
    )

    # Mark-valid happens at MARK_VALID_DELAY_US (180 s) of clean runtime + MQTT.
    # Wait past it and confirm the box is STILL online on the NEW version — i.e.
    # the bootloader did not roll back. This is the core "OTA stuck / didn't
    # brick" assertion.
    time.sleep(200)
    still = mqtt_rec.latest(avail)
    assert still is not None and _online(still), (
        "box went offline after the mark-valid window — rolled back or crash-looping"
    )
    ver_final = _fw_id(mqtt_rec.wait_for(fw, timeout=30.0))
    assert ver_final == ver_after, (
        f"version reverted after mark-valid window ({ver_after!r} -> {ver_final!r}) "
        "— image was rolled back; investigate before any field OTA"
    )
