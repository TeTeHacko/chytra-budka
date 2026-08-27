"""
test_mode_fsm — exercise the power_profile NVS selector.

The bench has no battery, so the SOC ladder seeds Active by default.
`cfg("power_profile", <name>)` pins a tier regardless of SOC; name ∈
{auto, max, active, eco, sentinel, hibernate}.

power_profile is an HA *select* entity (app_config format_value emits the
named label so the select shows a valid option), so its state/cfg echo is the
NAME, not the int. cfg() matches the echo against what it published, so we MUST
publish the named label — passing an int would set the tier but echo the name
and time out. (The setter accepts either form on input; the echo is always the
name.)

These tests cycle through the AWAKE tiers and verify state/profile flips on the
wire. `hibernate` is deliberately NOT exercised: it deep-sleeps the SoC, which
would drop the bench off the bus mid-session. Restores `active` at the end so a
CI chain leaves the bench awake + reachable (not light-sleeping in
eco/sentinel, which would add DTIM latency to later tests).
"""

from __future__ import annotations

import time

# Awake tiers only (hibernate excluded — it would deep-sleep the bench).
PROFILE_VALUES = ["max", "active", "eco", "sentinel"]


def test_power_profile_cycles_through_awake_tiers(mqtt_rec, cfg, bench_id):
    """Force each awake tier, verify retained state/profile matches.

    The cfg() helper itself confirms the NVS echo on state/cfg/power_profile;
    this test additionally checks the resolved-tier output topic state/profile
    reflects the forced tier within ~2 ticks (1 s each by design).

    Uses retained-value check, not wait-for-publish, because profile_tick only
    republishes state/profile on actual *transitions*. If the bench is already
    in the target tier, re-applying it produces no publish but the retained
    value is still correct.
    """
    try:
        for expected in PROFILE_VALUES:
            cfg("power_profile", expected)  # publish the NAMED label (echo = name)
            # profile_tick runs ~1 Hz; allow two ticks + a margin. Eco/Sentinel
            # light-sleep, which adds sub-second DTIM latency — 2.5 s covers it.
            time.sleep(2.5)
            latest = mqtt_rec.latest(f"{bench_id}/state/profile")
            assert latest is not None, "no retained state/profile payload — broker may have lost it"
            assert latest.decode().lower() == expected, (
                f"power_profile={expected}: expected state/profile={expected!r}, "
                f"got {latest.decode().lower()!r}"
            )
    finally:
        # Leave the bench in an awake, non-sleeping tier so the next test
        # doesn't inherit eco/sentinel light-sleep latency.
        cfg("power_profile", "active")


def test_power_profile_auto_hands_back_to_ladder(mqtt_rec, cfg, bench_id):
    """Releasing the selector to `auto` hands control back to the SOC ladder.

    SOC-agnostic by design — the bench may or may not have a MAX17048 wired:
      * With a gauge, `auto` lets profile_tick pick the SOC-appropriate AWAKE
        tier (max/active/eco/sentinel — never hibernate, since soc_hib_en=0).
      * Without one (SOC<0), the no-battery branch keeps the last tier.
    Either way the resolved tier must settle to a valid AWAKE tier and never to
    hibernate (which would deep-sleep the bench out of the session).
    """
    awake = {"max", "active", "eco", "sentinel"}
    try:
        cfg("power_profile", "max")  # force a known starting tier (named label)
        mqtt_rec.wait_for(
            f"{bench_id}/state/profile",
            lambda p: p.decode().lower() == "max",
            timeout=4.0,
        )

        cfg("power_profile", "auto")
        time.sleep(4.0)  # let a couple of ladder ticks settle
        latest = mqtt_rec.latest(f"{bench_id}/state/profile")
        assert latest is not None, "no retained state/profile after auto"
        tier = latest.decode().lower()
        assert tier in awake, (
            f"auto resolved to {tier!r}; expected an awake tier {sorted(awake)} "
            "(auto must never self-hibernate with soc_hib_en=0)"
        )
    finally:
        cfg("power_profile", "active")
