"""HIL: AP-first onboarding acceptance — AP portal → set creds → STA online.

Joins the bench SoftAP, submits real WiFi creds on the /wifi portal, lets the
box reboot into STA + run the verify-before-commit candidate ladder, then
confirms it reappears on the station LAN (provisioned). This is the end-to-end
onboarding path the AP-first feature exists for.

Needs the bench in AP/unprovisioned mode and CB_PROVISION_SSID + CB_PROVISION_PSK
in the environment (the station-LAN creds the box should join). Skips otherwise.
"""

from __future__ import annotations

import time

import httpx
import pytest


def _bench_reachable(ip: str, tries: int = 50, delay: float = 4.0) -> bool:
    """Poll the bench on the station LAN until any HTTP(S) response (it's up)."""
    for _ in range(tries):
        for scheme in ("https", "http"):
            try:
                r = httpx.get(
                    f"{scheme}://{ip}/",
                    verify=False,
                    timeout=3.0,
                    follow_redirects=False,
                )
                if r.status_code in (200, 301, 401):
                    return True
            except httpx.HTTPError:
                pass
        time.sleep(delay)
    return False


@pytest.mark.state_change
@pytest.mark.ap_mode
def test_provision_ap_to_sta(provisioned_sta, bench_ip) -> None:
    """AP-first onboarding acceptance — the AP→STA transition of the lifecycle.

    The `provisioned_sta` fixture does the real work (join the SoftAP, POST the
    station creds on /wifi, wait for the box to reboot into STA + come back
    MQTT-online, discover its DHCP IP). This test asserts the box actually
    reappeared on the station LAN and serves HTTP(S) there — provisioning
    succeeded end-to-end. It's the boundary between the AP phase and the STA
    phase: it runs after the @ap_mode suite and before the @sta_mode suite.
    """
    if provisioned_sta is None:
        pytest.skip(
            "provisioning not configured — set CB_PROVISION_SSID + "
            "CB_PROVISION_PSK (the station-LAN creds the box should join)"
        )
    assert _bench_reachable(bench_ip), (
        f"bench never reappeared on the station LAN at {bench_ip} after "
        "provisioning — candidate may have reverted (bad creds / broker "
        "unreachable / boot crash)."
    )
