"""HIL: WiFi scan picker (/wifiscan).

GET /wifiscan triggers an active scan and renders the nearby APs as a tap-list
with RSSI signal bars, each linking to /config?ssid=… . Gated like /config
(HTTPS-only auth). The scan briefly borrows the STA (or, in AP-only mode,
APSTA) and blocks ~a few seconds.
"""

from __future__ import annotations


def test_wifiscan_responds(https, http_basic_creds) -> None:
    """/wifiscan returns the scan page (200) with the title; on a bench that
    can see APs it lists them with dBm. A shielded/empty RF env may legitimately
    report "no networks" — accept that, but not a 5xx or an empty body."""
    u, p = http_basic_creds["user"], http_basic_creds["password"]
    r = https.get("/wifiscan", auth=(u, p), timeout=25.0)
    assert r.status_code == 200, f"/wifiscan {r.status_code}: {r.text[:200]}"
    body = r.text
    assert "WiFi" in body, "scan page title missing"
    assert (
        "dBm" in body  # ≥1 AP listed with signal
        or "No networks" in body  # EN empty
        or "Žádné" in body  # CS empty
    ), f"scan page neither listed APs nor reported empty: {body[:300]}"


def test_wifiscan_requires_auth(https) -> None:
    """/wifiscan is gated — an unauthenticated request must NOT get the scan
    page. The firmware returns 401 when basic-auth is configured, and 404 when
    it isn't (wifi_form_allowed: "no authenticated path → don't expose it"), so
    on an auth-disabled bench (GlitchTip PJ, placeholder creds) 404 is the
    correct gate, not a regression. Either denial is acceptable; only a 200
    (the scan page served without auth) is a real failure."""
    status = https.get("/wifiscan", auth=None, timeout=10.0).status_code
    assert status in (401, 404), f"unauthenticated /wifiscan must be denied, got {status}"
