"""
test_jpeg_sweep — characterize OV3660 across (framesize, quality) for
both still-capture and MJPEG-stream profiles, on every device listed
in jpeg_sweep_matrix.toml.

`@pytest.mark.manual` because it's slow (~10 min per profile per device)
and results need human review. Run explicitly:

    pytest -m manual firmware/tests/hil/test_jpeg_sweep.py
    pytest -m manual firmware/tests/hil/test_jpeg_sweep.py -k still
    pytest -m manual firmware/tests/hil/test_jpeg_sweep.py -k stream
    pytest -m manual firmware/tests/hil/test_jpeg_sweep.py -k bench
    pytest -m manual firmware/tests/hil/test_jpeg_sweep.py -k 'stream and bench'

Outputs land in `output/jpeg_sweep_<sha>/`:

    <device>/<profile>_fs<NN>_q<NN>_f<N>.jpg     all JPEGs
    summary.csv                                  device,profile,fs,q,frame,bytes,path
    index.html                                   static viewer (no server needed)

The viewer is pure HTML+CSS+vanilla-JS — open `index.html` from file://
in any browser; click images to zoom; filter checkboxes at the top
toggle which devices/profiles/framesizes/qualities are shown.
"""

from __future__ import annotations

import csv
import json
import os
import re
import socket
import subprocess
import threading
import time
import tomllib
from pathlib import Path
from typing import Any

import httpx
import paho.mqtt.client as mqtt
import pytest
from conftest import DEFAULT_MQTT_HOST, DEFAULT_MQTT_PORT, _mqtt_auth

# Opt-in, like the soak test. This is a CHARACTERISATION sweep, not a
# regression guard: 30 (framesize × quality) points × 3 frames × 2 variants is
# ~180 captures, which dominated the pre-deploy gate — for a knob the 2026-05
# sweep already showed to be nearly flat. Nothing here would catch a firmware
# regression that the rest of the suite misses; run it when tuning the camera.
#
#   CB_SWEEP=1 pytest test_jpeg_sweep.py
pytestmark = [
    pytest.mark.sweep,
    pytest.mark.skipif(
        not os.environ.get("CB_SWEEP"),
        reason="JPEG characterisation sweep is opt-in — set CB_SWEEP=1 to run",
    ),
]

HERE = Path(__file__).resolve().parent
MATRIX_FILE = HERE / "jpeg_sweep_matrix.toml"

with MATRIX_FILE.open("rb") as _f:
    MATRIX = tomllib.load(_f)

# Human-readable resolution per framesize enum (matches
# esp32-camera framesize_t). Pixel WxH is what an operator actually
# thinks about; the abbreviation (UXGA/SVGA/...) is a historical
# convention that doesn't tell you anything about the file size or
# bandwidth implications. Format kept as "WxH" everywhere — CSV,
# bandwidth table, viewer.
FRAMESIZE_NAMES: dict[int, str] = {
    0: "96×96",
    1: "160×120",
    2: "176×144",
    3: "240×176",
    4: "240×240",
    5: "320×240",
    6: "400×296",
    7: "480×320",
    8: "640×480",
    9: "800×600",
    10: "1024×768",
    11: "1280×720",
    12: "1280×1024",
    13: "1600×1200",
}

DEVICE_IDS = list(MATRIX["devices"].keys())
DEVICE_NAMES = [MATRIX["devices"][d].get("name", d) for d in DEVICE_IDS]


def _device_full_id(mactail: str) -> str:
    """Topic root for a matrix device: cb-<6hex>, matching conftest.bench_id.

    The default used to be "chytra-budka-<mactail>", the pre-2026-06-22 id
    format. Nothing has answered on those topics since the rename, so every
    sweep has been publishing config into the void and timing out waiting for
    an echo — the failure looked like a broker or ACL problem and was neither.
    """
    return MATRIX["devices"][mactail].get("id", f"cb-{mactail}")


# ── MQTT session-wide recorder ─────────────────────────────────────────────


class _SweepRecorder:
    """Like conftest._MqttRecorder but exposed for direct use here so the
    sweep can share one paho client + recorder across every parametrized
    test case (saves ~5s per case in connect/replay)."""

    def __init__(self) -> None:
        self._messages: list[tuple[str, bytes, float]] = []
        self._lock = threading.Lock()

    def _on_message(self, _c: Any, _u: Any, msg: mqtt.MQTTMessage) -> None:
        with self._lock:
            self._messages.append((msg.topic, msg.payload, time.time()))

    def wait_for(
        self,
        topic: str,
        predicate: Any = lambda _p: True,
        *,
        timeout: float = 10.0,
        since: float = 0.0,
    ) -> bytes:
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                for t, payload, ts in self._messages:
                    if t == topic and ts >= since and predicate(payload):
                        return payload
            time.sleep(0.05)
        raise TimeoutError(f"no msg on {topic!r} matching predicate within {timeout}s")

    def latest(self, topic: str) -> bytes | None:
        with self._lock:
            for t, payload, _ in reversed(self._messages):
                if t == topic:
                    return payload
        return None


