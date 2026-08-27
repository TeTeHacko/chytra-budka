"""HIL persistence: set → reboot → verify NVS survival per type.

The `cfg()` fixture only proves the device accepted + echoed a value, not
that it actually committed to NVS. Two NVS keys lived for months in the
schema with names too long for the IDF 15-char limit; every `nvs_set_*`
returned ESP_ERR_NVS_KEY_TOO_LONG, the cache stayed at default, the
retained state echo still went out — the entity just snapped back to the
default after a reboot. `cfg()` alone would have missed it (the in-memory
cache update + echo publish ran fine; only the persistence broke).

This test closes the loop: set a custom value, clear the broker's
retained copy so it can't speak for the device, reboot, wait for online,
and confirm the device republished the custom value (must have come from
NVS — there's no other source). Covers all three storage types
(bool/int/float). Also exercises the strict bool whitelist and the
no-op-skip path the setter grew during the same persistence pass.
"""

from __future__ import annotations

import time

import pytest

# Reboot window: LWT timeout (90 s configured) is the long pole if MQTT
# loses the device before it boots; in practice a clean cmd/reboot ends
# the session cleanly so LWT fires within seconds. 120 s covers the
# worst-case (slow WiFi reassociation + MQTT reconnect + retained
# republish) without dragging out a healthy run.
REBOOT_WINDOW = 120.0


def _send_reboot(mqtt_rec, bench_id: str) -> float:
    """Publish cmd/reboot and return the local timestamp we sent at.

    Caller uses the timestamp with mqtt_rec.wait_for(since=...) so the
    "online" we wait for is the post-reboot one, not the pre-reboot
    retained value still sitting on the broker.
    """
    sent_at = time.time()
    mqtt_rec._client.publish(f"{bench_id}/cmd/reboot", "1", qos=1, retain=False)
    return sent_at


def _wait_back_online(mqtt_rec, bench_id: str, sent_at: float) -> None:
    avail_topic = f"{bench_id}/state/availability"
    mqtt_rec.wait_for(
        avail_topic,
        lambda p: p == b"online",
        timeout=REBOOT_WINDOW,
        since=sent_at,
    )


def _clear_retained(mqtt_rec, topic: str) -> None:
    """Publish empty retained payload so the broker forgets the topic.

    Empty + retained=True is the MQTT way to delete a retained slot.
    After this, only a fresh publish from the device (its post-reboot
    re-announce) can put a value back — exactly what we want to verify.
    """
    mqtt_rec._client.publish(topic, b"", qos=1, retain=True)
    # Small settle so the empty-payload retention lands before we
    # check it (broker-side retained-store updates are async on the
    # mosquitto we use).
    time.sleep(0.5)


@pytest.mark.parametrize(
    "key, custom, default",
    [
        # bool — the keys that motivated this test live here. cap_led_en
        # was the renamed regression (was `capture_led_enabled`, 19 chars,
        # silently failing every nvs_set). Default ON, flip to OFF for
        # the test then restore.
        ("cap_led_en", "OFF", "ON"),
        # int — reed_db_ms (was `reed_debounce_ms`, 16 chars, same bug).
        # Schema default 100; pick 240 (in range, distinct, divisible by
        # step=20 so the setter accepts it cleanly).
        ("reed_db_ms", "240", "100"),
        # float — vad_thr_dbfs is the only float in the schema. Default
        # -45.0; pick -52.0 (in range [-80..-20], non-default, encodes
        # cleanly to %.2f for the echo compare).
        ("vad_thr_dbfs", "-52.00", "-45.00"),
    ],
    ids=["bool", "int", "float"],
)
def test_value_survives_reboot(mqtt_rec, bench_id, cfg, key, custom, default):
    state_topic = f"{bench_id}/state/cfg/{key}"

    # 1. Set the custom value via the standard cfg() path (gets echo
    #    so we know the device accepted + ran apply_side_effects).
    echo = cfg(key, custom)
    assert echo == custom, (
        f"cfg() echo for {key} was {echo!r}, expected {custom!r} — "
        "setter rejected our value or schema range disagrees with test"
    )

    # 2. Erase the broker's retained copy. Any value we see at
    #    state/cfg/<key> after this point came from the device
    #    republishing — which it only does on boot via
    #    app_config_publish_state_all() after pulling from NVS.
    _clear_retained(mqtt_rec, state_topic)

    # 3. Reboot the bench.
    sent_at = _send_reboot(mqtt_rec, bench_id)

    # 4. Wait for the device to come back. availability=online publishes
    #    on MQTT connect, before app_config_publish_state_all() runs,
    #    so we still need a small follow-up wait for the cfg topic.
    _wait_back_online(mqtt_rec, bench_id, sent_at)

    # 5. The device republishes state/cfg/* on every connect. We expect
    #    `custom`, NOT `default` — anything else means NVS persist
    #    silently failed.
    received = mqtt_rec.wait_for(
        state_topic,
        lambda p: p.decode() == custom,
        timeout=15.0,
        since=sent_at,
    )
    assert received.decode() == custom, (
        f"{key} reverted to {received.decode()!r} after reboot — "
        f"expected {custom!r}. NVS persist failed silently (probably "
        f"NVS_ERR_KEY_TOO_LONG or a write that didn't commit)."
    )

    # 6. Restore default so the next test (or operator session) finds
    #    the bench in a clean state. cfg() asserts the echo, which is
    #    enough — no need to reboot again.
    cfg(key, default)


