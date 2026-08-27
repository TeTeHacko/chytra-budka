"""
test_stress_soak — concurrency + soak stress against a powered bench (opt-in).

Reproduces the load that broke the fleet "since BLE" and gauges the fixes:

  * task_wdt busy-spin in /mic.wav (FIXED in v0.4.5) — under concurrent load the
    board must NOT reboot. The hard assertion is "no reboot": detected via the
    state/uptime_s telemetry resetting, NOT via the MQTT availability LWT — under
    load the MQTT link itself drops ('offline') WITHOUT the board rebooting, so
    availability is the wrong tripwire (an early version of this test false-failed
    on exactly that).
  * esp-aes "Failed to allocate memory" / MQTT write-timeout — BLE-on internal-
    DRAM pressure degrades TLS under concurrent handshakes. That degradation
    (tls_err + mqtt-drop counts) is REPORTED, not failed — it's the before/after
    gauge for the HW-AES change. Each request forces a fresh TLS handshake
    (Connection: close) to maximise crypto churn; auth-gated endpoints (401) still
    complete the full TLS+AES handshake, so web-admin creds aren't required.

Opt-in (long + hammers real hardware). Does NOT factory-reset; run with
CB_HIL_NO_RESET=1 so the autouse lifecycle stays a no-op:

  CB_STRESS=1 CB_HIL_NO_RESET=1 CB_BENCH_IP=198.51.100.90 \
      .venv/bin/python -m pytest test_stress_soak.py -m stress -s

Env: CB_STRESS_SECS (120), CB_STRESS_WORKERS (8), CB_BENCH_IP, CB_MAX_TLS_ERR_PCT
(soft ceiling for the degradation report; default 100 = report only, never fail).
"""

from __future__ import annotations

import concurrent.futures as cf
import itertools
import os
import threading
import time

import httpx
import pytest

pytestmark = [
    pytest.mark.stress,
    pytest.mark.skipif(
        not os.environ.get("CB_STRESS"),
        reason="soak/stress is opt-in — set CB_STRESS=1 to run",
    ),
]

SECS = int(os.environ.get("CB_STRESS_SECS", "120"))
WORKERS = int(os.environ.get("CB_STRESS_WORKERS", "8"))
BENCH_IP = os.environ.get("CB_BENCH_IP", "198.51.100.90")
MAX_TLS_ERR_PCT = float(os.environ.get("CB_MAX_TLS_ERR_PCT", "100"))
# Web-admin creds so the heavy endpoints transfer REAL bulk TLS payloads
# (streaming PCM / a fresh JPEG) — that bulk AES is what stresses the HW-AES
# DMA buffer. Without creds they 401 after the handshake and never transfer
# (which under-stresses AES — the gap in the first soak). Set on the bench via
# cmd/auth. Empty user → unauthenticated (handshake-only) fallback.
AUTH_USER = os.environ.get("CB_STRESS_USER", "hil")
AUTH_PASS = os.environ.get("CB_STRESS_PASS", "hilstress2026")
AUTH = (AUTH_USER, AUTH_PASS) if AUTH_USER else None

# Bulk-heavy: /mic.wav streams PCM for the whole window (sustained bulk AES),
# /capture pushes a fresh ~100 KB JPEG over TLS (1-at-a-time → others 503),
# / and /photos add handshake churn. Each request = a fresh TLS handshake.
PATHS = ["/mic.wav?max=4", "/capture", "/", "/photos", "/mic.wav?max=2"]


def _uptime_samples(mqtt_rec, bench_id, since=0.0):
    """All (ts, uptime_s) seen on state/uptime_s since `since`, oldest first."""
    topic = f"{bench_id}/state/uptime_s"
    out = []
    with mqtt_rec._lock:
        for t, payload, ts in mqtt_rec._messages:
            if t == topic and ts >= since:
                try:
                    out.append((ts, int(payload.decode().strip())))
                except (ValueError, UnicodeDecodeError):
                    pass
    return out