@pytest.fixture(scope="session")
def sweep_mqtt(mqtt_creds: dict[str, str]):
    """Session-wide paho client subscribed to every configured device.

    Auth comes from conftest (_mqtt_auth). We do NOT subscribe to the broker
    wildcard (`chytra-budka-+/#`) because mosquitto rejects `+` as a
    substring of a level — see OBSERVABILITY.md § MQTT TUI gotcha.
    Explicit per-device subscriptions instead.
    """
    host = os.environ.get("CB_MQTT_HOST", DEFAULT_MQTT_HOST)
    port = int(os.environ.get("CB_MQTT_PORT", DEFAULT_MQTT_PORT))

    client = mqtt.Client(  # type: ignore[call-arg]
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,  # type: ignore[attr-defined]
        client_id=f"hil-sweep-{os.getpid()}-{int(time.time())}",
    )
    # Same auth path as every other fixture — certificate first (see conftest).
    _mqtt_auth(client)
    try:
        client.connect(host, port, keepalive=30)
    except (OSError, socket.gaierror) as e:
        pytest.skip(f"MQTT broker {host}:{port} unreachable: {e}")

    rec = _SweepRecorder()
    client.on_message = rec._on_message
    for mactail in DEVICE_IDS:
        client.subscribe(f"{_device_full_id(mactail)}/#")
    client.loop_start()
    time.sleep(0.7)  # broker replays retained messages on subscribe
    try:
        yield client, rec
    finally:
        client.loop_stop()
        client.disconnect()


@pytest.fixture(scope="session")
def device_ips(sweep_mqtt) -> dict[str, str]:
    """Resolve each device's IP — either from matrix toml or via state/ip."""
    _client, rec = sweep_mqtt
    out: dict[str, str] = {}
    deadline = time.time() + 6.0
    while time.time() < deadline:
        for mactail in DEVICE_IDS:
            if mactail in out:
                continue
            ip_override = MATRIX["devices"][mactail].get("ip")
            if ip_override:
                out[mactail] = ip_override
                continue
            payload = rec.latest(f"{_device_full_id(mactail)}/state/ip")
            if payload:
                out[mactail] = payload.decode().strip()
        if len(out) == len(DEVICE_IDS):
            break
        time.sleep(0.2)
    return out


# ── Output directory + record collector ────────────────────────────────────


@pytest.fixture(scope="session")
def sweep_output_dir() -> Path:
    try:
        sha = (
            subprocess.check_output(
                ["git", "-C", str(HERE), "rev-parse", "--short", "HEAD"],
                stderr=subprocess.DEVNULL,
                timeout=3,
            )
            .decode()
            .strip()
        )
    except Exception:
        sha = time.strftime("%Y%m%d_%H%M%S")
    d = HERE / "output" / f"jpeg_sweep_{sha}"
    d.mkdir(parents=True, exist_ok=True)
    return d


@pytest.fixture(scope="session")
def sweep_records() -> list[dict[str, Any]]:
    return []


@pytest.fixture(scope="session", autouse=True)
def _emit_summary_after_session(sweep_output_dir: Path, sweep_records: list[dict[str, Any]]):
    """Write summary.csv + bandwidth.md + index.html once all sweep tests have run."""
    yield
    if not sweep_records:
        return
    _write_summary(sweep_output_dir, sweep_records)
    agg = _aggregate(sweep_records)
    recos = _recommend(agg)
    _write_bandwidth_md(sweep_output_dir, agg, recos)
    _write_index(sweep_output_dir, sweep_records, agg, recos)
    print(f"\nsweep output: {sweep_output_dir}/index.html")
    print(f"bandwidth:    {sweep_output_dir}/bandwidth.md")


# ── helpers ────────────────────────────────────────────────────────────────


def _set_cfg(
    client: mqtt.Client,
    rec: _SweepRecorder,
    dev_id: str,
    key: str,
    value: int,
    *,
    timeout: float = 15.0,
    retries: int = 4,
) -> None:
    """Publish cmd/cfg/<key>; wait for state/cfg/<key> retained echo to match.

    Retries the publish/wait pair on timeout — when the firmware is mid-
    sensor-reconfigure (set_framesize + 2-frame FB drain can stall the
    MQTT task for ~500-700 ms at slow framesizes), the state echo lands
    later than usual. 15 s window per attempt × 4 retries (5 total
    attempts) tolerates worst-case stress without false failure. 2 s
    pause between retries gives the firmware breathing room.
    """
    payload = str(value)
    last_err: Exception | None = None
    for attempt in range(retries + 1):
        sent = time.time()
        client.publish(f"{dev_id}/cmd/cfg/{key}", payload, qos=1, retain=False)
        try:
            rec.wait_for(
                f"{dev_id}/state/cfg/{key}",
                lambda p, want=payload: p.decode().strip() == want,
                timeout=timeout,
                since=sent,
            )
            return
        except TimeoutError as e:
            last_err = e
            print(f"  retry {attempt + 1}/{retries}: cfg {key}={payload} echo timed out")
            time.sleep(2.0)
    raise last_err if last_err else RuntimeError("set_cfg retries exhausted")


def _restore_defaults(client: mqtt.Client, rec: _SweepRecorder, dev_id: str, profile: str) -> None:
    """Best-effort restore — never raise from teardown."""
    defs = MATRIX["defaults"]
    try:
        if profile == "still":
            _set_cfg(client, rec, dev_id, "cam_framesize", defs["cam_framesize"])
            _set_cfg(client, rec, dev_id, "cam_quality", defs["cam_quality"])
        else:
            _set_cfg(client, rec, dev_id, "mjpg_framesize", defs["mjpg_framesize"])
            _set_cfg(client, rec, dev_id, "mjpg_quality", defs["mjpg_quality"])
    except Exception as e:
        print(f"WARN: restore-defaults for {dev_id} {profile} failed: {e}")


