"""
test_http_endpoints — smoke test every documented HTTP endpoint.

Catches "registered handler accidentally compiled out", "URI table
overflowed CONFIG_HTTPD_MAX_URI_HANDLERS", or a route that now returns
500 because of a regression in its handler.

Not a content/snapshot test — those land in a future
test_http_contracts.py once the format stabilises. This is just
"the URL still responds, with a status in [200, 4xx]".
"""

from __future__ import annotations

import pytest

# (URI, expected status — None = "anything in 2xx-4xx", explicit int = exact)
ENDPOINTS = [
    ("/", None),  # homepage HTML
    ("/selftest", 200),
    ("/i2c", 200),
    ("/capture", None),  # may 5xx if camera busy
    ("/last.jpg", None),  # 404 if no capture yet
    ("/photo", None),  # raw bytes, may 5xx if camera busy
    ("/photos", 200),
    ("/last.json", None),  # EXIF of last frame as JSON (200, or {"exif":false})
    ("/photo/exif", None),  # 400 without ?f= (after auth)
    ("/view", None),  # 400 without ?f= (after auth)
    ("/sht41/bus1", None),  # 200 if wired, 5xx if not
    ("/max17048/bus1", None),
    ("/i2c/bus1_diag", None),  # 200 with JSON if wired
]


@pytest.mark.parametrize("uri,expected", ENDPOINTS)
def test_endpoint_responds(http, uri, expected):
    """GET `uri` returns the expected status (or any non-5xx)."""
    # /capture and /photo take a FRESH UXGA frame: camera AGC settle + JPEG
    # encode measures ~11 s on the bench (verified), so the default 10 s read
    # timeout was just-too-tight and flaked with ReadTimeout. Give the capture
    # endpoints headroom; everything else stays snappy at 10 s.
    timeout = 30.0 if uri in ("/capture", "/photo") else 10.0
    r = http.get(uri, timeout=timeout)
    if expected is None:
        # Permissive: anything except 5xx counts as "endpoint is alive".
        assert r.status_code < 500, f"{uri} returned {r.status_code}: {r.text[:200]}"
    else:
        assert r.status_code == expected, (
            f"{uri} expected {expected}, got {r.status_code}: {r.text[:200]}"
        )


def test_debug_endpoints_present_when_enabled(http):
    """If debug endpoints are compiled in, /debug/pir is reachable.

    On production builds CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS=n strips
    these — the test then verifies they're absent (404). Either
    way, an inconsistency between build flag and actual URI table
    would surface here.
    """
    r = http.get("/debug/pir", timeout=5.0)
    # In a dev build we expect 200 with a "pin=GPIOn level=..." body.
    # In a production build we expect 404. The test passes either way
    # but fails on 5xx (handler broken) or weird 3xx redirects.
    assert r.status_code in (200, 404), (
        f"/debug/pir returned unexpected {r.status_code}: {r.text[:200]}"
    )
    if r.status_code == 200:
        assert "pin=GPIO" in r.text and "level=" in r.text, r.text


def test_unknown_endpoint_returns_404(http):
    """A URI the firmware doesn't register returns 404 (not 5xx)."""
    r = http.get("/this-uri-does-not-exist", timeout=5.0)
    assert r.status_code == 404, (
        f"unknown URI returned {r.status_code}, expected 404: {r.text[:200]}"
    )


def test_debug_uart_servo_present(http):
    """POST /debug/uart_servo accepts hex bodies and returns 200 / 503.

    When the pin map has both uart_tx + uart_rx assigned (bench rev 3.2
    after the phase-9 swap), the handler echoes hex of whatever the bus
    returned (often empty when nothing is wired). When unassigned it
    refuses with 503. Either way is "endpoint alive" — 5xx other than
    503 or a 4xx parse error would mean a regression.

    Production builds strip the handler → 404.
    """
    r = http.post("/debug/uart_servo", data="FFFFFE0201FE", timeout=5.0)
    assert r.status_code in (200, 404, 503), (
        f"/debug/uart_servo returned {r.status_code}: {r.text[:200]}"
    )


def test_homepage_has_pin_map_section(http):
    """Homepage carries the runtime Pin map section + an endpoint list.

    Regression guard for the pin-function-map refactor: the operator
    discoverability landed in commit 680721f and depends on
    `app_config_pin_slot_info()` returning all 8 slots. A future
    refactor that drops the section, or breaks the helper so it returns
    zero slots, would still let / return 200 but with no diagnostic
    value — only a content check catches it.
    """
    r = http.get("/", timeout=10.0)
    assert r.status_code == 200, f"homepage 5xx: {r.status_code}"
    body = r.text
    assert "Pin map" in body, "homepage missing 'Pin map' heading"
    # All eight D-slots must appear (D0..D7). Older homepage variants
    # had no per-slot rows — checking each catches a partial-render
    # regression that "Pin map" substring alone would miss.
    for slot in range(8):
        assert f"<code>D{slot}</code>" in body, f"homepage Pin map missing D{slot} row"
    assert "Endpoints" in body, "homepage missing 'Endpoints' heading"
    # Pick a couple of links that used to be missing pre-refactor.
    for href in ("/sht41/bus1", "/max17048/bus1", "/i2c/bus1_diag"):
        assert href in body, f"homepage Endpoints list missing {href}"
