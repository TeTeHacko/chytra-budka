"""HIL: runtime web-admin (HTTP basic-auth) credentials + the gate.

Exercises auth_store + basic_auth_gate against the live bench over HTTPS (the
gate is HTTPS-only) and cmd/auth over MQTT. The out-of-box login is cb/cb
(secrets.h); tests that change it restore the default via cmd/auth
{"reset":true} so the bench is left clean.

Needs the bench enrolled (HTTPS on :443) and the broker reachable. The `https`
fixture skips if HTTPS isn't up; `http_basic_creds` skips if the creds are
placeholders (gate disabled).

@state_change tests perturb the live login — bench only.
"""

from __future__ import annotations

import time

import pytest


def _selftest_status(https, auth) -> int:
    """GET a gated endpoint; return the HTTP status (401 = gate blocked)."""
    return https.get("/selftest", auth=auth, timeout=10.0).status_code


def test_gate_blocks_then_allows(https, http_basic_creds) -> None:
    """Sensitive endpoint: 401 unauthenticated, 200 with the current creds."""
    u, p = http_basic_creds["user"], http_basic_creds["password"]
    assert _selftest_status(https, None) == 401, "gate must block unauthenticated"
    assert _selftest_status(https, (u, p)) == 200, "current creds must pass"


def test_confirm_mismatch_rejected(https, http_basic_creds) -> None:
    """A change whose two password fields differ is rejected, gate unchanged."""
    u, p = http_basic_creds["user"], http_basic_creds["password"]
    r = https.post(
        "/config",
        auth=(u, p),
        data={"auth_user": "hilx", "auth_pass": "abcdef1", "auth_pass2": "abcdef2"},
        timeout=10.0,
    )
    assert r.status_code == 200
    assert "match" in r.text.lower(), r.text[:200]
    assert _selftest_status(https, (u, p)) == 200, "creds must be unchanged"


def test_weak_password_rejected(https, http_basic_creds) -> None:
    """A <6-char password is rejected, gate unchanged."""
    u, p = http_basic_creds["user"], http_basic_creds["password"]
    r = https.post(
        "/config",
        auth=(u, p),
        data={"auth_user": "hilx", "auth_pass": "abc", "auth_pass2": "abc"},
        timeout=10.0,
    )
    assert r.status_code == 200
    low = r.text.lower()
    assert ("weak" in low) or ("at least 6" in low) or ("slab" in low), r.text[:200]
    assert _selftest_status(https, (u, p)) == 200, "creds must be unchanged"


def _reset_login(mqtt_rec, bench_id: str) -> None:
    """Restore the default login via cmd/auth (no HTTP auth needed)."""
    st = f"{bench_id}/state/auth"
    t0 = time.time()
    mqtt_rec._client.publish(f"{bench_id}/cmd/auth", '{"reset":true}', qos=1)
    mqtt_rec.wait_for(st, lambda b: b"reset" in b, timeout=10.0, since=t0)
    time.sleep(0.5)


@pytest.mark.state_change
def test_runtime_change_via_config(https, http_basic_creds, mqtt_rec, bench_id) -> None:
    """POST /config sets a new login, applied LIVE (no reboot): new creds pass,
    old default 401s. Restored via cmd/auth reset."""
    u, p = http_basic_creds["user"], http_basic_creds["password"]
    new_u, new_p = "hil_admin", "hil_pass_99"
    r = https.post(
        "/config",
        auth=(u, p),
        data={"auth_user": new_u, "auth_pass": new_p, "auth_pass2": new_p},
        timeout=10.0,
    )
    assert r.status_code == 200 and "saved" in r.text.lower(), r.text[:200]
    try:
        assert _selftest_status(https, (new_u, new_p)) == 200, "new creds must pass"
        assert _selftest_status(https, (u, p)) == 401, "old default must be rejected"
    finally:
        _reset_login(mqtt_rec, bench_id)
    assert _selftest_status(https, (u, p)) == 200, "default restored after reset"


@pytest.mark.state_change
def test_change_via_cmd_auth(https, http_basic_creds, mqtt_rec, bench_id) -> None:
    """cmd/auth {"user","pass"} sets the login (state/auth echo), gate honours
    it live, then reset restores the default."""
    u, p = http_basic_creds["user"], http_basic_creds["password"]
    st = f"{bench_id}/state/auth"
    t0 = time.time()
    mqtt_rec._client.publish(f"{bench_id}/cmd/auth", '{"user":"mq","pass":"mqtt_pw_77"}', qos=1)
    mqtt_rec.wait_for(st, lambda b: b"set" in b, timeout=10.0, since=t0)
    try:
        assert _selftest_status(https, ("mq", "mqtt_pw_77")) == 200
        assert _selftest_status(https, (u, p)) == 401
    finally:
        _reset_login(mqtt_rec, bench_id)
    assert _selftest_status(https, (u, p)) == 200, "default restored after reset"