def _grab_mjpeg_burst(
    ip: str, *, window_s: float = 5.0, keep_n: int = 3
) -> tuple[list[bytes], float, int]:
    """Open /stream.mjpg, read frames for `window_s` seconds. Returns
    (first keep_n frames, measured FPS, total frame count).

    FPS is computed across the whole window — accurate for a fixed-time
    burst. We keep only the first `keep_n` for storage but counting
    continues to window end so the FPS estimate isn't truncated.
    """
    # HTTPS: an enrolled board redirects :80 → :443, and httpx does not follow
    # redirects on a stream() unless told to. verify=False because the leaf is
    # signed by the private rfa CA. Same shape as the other HTTP fixtures.
    url = f"https://{ip}/stream.mjpg?max={int(window_s) + 5}"
    deadline = time.time() + window_s
    kept: list[bytes] = []
    total = 0
    first_frame_at: float | None = None
    last_frame_at: float | None = None
    boundary = b"--frame"
    with httpx.stream(
        "GET", url, timeout=httpx.Timeout(window_s + 10), verify=False, follow_redirects=True
    ) as r:
        buf = b""
        for chunk in r.iter_bytes():
            buf += chunk
            while True:
                idx = buf.find(boundary)
                if idx < 0:
                    break
                hdr_end = buf.find(b"\r\n\r\n", idx)
                if hdr_end < 0:
                    break
                header = buf[idx:hdr_end].decode("ascii", errors="ignore")
                m = re.search(r"Content-Length:\s*(\d+)", header)
                if not m:
                    break
                clen = int(m.group(1))
                body_start = hdr_end + 4
                if len(buf) < body_start + clen:
                    break
                now = time.time()
                if first_frame_at is None:
                    first_frame_at = now
                last_frame_at = now
                if len(kept) < keep_n:
                    kept.append(buf[body_start : body_start + clen])
                total += 1
                buf = buf[body_start + clen :]
            if time.time() > deadline:
                break
    fps = 0.0
    if total >= 2 and first_frame_at is not None and last_frame_at is not None:
        elapsed = last_frame_at - first_frame_at
        if elapsed > 0:
            # (total-1) intervals over elapsed seconds — single-frame
            # window would div-by-zero, hence the >= 2 guard.
            fps = (total - 1) / elapsed
    if not kept:
        raise TimeoutError(f"no stream frames received in {window_s}s")
    return kept, fps, total


# ── Image analysis (PIL + numpy) ───────────────────────────────────────────


def _analyze_jpeg(jpg_bytes: bytes) -> dict[str, Any]:
    """Compute objective image quality metrics on the original JPEG.

    sharpness: variance of 5-point Laplacian on luminance — proxy for
        how much high-frequency detail survived JPEG quantization.
        Higher = sharper / less artifact-blurred.
    edge_density: mean abs gradient (X+Y averaged) — how busy/textured
        the image is. Drops as JPEG q rises (compression smooths
        textures). Independent of sharpness.

    No crop is generated — the HTML viewer shows the ORIGINAL JPEG as
    a browser-scaled thumbnail, and click opens it at native resolution
    in a lightbox. Earlier the test re-encoded a 256×256 center crop
    at q=95 hoping it would let the operator see JPEG artifacts at 1:1,
    but the re-encode actually masked the artifacts and the crop was
    too small to convey scene context. Operators couldn't read it.
    """
    import io as _io

    import numpy as np
    from PIL import Image

    try:
        img = Image.open(_io.BytesIO(jpg_bytes))
        img.load()
    except Exception as e:
        return {"sharpness": -1.0, "edge_density": -1.0, "analysis_error": str(e)}

    arr = np.asarray(img.convert("L"), dtype=np.float32)
    if arr.shape[0] < 4 or arr.shape[1] < 4:
        return {"sharpness": -1.0, "edge_density": -1.0, "analysis_error": "image too small"}

    lap = -4.0 * arr[1:-1, 1:-1] + arr[:-2, 1:-1] + arr[2:, 1:-1] + arr[1:-1, :-2] + arr[1:-1, 2:]
    sharpness = float(lap.var())

    gx = np.abs(arr[:, 1:] - arr[:, :-1])
    gy = np.abs(arr[1:, :] - arr[:-1, :])
    edge_density = float((gx.mean() + gy.mean()) / 2)

    return {
        "sharpness": round(sharpness, 2),
        "edge_density": round(edge_density, 3),
    }


# ── tests ──────────────────────────────────────────────────────────────────