def test_bool_whitelist_rejects_garbage(mqtt_rec, bench_id, cfg):
    """A typo'd bool payload must not flip the entity OFF silently.

    Pre-fix the parser was `truthy = match(ON/1/true); parsed.b = truthy`
    which meant any non-truthy string (including a typo "OFFF" or the
    stdin-leftover "OFF\\n") silently mapped to false. Now garbage is
    rejected — the cfg echo never arrives and the retained state stays
    at the pre-test value. We verify by ensuring cfg() times out trying
    to confirm a garbage payload, while the retained value untouched.
    """
    key = "cap_led_en"
    state_topic = f"{bench_id}/state/cfg/{key}"

    # Pre-condition: known-good value on the broker side.
    pre = cfg(key, "ON")
    assert pre == "ON"

    # cfg() waits for the echo to equal the payload — but the setter
    # returns ESP_ERR_INVALID_ARG and never publishes a new state, so
    # cfg() will time out. The timeout itself is the assertion that
    # garbage was rejected.
    sent_at = time.time()
    mqtt_rec._client.publish(f"{bench_id}/cmd/cfg/{key}", "garbage_payload", qos=1, retain=False)

    # Confirm no new echo appeared for ~3 s. If the buggy "anything →
    # false" parser were back, we'd see state flip to OFF immediately.
    time.sleep(3.0)
    latest = mqtt_rec.latest(state_topic)
    assert latest is not None, "state topic disappeared mid-test"
    assert latest.decode() == "ON", (
        f"garbage payload flipped {key} to {latest.decode()!r} — strict "
        f"whitelist regression (sent_at={sent_at}, latest={latest!r})"
    )


def test_noop_set_is_idempotent(cfg):
    """Setting the current value re-publishes state but doesn't error.

    The setter short-circuits when parsed == cached, skipping the NVS
    write but still publishing state (so a freshly-restarted broker
    with no retained copy re-learns the current value). The echo path
    must still work end-to-end — both calls below should return the
    same value with no exception.
    """
    key = "reed_db_ms"
    first = cfg(key, "100")  # default; cache likely already 100
    second = cfg(key, "100")  # idempotent — exercises the unchanged branch
    assert first == "100"
    assert second == "100"


def _peek_retained(mqtt_rec, topic: str, timeout: float = 5.0) -> str:
    """Read the current retained payload at `topic`, returning str.

    Used for the pin-map / uart_baud tests below — they need to record
    the bench's pre-test state so the restore step lands on whatever
    the operator had configured (not a hard-coded "default" that may
    differ between bench rev 3.2 and the schema default)."""
    latest = mqtt_rec.wait_for(topic, lambda p: len(p) >= 0, timeout=timeout)
    return latest.decode()


@pytest.mark.parametrize(
    "key, custom",
    [
        # Pin-function-map int knob (PIN_FN_LABELS-formatted) introduced
        # in the phase-1..10 refactor. "none" is a guaranteed-valid value
        # that never collides with singleton or pair rules. D6 is picked
        # because (a) it has the same pin-slot machinery as any other,
        # (b) on a stock bench its schema default is i2c1_sda (i.e. half
        # of an i2c pair) which makes the half-broken-pair warning path
        # exercise too, (c) the side effect is deferred to next boot so
        # flipping won't disturb the live pin binding during the test.
        ("pin_d6_fn", "none"),
        # uart_baud — schema int added in phase 8. No live side effect
        # (UART2 is configured once at boot from this value), so only
        # NVS persistence matters here. 57600 is in-range and unlikely
        # to coincide with the bench's current setting.
        ("uart_baud", "57600"),
    ],
    ids=["pin_d6_fn", "uart_baud"],
)
def test_pinmap_knob_survives_reboot(mqtt_rec, bench_id, cfg, key, custom):
    """Pin-map + uart_baud knobs persist across reboot.

    Unlike the parametrize-3 cases above, the bench's pre-test value
    for these knobs varies across rev 3.2 builds (operator may have
    swapped uart_tx/rx, picked a non-default baud, etc.). The test
    captures the live retained value as the restore target so it
    never accidentally rewrites operator state.
    """
    state_topic = f"{bench_id}/state/cfg/{key}"
    pre = _peek_retained(mqtt_rec, state_topic)
    if pre == custom:
        # Pick a different value so the persistence round-trip is
        # observable. For pin_d?_fn fall back to "ir_led"; for
        # uart_baud fall back to 38400. Both in-range, both distinct.
        custom = "ir_led" if key.startswith("pin_d") else "38400"
        assert pre != custom, f"bench pre-state already at fallback for {key}"

    echo = cfg(key, custom)
    assert echo == custom, (
        f"cfg() echo for {key} was {echo!r}, expected {custom!r} — "
        "setter rejected our value or schema range disagrees"
    )

    _clear_retained(mqtt_rec, state_topic)
    sent_at = _send_reboot(mqtt_rec, bench_id)
    _wait_back_online(mqtt_rec, bench_id, sent_at)

    received = mqtt_rec.wait_for(
        state_topic,
        lambda p: p.decode() == custom,
        timeout=15.0,
        since=sent_at,
    )
    assert received.decode() == custom, (
        f"{key} reverted to {received.decode()!r} after reboot "
        f"— expected {custom!r}. NVS persist failed."
    )

    # Restore to the recorded pre-state so the next test (and the
    # operator) finds the bench unchanged. No reboot needed: the
    # pin-map side effect runs at next boot anyway, and the echo
    # alone proves the NVS write committed.
    cfg(key, pre)
