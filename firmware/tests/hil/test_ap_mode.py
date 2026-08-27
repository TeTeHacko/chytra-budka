"""HIL: everything that should work in the AP/unprovisioned onboarding portal.

These join the bench SoftAP on the host's wlan0 (via the `ap_join` fixture —
invasive: the host leaves its station LAN for the test) and verify the local
portal + sensors are live, so an operator positioning the box during onboarding
can see the camera, hear the mic, and scan WiFi. Requires the bench in AP mode
(factory-reset / unprovisioned); skips otherwise.

Background: AP mode used to freeze the mode FSM at Boot, so audio_task never
pumped and the mic looked dead (frames_captured=0). The fix runs mode_tick() in
AP mode too; test_ap_mic_capturing is the regression guard for that.
"""

from __future__ import annotations

import time

import httpx
import pytest

pytestmark = pytest.mark.ap_mode


def test_ap_homepage(ap_join) -> None:
    """Portal homepage serves over the AP (plain HTTP, no auth)."""
    r = ap_join.get("/")
    assert r.status_code == 200, f"AP homepage {r.status_code}"
    assert "Chytr" in r.text, "homepage body missing brand"


def test_ap_config_open(ap_join) -> None:
    """While the AP is up the gate is off (WPA2 is the boundary) → /config is
    reachable without basic-auth, with the WiFi + admin-login sections."""
    r = ap_join.get("/config")
    assert r.status_code == 200, f"/config {r.status_code} over AP"
    assert "ssid" in r.text.lower(), "/config missing WiFi STA form"


def test_ap_wifiscan_works(ap_join) -> None:
    """/wifiscan must work IN AP mode (borrows STA → APSTA → scans → AP) — the
    whole point of the picker is onboarding, where you pick your home WiFi."""
    r = ap_join.get("/wifiscan", timeout=25.0)
    assert r.status_code == 200, f"/wifiscan {r.status_code} over AP"
    assert "dBm" in r.text or "No networks" in r.text or "Žádné" in r.text, (
        f"scan neither listed APs nor reported empty: {r.text[:300]}"
    )


def test_ap_capture(ap_join) -> None:
    """Camera capture works over the AP (for framing the box). Retries with a
    generous timeout: the first shot right after AP boot can be slow (camera
    warmup / a VAD capture in flight) and the SoftAP link is modest."""
    last = ""
    for _ in range(3):
        try:
            r = ap_join.get("/capture", timeout=30.0)
            if r.status_code == 200 and r.content[:3] == b"\xff\xd8\xff":
                return
            last = f"status={r.status_code}, magic={r.content[:3]!r}"
        except httpx.HTTPError as e:
            last = f"{type(e).__name__}: {e}"
        time.sleep(2)
    pytest.fail(f"/capture did not return a JPEG over AP after retries: {last}")


def test_ap_mic_capturing(ap_join) -> None:
    """Mic must be CAPTURING in AP mode (selftest mic:true). Regression guard
    for the FSM-frozen-at-Boot bug: without mode_tick() in AP mode the mic never
    pumps. Retries — frames_captured needs a moment after the mode transition."""
    last = ""
    for _ in range(15):
        r = ap_join.get("/selftest", timeout=8.0)
        last = r.text
        if r.status_code == 200 and '"mic":true' in r.text:
            return
        time.sleep(2)
    pytest.fail(f"mic never reported true in AP mode (FSM stuck in Boot?): {last[:200]}")
