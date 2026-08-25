"""HIL: reconfigurable WiFi + brick-safe credential rollback.

These exercise the verify-before-commit ladder in wifi_store.c / main.cpp
against the live bench board over MQTT.

DESTRUCTIVE + SLOW: the brick-safety test pushes a deliberately-bad
credential candidate, which takes the board OFFLINE for the verify-timeout
window (~2 min) before it auto-reverts to known-good and reconnects. Run
only against the bench (ex01), never the field unit.

    pytest test_wifi_provision.py -v -m state_change

Pre-req: the bench's known-good / compile-default creds must be the network
the broker is on, so that after the auto-revert the board can actually
reconnect (which is the whole point of the test).
"""

import os
import time
from collections.abc import Iterator
from typing import Any

import pytest
from conftest import _mqtt_client, _MqttRecorder

# Recognisable enough to grep for on any topic, and clearly not a real PSK.
SECRET_PW = "HILsecret_pw_4242"


def _online(payload: bytes) -> bool:
    return payload.strip() == b"online"


def _offline(payload: bytes) -> bool:
    return payload.strip() == b"offline"


@pytest.fixture(scope="module")
def bad_candidate_cycle(bench_id: str, provisioned_sta) -> Iterator[dict[str, Any]]:
    """Run ONE bad-WiFi-candidate cycle and hand back what it produced.

    Staging a candidate that cannot associate costs the full
    WIFI_CAND_VERIFY_TIMEOUT_S (240 s) plus a reboot and reconnect — ~270 s
    measured — and it is the most expensive thing in the whole HIL gate. This
    module used to pay it twice: once to prove the board recovers, once to prove
    the password never reaches a topic. Both read properties of the SAME cycle,
    so run it once. That also stops the next test in the module inheriting a
    board that is 270 s into a revert, which it then had to sit and wait out.

    Uses the secret as the candidate's password so the leak check has something
    to look for. Module-scoped, so it builds its own MQTT client: conftest's
    mqtt_rec is deliberately function-scoped for per-test isolation and a
    broader fixture cannot depend on it.

    Returns the recorder plus the timings the assertions need.
    """
    cli = _mqtt_client(f"hil-wifi-{os.getpid()}-{int(time.time())}", {"user": "", "password": ""})
    if cli is None:
        pytest.skip("MQTT broker unreachable — cannot drive a candidate cycle")
    rec = _MqttRecorder(cli)
    cli.subscribe(f"{bench_id}/#")
    cli.loop_start()
    avail = f"{bench_id}/state/availability"
    try:
        # Sanity: board is online before we perturb it.
        rec.wait_for(avail, _online, timeout=30.0)

        t0 = time.time()
        # Correct-looking SSID that does not exist on this LAN + a bogus PSK →
        # the candidate can never associate, so it must be reverted.
        cli.publish(
            f"{bench_id}/cmd/wifi",
            f'{{"ssid":"cb-hil-nonexistent","password":"{SECRET_PW}"}}',
            qos=1,
            retain=False,
        )

        # The handler publishes availability=offline (graceful LWT) before it
        # reboots into the candidate.
        rec.wait_for(avail, _offline, timeout=30.0, since=t0)
        went_offline_at = time.time()
        time.sleep(1.0)  # let the state/wifi ack land before we inspect topics

        # Now the board boots the bad candidate, fails to reach MQTT, and after
        # WIFI_CAND_VERIFY_TIMEOUT_S (currently 240 s — generous for slow-DHCP
        # weak links) reverts to known-good + reboots. Budget = that timeout +
        # reboot + WiFi/MQTT reconnect; measured end-to-end at ~270 s on the
        # bench's link, so 330 s leaves margin. (Was 220 s with a stale "~120 s"
        # assumption, which is < the revert timeout itself → could never pass.)
        payload = rec.wait_for(avail, _online, timeout=330.0, since=went_offline_at + 1.0)
        recovered_after = time.time() - went_offline_at
        yield {
            "rec": rec,
            "online_payload": payload,
            "recovered_after": recovered_after,
        }
    finally:
        cli.loop_stop()
        cli.disconnect()


@pytest.mark.state_change
def test_bad_candidate_auto_reverts(bad_candidate_cycle) -> None:
    """A wrong WiFi candidate must auto-revert to known-good with no reflash.

    This is THE anti-brick acceptance test: creds that can't associate, board
    takes itself offline, then comes back online on its own within the
    verify-timeout + reboot + reconnect budget.
    """
    assert _online(bad_candidate_cycle["online_payload"])
    print(
        f"recovered to online {bad_candidate_cycle['recovered_after']:.0f}s after "
        "going offline (auto-reverted to known-good, no reflash)"
    )


@pytest.mark.state_change
def test_password_never_published(bad_candidate_cycle) -> None:
    """The WiFi password must never appear on any topic.

    The cycle above staged the candidate with a recognisable password; it must
    show up on no message the broker relayed (state/wifi carries SSID + status
    only).

    Only DEVICE-published topics matter. The inbound cmd/wifi the fixture
    published legitimately carries the password and paho echoes it back to us
    (we subscribe to <id>/#) — exclude the command channel.
    """
    rec = bad_candidate_cycle["rec"]
    with rec._lock:
        leaks = [
            t
            for t, payload, _ in rec._messages
            if SECRET_PW.encode() in payload and "/cmd/" not in t
        ]
    assert not leaks, f"password leaked on device-published topic(s): {leaks}"


