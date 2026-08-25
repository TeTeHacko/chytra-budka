"""
test_boot_smoke — verify the bench's basic post-boot state.

Doesn't flash anything; assumes the bench is already up and on WiFi.
Future iteration will add a `pytest-embedded` variant that drives
a clean flash+boot cycle and asserts these same things against a
fresh image.
"""

from __future__ import annotations

import json
import time


def test_selftest_returns_200(http):
    """/selftest returns a JSON body with the expected sensor keys."""
    r = http.get("/selftest")
    assert r.status_code == 200, f"/selftest HTTP {r.status_code}: {r.text}"
    body = r.json()

    # The summary string drives the dashboard's red/green badge.
    assert "summary" in body and isinstance(body["summary"], str)
    assert body["summary"].startswith(("ok", "degraded")), body["summary"]

    # Every sensor key must be present (true/false depending on wiring),
    # not silently missing. This catches a build regression that drops
    # an entire selftest row.
    expected = {
        "battery",
        "sht41",
        "sht41_ext",
        "ina226",
        "sd",
        "camera",
        "pir",
        "reed",
        "mic",
        "wifi",
        "mqtt",
    }
    assert expected.issubset(body.keys()), f"missing keys: {expected - set(body)}"


def test_homepage_serves_html(http):
    """`/` renders the dashboard HTML."""
    r = http.get("/")
    assert r.status_code == 200, r.text
    body = r.text
    assert "Chytrá Budka" in body
    assert "<table>" in body  # the sensor/selftest tables


def test_mqtt_availability_online(mqtt_rec, bench_id):
    """LWT `state/availability` retained payload reads 'online'."""
    # The topic is retained; the subscribe in the mqtt_rec fixture
    # already triggered a replay, so the value should be there.
    payload = mqtt_rec.wait_for(
        f"{bench_id}/state/availability",
        lambda p: p in (b"online", b"offline"),
        timeout=5.0,
    )
    assert payload == b"online", (
        f"bench published availability={payload!r}; expected 'online' from a healthy board."
    )


def test_mqtt_fw_version_published_once(mqtt_rec, bench_id):
    """`state/fw_version` retained payload parses as JSON with expected keys.

    Per the Q11 follow-up, the function is now once-per-boot guarded
    internally so this topic should reflect the running app exactly.
    """
    payload = mqtt_rec.wait_for(
        f"{bench_id}/state/fw_version",
        lambda p: p.startswith(b"{"),
        timeout=5.0,
    )
    body = json.loads(payload)
    for key in ("version", "project_name", "date", "time", "idf_ver", "sha"):
        assert key in body, f"{key!r} missing from fw_version payload: {body}"
    assert body["project_name"] == "chytra-budka"


def test_required_modules_in_selftest_green(http):
    """Required modules (wifi, mqtt, camera, mic, sd, sht41) must all be true.

    Optional ones (battery, ina226, sht41_ext, reed, pir) depend on
    the bench's actual wiring and are allowed to be false. This test
    catches a regression that breaks a module we always have wired.
    """
    body = http.get("/selftest").json()
    required = ["wifi", "mqtt", "camera", "mic", "sd", "sht41"]
    failing = [m for m in required if not body.get(m)]
    assert not failing, f"selftest failing on required modules: {failing}; full body: {body}"


def test_uptime_increases(http):
    """`state/uptime_s` (via the / homepage Uptime row) advances over 2s.

    Sanity check that the device's clock is actually running — a
    hung scheduler would freeze this. The homepage renders Uptime
    as "<N> s"; we parse the integer out and verify it monotonically
    increased.
    """
    import re

    def fetch_uptime() -> int:
        html = http.get("/").text
        m = re.search(r'<td class="k">Uptime</td><td class="v">(\d+)\s*s</td>', html)
        assert m, f"Uptime row not found in homepage HTML: {html[:300]}"
        return int(m.group(1))

    a = fetch_uptime()
    time.sleep(2.5)
    b = fetch_uptime()
    assert b > a, f"uptime did not advance: {a} → {b} after 2.5 s wait"
