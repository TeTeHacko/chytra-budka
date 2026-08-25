"""
test_audio — exercise the audio subsystem for the ONE HW variant currently
wired on the bench.

Audio output is hardware-shaped (see audio_matrix.toml): which pads carry the
buzzer / PCM "1-bit DAC", and what's hooked to them, changes between sessions.
So you DECLARE what's wired and only that variant runs:

    CB_HIL_NO_RESET=1 CB_BENCH_IP=<ip> CB_AUDIO_WIRED=both \
        pytest -m manual firmware/tests/hil/test_audio.py

Every other variant SKIPS at collection ("not the wired variant") — so once a
variant is tested and unplugged you just stop selecting it; the suite never
reconfigures a board for hardware that isn't there.

What's AUTOMATED (the firmware contract):
  • the declared pin map applies and each backend initialises on the expected
    GPIO (read back from /selftest: audio_buzzer_gpio / audio_pcm_gpio);
  • every sound command is accepted without crashing or wedging the board;
  • the mic / camera / hot-path tasks keep working alongside (coexistence),
    and a photo still captures after the audio barrage.

What is NOT automatable here: whether it actually makes *sound* — the HIL host
has no microphone. The test prints "▶ LISTEN" cues so you can sign off by ear;
the asserts only cover the firmware side.

Marked @manual (needs wiring + an ear) and @state_change (writes the pin map +
reboots). Run against an already-provisioned bench with CB_HIL_NO_RESET=1 (and
CB_BENCH_IP so it doesn't try to re-provision over the SoftAP just to poke audio).
"""

from __future__ import annotations

import os
import time
import tomllib
from pathlib import Path

import httpx
import pytest

HERE = Path(__file__).resolve().parent
MATRIX = tomllib.load((HERE / "audio_matrix.toml").open("rb"))
VARIANTS: dict[str, dict] = MATRIX["variants"]

# XIAO ESP32-S3 breakout pad → GPIO (matches app_config.c PIN_SLOT_GPIO).
PAD_GPIO = {"d0": 1, "d1": 2, "d2": 3, "d3": 4, "d4": 5, "d5": 6, "d6": 43, "d7": 44}

WIRED = os.environ.get("CB_AUDIO_WIRED", "").strip()
REBOOT_WINDOW = 120.0


def _variant_params():
    """One param per variant; all but the wired one skip at collection (so a
    non-wired variant never spins up the bench fixtures just to skip)."""
    params = []
    for v in VARIANTS:
        if not WIRED:
            marks = pytest.mark.skip(
                reason=f"declare the wired variant: CB_AUDIO_WIRED=<{'|'.join(VARIANTS)}>"
            )
        elif v != WIRED:
            marks = pytest.mark.skip(reason=f"not the wired variant (CB_AUDIO_WIRED={WIRED!r})")
        else:
            marks = ()
        params.append(pytest.param(v, marks=marks, id=v))
    return params


def _expected_gpio(variant: str, fn: str) -> int:
    pad = VARIANTS[variant].get(fn)
    return PAD_GPIO[pad] if pad else -1


def _apply_pinmap(cfg, mqtt_rec, bench_id: str, variant: str) -> None:
    """Set the pin map to the declared variant. First free any pad that
    currently holds buzzer/pcm but isn't a target — they're singleton pin
    functions, so a leftover mapping from a prior variant would make the new
    set fail the cross-validation."""
    targets = {VARIANTS[variant][fn]: fn for fn in ("buzzer", "pcm") if VARIANTS[variant].get(fn)}
    for pad in PAD_GPIO:
        cur = mqtt_rec.latest(f"{bench_id}/state/cfg/pin_{pad}_fn")
        cur = cur.decode().strip() if cur else None
        if cur in ("buzzer", "pcm") and targets.get(pad) != cur:
            cfg(f"pin_{pad}_fn", "none")
    for pad, fn in targets.items():
        cfg(f"pin_{pad}_fn", fn)


def _reboot_and_wait(mqtt_rec, bench_id: str) -> None:
    """Reboot so speaker_init/pcm_init re-read the pin map, wait for the
    post-reboot availability=online (not the stale retained one)."""
    sent = time.time()
    mqtt_rec._client.publish(f"{bench_id}/cmd/reboot", "1", qos=1, retain=False)
    mqtt_rec.wait_for(
        f"{bench_id}/state/availability",
        lambda p: p.strip() == b"online",
        timeout=REBOOT_WINDOW,
        since=sent,
    )
    time.sleep(2)  # let the module inits + first selftest publish settle