@pytest.mark.state_change
def test_cfg_reset(mqtt_rec, bench_id: str, request) -> None:
    """cmd/cfg_reset restores schema defaults (reset tier b)."""
    avail = f"{bench_id}/state/availability"
    cq = f"{bench_id}/state/cfg/cam_quality"  # schema default 12

    # cfg_reset returns EVERY knob to its schema default — including
    # ota_enabled, whose default is ON. WiFi creds survive a tier-b reset, so
    # the board stays on the LAN and, left ON, will self-OTA to the stale image
    # on ota.example.com once mark-valid lifts the pending-verify gate (version guard
    # fails open → silent downgrade), breaking any test that follows. Restore
    # the lifecycle's OFF pin on teardown. addfinalizer runs even on assert.
    def _restore_ota_off():
        mqtt_rec._client.publish(f"{bench_id}/cmd/cfg/ota_enabled", "OFF", qos=1)
        try:
            mqtt_rec.wait_for(
                f"{bench_id}/state/cfg/ota_enabled", lambda p: p.strip() == b"OFF", timeout=10.0
            )
        except TimeoutError:
            pass

    request.addfinalizer(_restore_ota_off)
    # The bad-candidate cycle above already waited the board back online, so
    # this is normally instant; the budget covers a board still settling after
    # its revert reboot.
    mqtt_rec.wait_for(avail, _online, timeout=60.0)

    # Move it off-default and confirm the device accepted it.
    mqtt_rec._client.publish(f"{bench_id}/cmd/cfg/cam_quality", "20", qos=1)
    mqtt_rec.wait_for(cq, lambda p: p.strip() == b"20", timeout=10.0)

    t0 = time.time()
    mqtt_rec._client.publish(f"{bench_id}/cmd/cfg_reset", "1", qos=1)

    # Wait for the properties under test, NOT for an offline→online edge pair.
    #
    # This used to chain `wait_for(offline, since=t0)` → `wait_for(online, since=
    # that + 1)`, and it timed out three runs in a row at the tail of a full
    # gate while passing every time the module ran alone. That asymmetry is the
    # tell: availability is retained and only republished on a transition, so if
    # the first wait matches a *stale* offline still in flight from the preceding
    # reboot churn, the anchor is set from that older event and the second wait
    # is left watching for an `online` edge that has already gone past. Nothing
    # ever republishes it, so the test waits out its whole budget against a board
    # that is up and healthy — which is exactly what we saw: 120 s of nothing,
    # then 11 s when the edges happened to line up. Chasing edges through a
    # module whose whole job is rebooting the board is the bug.
    #
    # Both of these are level-triggered and anchored at t0, so a duplicate or
    # late-arriving earlier event cannot mislead them:
    #   - cam_quality back to its schema default = the reset itself
    #   - a fresh diag/boot                      = the board really rebooted
    payload = mqtt_rec.wait_for(cq, lambda p: p.strip() == b"12", timeout=240.0, since=t0)
    assert payload.strip() == b"12"
    boot = mqtt_rec.wait_for(
        f"{bench_id}/diag/boot", lambda p: b"reset" in p, timeout=240.0, since=t0
    )
    print(f"cfg_reset: defaults restored + rebooted in {time.time() - t0:.0f}s ({boot[:80]!r})")

    # And it must still be on the broker afterwards — current state, not an edge.
    assert _online(mqtt_rec.latest(avail) or b""), (
        "config reset restored the defaults but left the board off the broker"
    )


@pytest.mark.state_change
def test_factory_reset(mqtt_rec, bench_id: str, compile_wifi_real: bool, request) -> None:
    """cmd/factory_reset wipes config + TLS + WiFi and recovers (re-enroll).

    WiFi falls back to the compile-time default (the bench's working
    network), TLS re-enrolls on boot, config returns to defaults — so the
    board must come back online on its own.

    Requires a REAL compile-time WIFI_SSID: with blank/placeholder creds the
    factory reset drops the board into the unprovisioned AP-first portal
    instead of reconnecting to the broker's LAN, so it never comes back online
    from the HIL host's vantage (which is on the station network, not the AP).
    Skip rather than hang. (A future AP-join HIL fixture could cover that path.)
    """
    if not compile_wifi_real:
        pytest.skip(
            "compile-time WIFI_SSID is blank/placeholder → factory reset goes "
            "AP-first (unprovisioned), unreachable from the station-LAN HIL host"
        )
    avail = f"{bench_id}/state/availability"
    mqtt_rec.wait_for(avail, _online, timeout=240.0)

    # factory_reset also returns ota_enabled to its ON default — restore the
    # OFF pin on teardown so a post-reset self-OTA can't downgrade the bench
    # for whatever runs next (registered only when the test actually runs, i.e.
    # past the compile_wifi_real skip above).
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
    mqtt_rec._client.publish(f"{bench_id}/cmd/factory_reset", "1", qos=1)
    mqtt_rec.wait_for(avail, _offline, timeout=30.0, since=t0)
    off = time.time()
    # Reboot + WiFi(default) + MQTT + TLS re-enroll. 180 s budget.
    payload = mqtt_rec.wait_for(avail, _online, timeout=180.0, since=off + 1.0)
    assert _online(payload)
    print(f"factory reset recovered to online {time.time() - off:.0f}s after wipe")