def test_stress_soak(mqtt_rec, bench_id):
    start = time.time()
    # Baseline uptime — wait up to one telemetry tick for a fresh sample.
    base_up = None
    for _ in range(70):
        s = _uptime_samples(mqtt_rec, bench_id, since=start)
        if s:
            base_up = s[-1][1]
            break
        time.sleep(1.0)

    stop = threading.Event()
    stats = {"ok": 0, "tls_err": 0, "other_err": 0, "by_code": {}}
    lock = threading.Lock()

    def worker() -> None:
        while not stop.is_set():
            for p in PATHS:
                if stop.is_set():
                    return
                try:
                    with httpx.Client(
                        base_url=f"https://{BENCH_IP}",
                        verify=False,
                        timeout=30.0,
                        headers={"Connection": "close"},
                        auth=AUTH,
                    ) as c:
                        r = c.get(p)
                    with lock:
                        stats["ok"] += 1
                        stats["by_code"][r.status_code] = stats["by_code"].get(r.status_code, 0) + 1
                except (
                    httpx.ConnectError,
                    httpx.ConnectTimeout,
                    httpx.ReadError,
                    httpx.ReadTimeout,
                    httpx.RemoteProtocolError,
                ):
                    with lock:
                        stats["tls_err"] += 1
                except Exception:
                    with lock:
                        stats["other_err"] += 1

    # Count MQTT availability 'offline' blips during the soak — a degradation
    # signal (link dropped under load), NOT a reboot.
    def mqtt_drops():
        topic = f"{bench_id}/state/availability"
        with mqtt_rec._lock:
            return sum(
                1
                for (t, p, ts) in mqtt_rec._messages
                if t == topic and ts >= start and p == b"offline"
            )

    with cf.ThreadPoolExecutor(max_workers=WORKERS) as ex:
        futs = [ex.submit(worker) for _ in range(WORKERS)]
        time.sleep(SECS)
        stop.set()
        for f in futs:
            f.result(timeout=40)

    # Let MQTT reconnect + publish a post-soak uptime sample.
    soak_end = time.time()
    end_up = None
    for _ in range(90):
        s = _uptime_samples(mqtt_rec, bench_id, since=soak_end)
        if s:
            end_up = s[-1][1]
            break
        time.sleep(1.0)

    # Reboot tripwire: uptime reset. A reboot drops uptime_s far below the
    # pre-soak value; a healthy board's uptime only grew.
    all_up = _uptime_samples(mqtt_rec, bench_id, since=start)
    rebooted = False
    if base_up is not None and end_up is not None:
        rebooted = end_up < base_up
    elif len(all_up) >= 2:
        rebooted = any(b < a - 30 for (_, a), (_, b) in itertools.pairwise(all_up))

    total = stats["ok"] + stats["tls_err"] + stats["other_err"]
    tls_pct = (100.0 * stats["tls_err"] / total) if total else 0.0
    final = httpx.get(f"https://{BENCH_IP}/", verify=False, timeout=10.0)
    print(
        f"\n[stress] {SECS}s x{WORKERS}w: req={total} ok={stats['ok']} "
        f"tls_err={stats['tls_err']} ({tls_pct:.0f}%) other={stats['other_err']} "
        f"codes={stats['by_code']} mqtt_drops={mqtt_drops()} "
        f"uptime {base_up}->{end_up} rebooted={rebooted}"
    )

    # HARD: no crash/reboot under load, and still serving.
    assert not rebooted, (
        f"board rebooted during soak (uptime {base_up}->{end_up}) — crash regression"
    )
    assert final.status_code == 200, f"board not serving after soak: {final.status_code}"
    # SOFT degradation ceiling (default 100% = report-only).
    assert tls_pct <= MAX_TLS_ERR_PCT, (
        f"TLS error rate {tls_pct:.0f}% exceeds ceiling {MAX_TLS_ERR_PCT:.0f}%"
    )