@pytest.mark.manual
@pytest.mark.parametrize("device", DEVICE_IDS, ids=DEVICE_NAMES)
def test_still_sweep(
    device: str,
    sweep_mqtt,
    device_ips: dict[str, str],
    sweep_output_dir: Path,
    sweep_records: list[dict[str, Any]],
) -> None:
    """Iterate cam_framesize × cam_quality, capture frames via cmd/photo,
    grab the binary JPEG from image/photo retained topic.

    On UXGA q=8 the JPEG may exceed the MQTT publish cap (160 KB) and the
    image topic never updates — we detect this by waiting for event/photo
    first (always lands), then falling back to HTTP /last.jpg if MQTT
    image is missing.
    """
    client, rec = sweep_mqtt
    dev_id = _device_full_id(device)
    dev_dir = sweep_output_dir / device
    dev_dir.mkdir(exist_ok=True)
    fpc = MATRIX["matrix"]["frames_per_combo"]
    # None when the board never published state/ip → the HTTP fallback for
    # oversized JPEGs is skipped, the MQTT path still runs.
    ip = device_ips.get(device)

    try:
        for fs in MATRIX["matrix"]["framesizes"]:
            for q in MATRIX["matrix"]["qualities"]:
                _set_cfg(client, rec, dev_id, "cam_framesize", fs)
                _set_cfg(client, rec, dev_id, "cam_quality", q)
                time.sleep(1.0)  # sensor settle

                for frame_idx in range(fpc):
                    sent = time.time()
                    client.publish(f"{dev_id}/cmd/photo", "1", qos=1, retain=False)

                    # Accept whatever event/photo arrives next (since=sent).
                    # We *record* the actual (framesize, quality) from the
                    # event JSON rather than asserting against (want_fs,
                    # want_q) — the OV3660 JPEG pipeline can lag a sensor
                    # reconfigure by 1-3 frames, so an "intent vs reality"
                    # mismatch is common and not a sweep failure. The
                    # aggregator groups by actual (fs, q); intermediate
                    # frames just land under whichever combo they truly
                    # belong to.
                    try:
                        event_raw = rec.wait_for(
                            f"{dev_id}/event/photo",
                            lambda p: p.startswith(b"{"),
                            timeout=15.0,
                            since=sent,
                        )
                    except TimeoutError:
                        print(f"  skip {device} still fs={fs} q={q} #{frame_idx}: no event/photo")
                        sweep_records.append(
                            {
                                "device": device,
                                "profile": "still",
                                "framesize": fs,
                                "framesize_name": FRAMESIZE_NAMES.get(fs, "?"),
                                "quality": q,
                                "frame": frame_idx,
                                "capture_ms": -1,
                                "agc": -1,
                                "ir": 0,
                                "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
                                "bytes": 0,
                                "path": "",
                                "notes": "no event/photo received",
                            }
                        )
                        time.sleep(1.0)
                        continue
                    event_arrived = time.time()
                    event = json.loads(event_raw.decode())
                    capture_ms = int((event_arrived - sent) * 1000)
                    # Overwrite (fs, q) with what the sensor actually used —
                    # may differ from the intent (pipeline lag).
                    actual_fs = int(event.get("framesize", fs))
                    actual_q = int(event.get("quality", q))

                    # Try MQTT binary image first (limit 160 KB). On miss,
                    # fall back to HTTP /last.jpg (PSRAM, no size limit).
                    img: bytes | None
                    try:
                        img = rec.wait_for(
                            f"{dev_id}/image/photo",
                            timeout=4.0,
                            since=sent,
                        )
                    except TimeoutError:
                        img = None

                    if img is None and ip:
                        try:
                            with httpx.Client(
                                timeout=8.0, verify=False, follow_redirects=True
                            ) as h:
                                rr = h.get(f"https://{ip}/last.jpg")
                                if rr.status_code == 200 and rr.content[:2] == b"\xff\xd8":
                                    img = rr.content
                        except Exception as e:
                            print(f"WARN: HTTP /last.jpg fallback failed: {e}")

                    notes_lag = (
                        ""
                        if (actual_fs == fs and actual_q == q)
                        else f"pipeline-lag: intent fs={fs} q={q}, got fs={actual_fs} q={actual_q}"
                    )
                    common = {
                        "device": device,
                        "profile": "still",
                        "framesize": actual_fs,
                        "framesize_name": FRAMESIZE_NAMES.get(actual_fs, "?"),
                        "quality": actual_q,
                        "frame": frame_idx,
                        "capture_ms": capture_ms,
                        "agc": event.get("agc", -1),
                        "ir": int(event.get("ir", 0)),
                        "fps": 0.0,
                        "frames_in_window": 0,
                        "ts": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(event_arrived)),
                    }
                    if img is None:
                        sweep_records.append(
                            {
                                **common,
                                "bytes": int(event.get("size", 0)),
                                "path": "",
                                "sharpness": -1.0,
                                "edge_density": -1.0,
                                "notes": notes_lag
                                or "image/photo above MQTT cap; HTTP fallback unavailable",
                            }
                        )
                        continue

                    # Filename uses ACTUAL (fs, q) so the file name stays
                    # consistent with the record. Intent suffix lets the
                    # operator see which iteration produced this frame
                    # even when pipeline lag put it in a different (fs,q)
                    # bucket than intended.
                    fname = f"still_fs{actual_fs:02d}_q{actual_q:02d}_f{frame_idx}_i{fs:02d}q{q:02d}.jpg"
                    (dev_dir / fname).write_bytes(img)
                    metrics = _analyze_jpeg(img)
                    sweep_records.append(
                        {
                            **common,
                            "bytes": len(img),
                            "path": f"{device}/{fname}",
                            "sharpness": metrics.get("sharpness", -1.0),
                            "edge_density": metrics.get("edge_density", -1.0),
                            "notes": notes_lag or metrics.get("analysis_error", ""),
                        }
                    )
    finally:
        _restore_defaults(client, rec, dev_id, "still")


@pytest.mark.manual
@pytest.mark.parametrize("device", DEVICE_IDS, ids=DEVICE_NAMES)
def test_stream_sweep(
    device: str,
    sweep_mqtt,
    device_ips: dict[str, str],
    sweep_output_dir: Path,
    sweep_records: list[dict[str, Any]],
) -> None:
    """Iterate mjpg_framesize × mjpg_quality, open /stream.mjpg, grab N
    frames from the multipart stream per combo."""
    if device not in device_ips:
        pytest.skip(
            f"no IP for {device} — stream sweep needs HTTP; set ip in matrix or boot device"
        )
    client, rec = sweep_mqtt
    dev_id = _device_full_id(device)
    ip = device_ips[device]
    dev_dir = sweep_output_dir / device
    dev_dir.mkdir(exist_ok=True)
    fpc = MATRIX["matrix"]["frames_per_combo"]

    try:
        for fs in MATRIX["matrix"]["framesizes"]:
            for q in MATRIX["matrix"]["qualities"]:
                _set_cfg(client, rec, dev_id, "mjpg_framesize", fs)
                _set_cfg(client, rec, dev_id, "mjpg_quality", q)
                # Note: stream profile is applied at stream-open; the values
                # we just set are read by camera_apply_stream_profile() when
                # the next /stream.mjpg request lands.
                try:
                    frames, fps, total = _grab_mjpeg_burst(ip, window_s=5.0, keep_n=fpc)
                except (TimeoutError, httpx.HTTPError) as e:
                    print(f"WARN: stream {device} fs={fs} q={q} failed: {e}")
                    sweep_records.append(
                        {
                            "device": device,
                            "profile": "stream",
                            "framesize": fs,
                            "framesize_name": FRAMESIZE_NAMES.get(fs, "?"),
                            "quality": q,
                            "frame": 0,
                            "capture_ms": -1,
                            "agc": -1,
                            "ir": 0,
                            "fps": 0.0,
                            "frames_in_window": 0,
                            "sharpness": -1.0,
                            "edge_density": -1.0,
                            "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
                            "bytes": 0,
                            "path": "",
                            "notes": f"stream open failed: {e}",
                        }
                    )
                    continue
                stream_ts = time.strftime("%Y-%m-%dT%H:%M:%S")
                for frame_idx, img in enumerate(frames):
                    fname = f"stream_fs{fs:02d}_q{q:02d}_f{frame_idx}.jpg"
                    (dev_dir / fname).write_bytes(img)
                    metrics = _analyze_jpeg(img)
                    sweep_records.append(
                        {
                            "device": device,
                            "profile": "stream",
                            "framesize": fs,
                            "framesize_name": FRAMESIZE_NAMES.get(fs, "?"),
                            "quality": q,
                            "frame": frame_idx,
                            "capture_ms": -1,  # not measurable per-frame on multipart
                            "agc": -1,
                            "ir": 0,
                            "fps": round(fps, 2),
                            "frames_in_window": total,
                            "sharpness": metrics.get("sharpness", -1.0),
                            "edge_density": metrics.get("edge_density", -1.0),
                            "ts": stream_ts,
                            "bytes": len(img),
                            "path": f"{device}/{fname}",
                            "notes": metrics.get("analysis_error", ""),
                        }
                    )
    finally:
        _restore_defaults(client, rec, dev_id, "stream")