def _selftest(http: httpx.Client, *, tries: int = 8) -> dict:
    """GET /selftest as JSON, retrying while HTTPS comes back up post-reboot."""
    last: Exception | None = None
    for _ in range(tries):
        try:
            r = http.get("/selftest", timeout=10.0)
            if r.status_code == 200:
                return r.json()
        except httpx.HTTPError as e:
            last = e
        time.sleep(3)
    raise AssertionError(f"/selftest never returned 200 ({last})")


@pytest.mark.manual
def test_audio_wired_is_known():
    """Guard against a typo in CB_AUDIO_WIRED. @manual like the rest of this
    file so the whole audio suite is excluded from the unattended OTA HIL gate
    (`-m "not manual"`) — audio needs wiring + an ear, it can't gate."""
    if WIRED and WIRED not in VARIANTS:
        pytest.fail(
            f"CB_AUDIO_WIRED={WIRED!r} is not a variant in audio_matrix.toml "
            f"({', '.join(VARIANTS)})"
        )


@pytest.mark.manual
@pytest.mark.state_change
@pytest.mark.parametrize("variant", _variant_params())
def test_audio_wired_variant(variant, cfg, mqtt_rec, bench_id, http):
    spec = VARIANTS[variant]
    print(f"\n=== audio variant {variant!r}: {spec.get('desc', '')} ===")

    # 1) apply the declared pin map, reboot so the modules pick it up.
    _apply_pinmap(cfg, mqtt_rec, bench_id, variant)
    _reboot_and_wait(mqtt_rec, bench_id)

    # 2) each backend must have initialised on the expected GPIO (or be -1).
    st = _selftest(http)
    exp_b = _expected_gpio(variant, "buzzer")
    exp_p = _expected_gpio(variant, "pcm")
    assert st.get("audio_buzzer_gpio") == exp_b, (
        f"buzzer GPIO {st.get('audio_buzzer_gpio')} != expected {exp_b} "
        f"— pin map didn't apply or speaker_init failed"
    )
    assert st.get("audio_pcm_gpio") == exp_p, (
        f"pcm GPIO {st.get('audio_pcm_gpio')} != expected {exp_p} "
        f"— pin map didn't apply or pcm_init failed"
    )
    print(f"  ✓ backends up: buzzer_gpio={exp_b} pcm_gpio={exp_p}")

    # 3) drive every sound path through the audiofx router. LISTEN cues for the
    #    by-ear pass; the assert is only that the board survives each command.
    for topic, payload, human in [
        ("cmd/beep", "1000,250", "1 kHz beep"),
        ("cmd/melody", "523:150,659:150,784:200,1047:300", "4-note melody"),
        ("cmd/sfx", "988:80,1319:350", "coin sfx (legato)"),
        ("cmd/pcm", "coin", "recorded coin sample (pcm) + chiptune (buzzer)"),
        ("cmd/pcm", "test", "1 kHz sine on pcm + tone on buzzer (RC tuning)"),
    ]:
        mqtt_rec._client.publish(f"{bench_id}/{topic}", payload, qos=1)
        print(f"  ▶ LISTEN: {human}")
        time.sleep(3.0)
    mqtt_rec._client.publish(f"{bench_id}/cmd/pcm", "stop", qos=1)
    time.sleep(0.5)

    # 4) loop-guard: a capture during an alarm loop must neither wedge the board
    #    nor (by ear) make the loop collect coins / burst when it stops.
    mqtt_rec._client.publish(f"{bench_id}/cmd/alarm", "880:200,988:200", qos=1)
    print(
        "  ▶ LISTEN: alarm loop — the next photo must NOT add a coin, and there's"
        " no coin burst when it stops"
    )
    time.sleep(1.5)
    mqtt_rec._client.publish(f"{bench_id}/cmd/photo", "1", qos=1)
    time.sleep(2.5)
    mqtt_rec._client.publish(f"{bench_id}/cmd/alarm", "stop", qos=1)
    time.sleep(1.5)

    # 5) capture beep + coexistence: a photo still fires its event, and the mic /
    #    camera / hot-path tasks are still healthy after the audio barrage.
    sent = time.time()
    mqtt_rec._client.publish(f"{bench_id}/cmd/photo", "1", qos=1)
    print("  ▶ LISTEN: capture beep (coin) on this photo")
    mqtt_rec.wait_for(
        f"{bench_id}/event/photo",
        lambda p: p.startswith(b"{"),
        timeout=20.0,
        since=sent,
    )  # raises TimeoutError → test fails if the camera path broke

    st2 = _selftest(http)
    for sub in ("mic", "audio_task", "camera", "wifi", "mqtt"):
        assert st2.get(sub) is True, (
            f"{sub} unhealthy after the audio barrage — {st2.get('summary')!r}"
        )
    assert st2.get("audio_buzzer_gpio") == exp_b
    assert st2.get("audio_pcm_gpio") == exp_p
    print(f"  ✓ coexistence ok: {st2.get('summary')}")
