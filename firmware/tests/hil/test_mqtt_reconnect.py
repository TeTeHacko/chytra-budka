"""
test_mqtt_reconnect — verify LWT + reconnect after a forced WiFi drop.

Exercises the end-to-end disconnect path that field deployments care
about most: the bench loses WiFi (router reboot, AP roam, signal dip),
the broker eventually marks it offline via LWT, and once WiFi is back
the firmware re-associates → reconnects MQTT → republishes
`state/availability=online`. Two slow tests (LWT timing is gated by
the broker's keepalive, default 90 s = 1.5 × the 60 s firmware-side
keepalive) — but they're the only way to catch a regression where
the reconnect backoff has been broken (eg. by an exception in the
MQTT_EVENT_CONNECTED handler that prevents publishing `online`).

The disconnect itself is triggered via `/debug/wifi_disconnect`
which calls `wifi_mgr_force_disconnect()`. The handler responds
BEFORE killing WiFi so the HTTP client doesn't see a connection
drop mid-body.
"""

from __future__ import annotations

import time

import pytest

# Broker-side LWT: firmware keepalive=60 s. In practice mosquitto
# fires it on TCP RST in seconds, but allow 120 s for the worst-case
# keepalive-expiry path.
LWT_TIMEOUT = 120.0

# After LWT fires, WiFi backoff (≤2 s) + reassociate (1-5 s) + MQTT
# reconnect (handshake + subscribe + publish online ≈ 2-3 s). Worst
# case is ~30 s; budget 60 s for AP-side surprise.
RECONNECT_TIMEOUT = 60.0


def test_wifi_disconnect_triggers_lwt_then_reconnect(http, mqtt_rec, bench_id):
    """Drop WiFi → bench cycles availability and ends back online.

    What we assert:
      1. The kick request reaches the firmware (HTTP 200 OR TCP reset
         mid-flush — both prove the handler ran; firmware kills WiFi
         right after `httpd_resp_send` without a vTaskDelay).
      2. The bench publishes BOTH 'offline' (broker LWT) and 'online'
         (firmware republish on reconnect) after sent_at, in EITHER
         ORDER. Mosquitto's LWT may fire after the new session has
         already CONNECTed — the spec says it shouldn't, but the
         implementation is loose under fast reconnect — so we don't
         require offline-then-online ordering.
      3. Final retained value is 'online' so subsequent tests don't
         inherit a broken state.
    """
    avail_topic = f"{bench_id}/state/availability"

    # Sanity: bench is online before we kick it.
    pre = mqtt_rec.latest(avail_topic)
    assert pre == b"online", (
        f"bench not online before disconnect test (got {pre!r}); aborting "
        "so we don't chain failure modes."
    )

    sent_at = time.time()
    r = http.get("/debug/wifi_disconnect?confirm=yes", timeout=10.0)
    assert r.status_code == 200, f"/debug/wifi_disconnect returned {r.status_code}: {r.text[:200]}"
    assert "OK" in r.text, r.text

    # Order-independent: track whether we've seen both transitions
    # since sent_at; final state must be online.
    deadline = time.time() + LWT_TIMEOUT + RECONNECT_TIMEOUT
    saw_offline = False
    saw_online_after_kick = False
    while time.time() < deadline:
        # Scan recorder buffer for any post-kick offline/online events.
        with mqtt_rec._lock:
            for t, payload, ts in mqtt_rec._messages:
                if t != avail_topic or ts < sent_at:
                    continue
                if payload == b"offline":
                    saw_offline = True
                elif payload == b"online":
                    saw_online_after_kick = True
        latest = mqtt_rec.latest(avail_topic)
        if saw_offline and saw_online_after_kick and latest == b"online":
            return
        time.sleep(1.0)

    pytest.fail(
        f"availability cycle incomplete after kick: "
        f"saw_offline={saw_offline}, saw_online_after_kick={saw_online_after_kick}, "
        f"latest={mqtt_rec.latest(avail_topic)!r}"
    )


def test_http_recovers_after_reconnect(http, mqtt_rec, bench_id):
    """After the reconnect path completes, HTTP still works.

    Re-runs the disconnect, waits for the online event, then GETs
    `/selftest` to verify the HTTP server came back up cleanly along
    with WiFi+MQTT (regression catch: if the HTTP server task got
    wedged during a WiFi-down event, this would 5xx or time out).
    """
    avail_topic = f"{bench_id}/state/availability"

    sent_at = time.time()
    http.get("/debug/wifi_disconnect?confirm=yes", timeout=10.0)

    # Order-independent: wait until both offline and online have been
    # seen after sent_at AND the latest retained is online.
    deadline = time.time() + LWT_TIMEOUT + RECONNECT_TIMEOUT
    saw_offline = False
    saw_online = False
    while time.time() < deadline:
        with mqtt_rec._lock:
            for t, payload, ts in mqtt_rec._messages:
                if t != avail_topic or ts < sent_at:
                    continue
                if payload == b"offline":
                    saw_offline = True
                elif payload == b"online":
                    saw_online = True
        if saw_offline and saw_online and mqtt_rec.latest(avail_topic) == b"online":
            break
        time.sleep(1.0)
    else:
        pytest.fail(
            f"availability cycle incomplete: saw_offline={saw_offline}, "
            f"saw_online={saw_online}, latest={mqtt_rec.latest(avail_topic)!r}"
        )

    # Small settle delay: WiFi-mgr might still be finalising. The HTTP
    # server task is independent but a fresh GET right at reconnect can
    # race with the lwIP socket bind.
    time.sleep(2.0)

    r = http.get("/selftest", timeout=10.0)
    assert r.status_code == 200, (
        f"/selftest after reconnect returned {r.status_code}: {r.text[:200]}"
    )
    assert r.json().get("wifi") is True, r.json()
    assert r.json().get("mqtt") is True, r.json()