# ── output writers ─────────────────────────────────────────────────────────


def _write_summary(out_dir: Path, records: list[dict[str, Any]]) -> None:
    csv_path = out_dir / "summary.csv"
    fieldnames = [
        "device",
        "profile",
        "framesize",
        "framesize_name",
        "quality",
        "frame",
        "bytes",
        "capture_ms",
        "agc",
        "ir",
        "fps",
        "frames_in_window",
        "sharpness",
        "edge_density",
        "ts",
        "path",
        "notes",
    ]
    with csv_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for r in records:
            w.writerow({k: r.get(k, "") for k in fieldnames})


def _aggregate(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Group records by (device, profile, framesize, quality); return
    per-combo means of bytes / fps / sharpness / edge_density."""
    buckets: dict[tuple, list[dict[str, Any]]] = {}
    for r in records:
        if not r.get("path"):
            continue  # skip empty-record placeholders
        key = (r["device"], r["profile"], r["framesize"], r["quality"])
        buckets.setdefault(key, []).append(r)
    out: list[dict[str, Any]] = []
    for (dev, prof, fs, q), rs in buckets.items():
        mean_bytes = sum(r["bytes"] for r in rs) / len(rs)
        mean_sharp = sum(r.get("sharpness", -1) for r in rs if r.get("sharpness", -1) >= 0) / max(
            1, sum(1 for r in rs if r.get("sharpness", -1) >= 0)
        )
        mean_edge = sum(
            r.get("edge_density", -1) for r in rs if r.get("edge_density", -1) >= 0
        ) / max(1, sum(1 for r in rs if r.get("edge_density", -1) >= 0))
        fps_vals = [r.get("fps", 0) for r in rs if r.get("fps", 0) > 0]
        mean_fps = (sum(fps_vals) / len(fps_vals)) if fps_vals else 0.0
        out.append(
            {
                "device": dev,
                "profile": prof,
                "framesize": fs,
                "framesize_name": FRAMESIZE_NAMES.get(fs, "?"),
                "quality": q,
                "n": len(rs),
                "mean_bytes": int(mean_bytes),
                "mean_sharpness": round(mean_sharp, 2),
                "mean_edge_density": round(mean_edge, 3),
                "mean_fps": round(mean_fps, 2),
                "mean_kbps": round(mean_bytes * mean_fps * 8 / 1000) if mean_fps > 0 else 0,
            }
        )
    out.sort(key=lambda r: (r["device"], r["profile"], r["framesize"], r["quality"]))
    return out


# RSSI/throughput tiers — ordered worst → best. For a given required
# kbps, we walk this list and return the FIRST (= weakest) signal that
# still supports it. That answers "what's the lowest signal I can get
# away with and still run this profile". Caps are conservative for
# ESP32-S3 single-stream 802.11 b/g/n on a noisy 2.4 GHz field link;
# treat them as minimum signal needed, not throughput guarantee.
RSSI_TIERS = [
    (300, "≤ -85 dBm  (very weak — sporadic uploads only)"),
    (1_000, "≤ -80 dBm  (marginal — needs antenna improvements)"),
    (3_000, "≤ -75 dBm  (acceptable an upland forest site field deployment)"),
    (8_000, "≤ -70 dBm  (one wall / 5-10 m line-of-sight)"),
    (20_000, "≤ -60 dBm  (good signal, one room over)"),
    (40_000, "≤ -50 dBm  (close to AP)"),
]


def _rssi_for_kbps(kbps: int) -> str:
    if kbps <= 0:
        return "n/a"
    for cap, label in RSSI_TIERS:
        if kbps <= cap:
            return label
    return "> -50 dBm  (must be very close — exceeds tier table)"


def _recommend(agg: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """Per-device recommendation for cam_quality + mjpg_(framesize, quality).

    cam_quality (still): highest quality (= lowest q number) at UXGA
    where mean_bytes ≤ 150 KB (safety margin under 160 KB MQTT image
    out_size cap at firmware/main/mqtt.c:717). Falls back to next
    higher q if every UXGA row is too big.

    mjpg_(framesize, quality): Pareto-optimal pick by sharpness × fps
    score, gated by mean_kbps ≤ 5 Mbps (field deployment budget). The
    score balances "is it pretty enough" with "do I see it move."
    """
    MQTT_CAP_BYTES = 150_000  # MQTT image/photo cap is 160 KB; leave 10 KB margin
    STREAM_KBPS_CAP = 5_000  # 5 Mbps — reasonable upper bound for field

    out: dict[str, dict[str, Any]] = {}
    for dev in sorted({r["device"] for r in agg}):
        still_pick = None
        # Pick at UXGA first (framesize=13) — that's the canonical capture size
        uxga_rows = [
            r
            for r in agg
            if r["device"] == dev and r["profile"] == "still" and r["framesize"] == 13
        ]
        # sort by quality ascending (low q = high quality)
        uxga_rows.sort(key=lambda r: r["quality"])
        for r in uxga_rows:
            if r["mean_bytes"] <= MQTT_CAP_BYTES:
                still_pick = r
                break
        if still_pick is None and uxga_rows:
            # nothing under cap; pick lowest-quality (highest q) UXGA so MQTT can ship
            still_pick = uxga_rows[-1]

        stream_pick = None
        stream_rows = [
            r
            for r in agg
            if r["device"] == dev
            and r["profile"] == "stream"
            and r["mean_fps"] > 0
            and r["mean_kbps"] <= STREAM_KBPS_CAP
        ]
        # Score: sqrt(sharpness) × fps — sharper + faster wins; sqrt
        # tempers runaway scores from very busy scenes.
        import math

        for r in stream_rows:
            sh = max(r["mean_sharpness"], 0.01)
            r["_score"] = math.sqrt(sh) * r["mean_fps"]
        stream_rows.sort(key=lambda r: r["_score"], reverse=True)
        if stream_rows:
            stream_pick = stream_rows[0]

        out[dev] = {
            "still": still_pick,
            "stream": stream_pick,
        }
    return out


def _write_bandwidth_md(
    out_dir: Path, agg: list[dict[str, Any]], recommendations: dict[str, dict[str, Any]]
) -> None:
    """Markdown table summarising bytes / FPS / Mbps / required RSSI
    per (device, profile, fs, q). Easy to paste into a status note."""
    lines: list[str] = []
    lines.append("# JPEG sweep — bandwidth + signal requirements\n")
    lines.append(
        "Computed from one sweep run. KB/frame is the mean over "
        "the 3 captured frames; FPS is measured across a 5 s "
        "stream window. Mbps = KB × FPS × 8 / 1000.\n"
    )
    lines.append(
        "RSSI tiers are conservative for ESP32-S3 single-stream "
        "WiFi over 2.4 GHz; treat as minimum needed, not "
        "throughput guarantee.\n"
    )

    lines.append("## Recommended per device\n")
    lines.append("| Device | Use case | Recommended | Why |")
    lines.append("|---|---|---|---|")
    for dev, picks in recommendations.items():
        s = picks.get("still")
        if s:
            lines.append(
                f"| {dev} | still capture (cmd/photo) | "
                f"`cam_framesize={s['framesize']}` ({s['framesize_name']}) + "
                f"`cam_quality={s['quality']}` | "
                f"{s['mean_bytes'] // 1024} KB/frame — fits under MQTT cap, "
                f"highest q surviving |"
            )
        m = picks.get("stream")
        if m:
            lines.append(
                f"| {dev} | MJPEG stream (/stream.mjpg) | "
                f"`mjpg_framesize={m['framesize']}` ({m['framesize_name']}) + "
                f"`mjpg_quality={m['quality']}` | "
                f"{m['mean_fps']} fps × {m['mean_bytes'] // 1024} KB = "
                f"{m['mean_kbps']} kbps; needs {_rssi_for_kbps(m['mean_kbps'])} |"
            )

    for dev in sorted({r["device"] for r in agg}):
        for prof in ("still", "stream"):
            rows = [r for r in agg if r["device"] == dev and r["profile"] == prof]
            if not rows:
                continue
            lines.append(f"\n## {dev} · {prof}\n")
            if prof == "stream":
                lines.append(
                    "| Framesize | Quality | KB/frame | FPS | kbps | Min RSSI | Sharpness | Edge |"
                )
                lines.append("|---|---|---|---|---|---|---|---|")
                for r in rows:
                    lines.append(
                        f"| {r['framesize_name']} ({r['framesize']}) "
                        f"| q={r['quality']} "
                        f"| {r['mean_bytes'] // 1024} "
                        f"| {r['mean_fps']} "
                        f"| {r['mean_kbps']} "
                        f"| {_rssi_for_kbps(r['mean_kbps'])} "
                        f"| {r['mean_sharpness']} "
                        f"| {r['mean_edge_density']} |"
                    )
            else:
                lines.append("| Framesize | Quality | KB/frame | Sharpness | Edge | Notes |")
                lines.append("|---|---|---|---|---|---|")
                for r in rows:
                    note = ""
                    if r["mean_bytes"] > 160_000:
                        note = "⚠ exceeds 160 KB MQTT cap"
                    elif r["mean_bytes"] > 150_000:
                        note = "tight against 160 KB cap"
                    lines.append(
                        f"| {r['framesize_name']} ({r['framesize']}) "
                        f"| q={r['quality']} "
                        f"| {r['mean_bytes'] // 1024} "
                        f"| {r['mean_sharpness']} "
                        f"| {r['mean_edge_density']} "
                        f"| {note} |"
                    )
    (out_dir / "bandwidth.md").write_text("\n".join(lines) + "\n")


_VIEWER_TEMPLATE = """<!doctype html>
<html lang=en>
<meta charset=utf-8>
<title>JPEG sweep — chytra-budka</title>
<style>
  body { font-family: system-ui, sans-serif; margin: 0; color: #222; }
  h1 { font-size: 1.2em; margin: 0.4em 1em; }
  h2 { font-size: 1.05em; margin: 0.6em 1em 0.3em; }
  section { padding: 0 1em 0.5em; }
  .reco { background: #f1f7ff; border: 1px solid #b4d1f0; padding: 0.6em 0.9em;
          border-radius: 4px; margin: 0.6em 1em 1em; }
  .reco code { background: #e2ecf7; padding: 1px 4px; border-radius: 2px; }
  table.bw { border-collapse: collapse; width: 100%; font-size: 0.86em;
             margin-bottom: 1.2em; }
  table.bw th, table.bw td { padding: 4px 8px; border: 1px solid #ddd;
                              text-align: right; }
  table.bw th { background: #f0f0f0; text-align: center; }
  table.bw td:first-child, table.bw td:nth-child(2) { text-align: left; }
  table.bw tr.warn td { background: #fff3cd; }
  table.bw tr.bad td { background: #f8d7da; }
  .toolbar { position: sticky; top: 0; background: #f8f8f8;
             border-bottom: 1px solid #ccc; padding: 0.5em 1em; z-index: 10; }
  .toolbar label { margin-right: 0.6em; font-size: 0.85em; }
  .toolbar strong { margin-right: 0.4em; }
  .toolbar .group { margin-right: 1.4em; display: inline-block; }
  #counter { float: right; color: #666; }
  .grid { display: grid; padding: 1em;
          grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 12px; }
  .cell { border: 1px solid #ddd; padding: 6px; font-size: 0.78em;
          background: #fff; }
  .cell.missing { background: #ffe; }
  .cell .thumb { width: 100%; max-width: 100%; height: auto; cursor: zoom-in;
                  display: block; background: #eee; }
  .cell .caption { padding-top: 6px; line-height: 1.35; font-family: ui-monospace, monospace; }
  .cell .caption .kv { color: #666; }
  .cell .caption b { color: #111; }
  .lightbox { position: fixed; inset: 0; background: rgba(0,0,0,0.88);
              display: none; align-items: center; justify-content: center;
              z-index: 20; flex-direction: column; color: #ddd; }
  .lightbox.open { display: flex; }
  .lightbox img { max-width: 96vw; max-height: 92vh; }
  .lightbox .lbinfo { margin-top: 0.5em; font-family: ui-monospace, monospace; font-size: 0.9em; }
</style>

<h1>JPEG sweep — chytra-budka</h1>

<section id="recos-section">
  <h2>Recommendations per device</h2>
  <div id="recos"></div>
</section>

<section>
  <h2>Bandwidth + signal requirements</h2>
  <p style="font-size:0.85em; color:#555; margin:0 0 0.5em 0">
    KB/frame = mean over 3 captured frames. FPS measured across a 5 s
    stream window. kbps = KB × FPS × 8. RSSI tiers are conservative for
    ESP32-S3 single-stream 802.11 b/g/n on 2.4 GHz field deployments —
    treat as <em>minimum needed</em>, not a guarantee.
  </p>
  <div id="bwtables"></div>
</section>

<div class="toolbar">
  <span class="group"><strong>Device</strong><span id="device-filters"></span></span>
  <span class="group"><strong>Profile</strong><span id="profile-filters"></span></span>
  <span class="group"><strong>Framesize</strong><span id="fs-filters"></span></span>
  <span class="group"><strong>Quality</strong><span id="q-filters"></span></span>
  <span id="counter"></span>
</div>

<div id="grid" class="grid"></div>

<div id="lightbox" class="lightbox" onclick="this.classList.remove('open')">
  <img id="lbimg" alt>
  <div class="lbinfo" id="lbinfo"></div>
</div>

<script>
const DATA = __DATA__;
const STATE = {
  device: new Set(DATA.devices),
  profile: new Set(DATA.profiles),
  fs: new Set(DATA.framesizes),
  q: new Set(DATA.qualities),
};
const $ = (id) => document.getElementById(id);
const fsName = (fs) => DATA.framesize_names[fs] || fs;

// ── Recommendations panel ────────────────────────────────────────────────
function renderRecos() {
  const root = $("recos");
  root.innerHTML = "";
  for (const dev of Object.keys(DATA.recommendations)) {
    const r = DATA.recommendations[dev];
    const box = document.createElement("div");
    box.className = "reco";
    const lines = [`<strong>${dev}</strong>`];
    if (r.still) {
      const s = r.still;
      lines.push(
        `Still capture (cmd/photo): <code>cam_framesize=${s.framesize}</code> `
        + `(${fsName(s.framesize)}) + <code>cam_quality=${s.quality}</code>`
        + ` — ${Math.round(s.mean_bytes/1024)} KB/frame, sharpness ${s.mean_sharpness}`);
    }
    if (r.stream) {
      const m = r.stream;
      lines.push(
        `MJPEG stream (/stream.mjpg): <code>mjpg_framesize=${m.framesize}</code> `
        + `(${fsName(m.framesize)}) + <code>mjpg_quality=${m.quality}</code>`
        + ` — ${m.mean_fps} fps, ${m.mean_kbps} kbps`);
    }
    box.innerHTML = lines.join("<br>");
    root.appendChild(box);
  }
}

// ── Bandwidth tables ─────────────────────────────────────────────────────
function renderBandwidth() {
  const root = $("bwtables");
  root.innerHTML = "";
  for (const dev of DATA.devices) {
    for (const prof of DATA.profiles) {
      const rows = DATA.aggregate.filter(a => a.device === dev && a.profile === prof);
      if (!rows.length) continue;
      const h = document.createElement("h2");
      h.textContent = `${dev} · ${prof}`;
      root.appendChild(h);
      const tbl = document.createElement("table");
      tbl.className = "bw";
      let header;
      if (prof === "stream") {
        header = ["Framesize", "Quality", "KB/frame", "FPS", "kbps", "Min RSSI", "Sharpness", "Edge"];
      } else {
        header = ["Framesize", "Quality", "KB/frame", "Sharpness", "Edge", "Notes"];
      }
      const trh = document.createElement("tr");
      header.forEach(c => { const th = document.createElement("th"); th.textContent = c; trh.appendChild(th); });
      tbl.appendChild(trh);
      for (const r of rows) {
        const tr = document.createElement("tr");
        let cells;
        if (prof === "stream") {
          cells = [
            `${fsName(r.framesize)} (${r.framesize})`,
            `q=${r.quality}`,
            Math.round(r.mean_bytes/1024),
            r.mean_fps,
            r.mean_kbps,
            r.rssi_tier,
            r.mean_sharpness,
            r.mean_edge_density,
          ];
          if (r.mean_kbps > 5000) tr.className = "bad";
          else if (r.mean_kbps > 2000) tr.className = "warn";
        } else {
          let notes = "";
          if (r.mean_bytes > 160000) { notes = "⚠ exceeds MQTT 160 KB cap"; tr.className = "bad"; }
          else if (r.mean_bytes > 150000) { notes = "tight to MQTT cap"; tr.className = "warn"; }
          cells = [
            `${fsName(r.framesize)} (${r.framesize})`,
            `q=${r.quality}`,
            Math.round(r.mean_bytes/1024),
            r.mean_sharpness,
            r.mean_edge_density,
            notes,
          ];
        }
        cells.forEach(c => { const td = document.createElement("td"); td.textContent = c; tr.appendChild(td); });
        tbl.appendChild(tr);
      }
      root.appendChild(tbl);
    }
  }
}

// ── Filters + grid ───────────────────────────────────────────────────────
function chip(group, label, value, key) {
  const wrap = document.createElement("label");
  wrap.innerHTML = `<input type=checkbox checked> ${label}`;
  const cb = wrap.querySelector("input");
  cb.onchange = () => { if (cb.checked) STATE[key].add(value); else STATE[key].delete(value); render(); };
  group.appendChild(wrap);
}
DATA.devices.forEach(d => chip($("device-filters"), d, d, "device"));
DATA.profiles.forEach(p => chip($("profile-filters"), p, p, "profile"));
DATA.framesizes.forEach(fs => chip($("fs-filters"), `${fsName(fs)} (${fs})`, fs, "fs"));
DATA.qualities.forEach(q => chip($("q-filters"), `q${q}`, q, "q"));

function render() {
  const grid = $("grid");
  grid.innerHTML = "";
  let shown = 0;
  for (const r of DATA.records) {
    if (!STATE.device.has(r.device)) continue;
    if (!STATE.profile.has(r.profile)) continue;
    if (!STATE.fs.has(r.framesize)) continue;
    if (!STATE.q.has(r.quality)) continue;
    shown++;
    const cell = document.createElement("div");
    cell.className = "cell" + (r.path ? "" : " missing");
    const kb = (r.bytes / 1024).toFixed(1);
    const sharp = (r.sharpness != null && r.sharpness >= 0) ? r.sharpness : "—";
    const edge = (r.edge_density != null && r.edge_density >= 0) ? r.edge_density : "—";
    const fps = (r.fps != null && r.fps > 0) ? ` <span class=kv>fps</span> <b>${r.fps}</b>` : "";
    const ms = (r.capture_ms != null && r.capture_ms >= 0) ? ` <span class=kv>ms</span> <b>${r.capture_ms}</b>` : "";
    const cap = `${r.device} · ${r.profile} · ${fsName(r.framesize)} q${r.quality} #${r.frame}<br>`
              + `<span class=kv>KB</span> <b>${kb}</b>`
              + `${ms}${fps}<br>`
              + `<span class=kv>sharp</span> <b>${sharp}</b> `
              + `<span class=kv>edge</span> <b>${edge}</b>`
              + (r.notes ? `<br><em>${r.notes}</em>` : "");
    cell.innerHTML = r.path
      ? `<img class=thumb loading=lazy src="${r.path}" onclick="zoom('${r.path}', this)" alt><div class=caption>${cap}</div>`
      : `<div class=caption>${cap}</div>`;
    grid.appendChild(cell);
  }
  $("counter").textContent = `${shown} / ${DATA.records.length} frames shown`;
}
function zoom(p, imgEl) {
  $("lbimg").src = p;
  $("lbinfo").textContent = imgEl.closest(".cell").querySelector(".caption").innerText.replace(/\\n+/g, " · ");
  $("lightbox").classList.add("open");
}

renderRecos();
renderBandwidth();
render();
</script>
"""


def _write_index(
    out_dir: Path,
    records: list[dict[str, Any]],
    agg: list[dict[str, Any]],
    recos: dict[str, dict[str, Any]],
) -> None:
    devices = sorted({r["device"] for r in records})
    profiles = sorted({r["profile"] for r in records})
    framesizes = sorted({r["framesize"] for r in records})
    qualities = sorted({r["quality"] for r in records})
    # Add rssi_tier inline so JS doesn't need to repeat the mapping table
    agg_with_rssi = [{**r, "rssi_tier": _rssi_for_kbps(r["mean_kbps"])} for r in agg]
    payload = {
        "records": records,
        "aggregate": agg_with_rssi,
        "recommendations": recos,
        "devices": devices,
        "profiles": profiles,
        "framesizes": framesizes,
        "qualities": qualities,
        "framesize_names": FRAMESIZE_NAMES,
    }
    html = _VIEWER_TEMPLATE.replace("__DATA__", json.dumps(payload))
    (out_dir / "index.html").write_text(html)
