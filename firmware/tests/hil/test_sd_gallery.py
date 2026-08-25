"""
test_sd_gallery — date-tree photo store: day-bucketed listing, the
/photos.json API, cacheable + range-able /photo serving, and the SD
autoprune config/telemetry wiring.

Contract for the rewrite in sd_layout.c / sd_storage.c / http_server.c:
  - /photos (HTML) lists day buckets when no ?day, one day when ?day=X.
  - /photos.json is machine-readable (index → days[]; day → files[]).
  - /photo?d=&f= serves the JPEG with an immutable ETag (conditional GET
    → 304) and honours a single Range request (→ 206 + Content-Range).
  - sd_autoprune / sd_min_free / sd_keep_days are live config knobs and
    /selftest surfaces sd_pruned_files once a card is mounted.

Most tests skip gracefully when the bench has no SD card or no photos yet,
so the file is safe to run against a freshly-reset board. The one
destructive test (actual oldest-bucket deletion) is gated behind
CB_HIL_ALLOW_PRUNE=1 so a normal run never wipes bench data.
"""

from __future__ import annotations

import datetime
import json
import os
import re
import time

import pytest

DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def _json(http, uri):
    r = http.get(uri, timeout=10.0)
    assert r.status_code == 200, f"{uri} → {r.status_code}: {r.text[:200]}"
    return r.json()


def _sd_mounted(http) -> bool:
    """True if the bench reports a mounted card (selftest has sd_total_mb)."""
    r = http.get("/selftest", timeout=10.0)
    if r.status_code != 200:
        return False
    try:
        return "sd_total_mb" in r.json()
    except json.JSONDecodeError:
        return False


def _find_a_photo(http):
    """Return (day, leaf) for some existing photo, or None if the card is empty."""
    idx = _json(http, "/photos.json")
    for day in idx.get("days", []):
        page = _json(http, f"/photos.json?day={day}")
        files = page.get("files", [])
        if files:
            return day, files[0]["f"]
    return None


# ── /photos.json shape ─────────────────────────────────────────────────────


def test_photos_json_index_shape(http):
    """Index: {"gen":int, "days":[bucket,…]} with valid bucket names."""
    if not _sd_mounted(http):
        pytest.skip("no SD card mounted on the bench")
    idx = _json(http, "/photos.json")
    assert isinstance(idx.get("gen"), int), idx
    assert isinstance(idx.get("days"), list), idx
    for b in idx["days"]:
        assert b in ("root", "boot") or DATE_RE.match(b), f"bad bucket {b!r}"
    # Newest-first: dated days descending.
    dated = [b for b in idx["days"] if DATE_RE.match(b)]
    assert dated == sorted(dated, reverse=True), f"days not newest-first: {dated}"


def test_photos_json_day_shape(http):
    """A day page: day/page/per_page/total/pages + files[{f,trig,bytes}]."""
    if not _sd_mounted(http):
        pytest.skip("no SD card mounted on the bench")
    found = _find_a_photo(http)
    if not found:
        pytest.skip("no photos on the card yet")
    day, _leaf = found
    page = _json(http, f"/photos.json?day={day}")
    assert page["day"] == day
    assert page["per_page"] == 100
    assert isinstance(page["total"], int) and page["total"] >= 1
    assert isinstance(page["files"], list) and page["files"]
    f0 = page["files"][0]
    assert set(("f", "trig", "bytes")).issubset(f0), f0
    assert f0["bytes"] >= 0


def test_photos_html_index_and_day(http):
    """/photos renders the day index; ?day=X renders that day with a back link."""
    if not _sd_mounted(http):
        pytest.skip("no SD card mounted on the bench")
    r = http.get("/photos", timeout=10.0)
    assert r.status_code == 200 and "<table>" in r.text
    found = _find_a_photo(http)
    if not found:
        pytest.skip("no photos on the card yet")
    day, leaf = found
    r = http.get(f"/photos?day={day}", timeout=10.0)
    assert r.status_code == 200
    # The day page links each photo through the new d=&f= addressing.
    assert f"/photo?d={day}&amp;f={leaf}" in r.text or f"f={leaf}" in r.text
    assert "/photos" in r.text  # "all days" back-link


# ── /photo: caching + range ────────────────────────────────────────────────


def test_photo_immutable_etag_and_304(http):
    """/photo carries an immutable Cache-Control + ETag; a matching
    If-None-Match returns 304 with no body."""
    if not _sd_mounted(http):
        pytest.skip("no SD card mounted on the bench")
    found = _find_a_photo(http)
    if not found:
        pytest.skip("no photos on the card yet")
    day, leaf = found
    r = http.get(f"/photo?d={day}&f={leaf}", timeout=15.0)
    assert r.status_code == 200, r.status_code
    assert r.headers.get("content-type", "").startswith("image/jpeg")
    cc = r.headers.get("cache-control", "")
    assert "immutable" in cc and "max-age" in cc, f"weak cache-control: {cc!r}"
    etag = r.headers.get("etag")
    assert etag and etag.startswith('"'), f"missing/odd etag: {etag!r}"

    r2 = http.get(
        f"/photo?d={day}&f={leaf}",
        headers={"If-None-Match": etag},
        timeout=15.0,
    )
    assert r2.status_code == 304, f"conditional GET → {r2.status_code}"
    assert r2.content == b"", "304 must have an empty body"


def test_photo_range_206(http):
    """A Range request returns 206 with a correct Content-Range and the
    requested slice length."""
    if not _sd_mounted(http):
        pytest.skip("no SD card mounted on the bench")
    found = _find_a_photo(http)
    if not found:
        pytest.skip("no photos on the card yet")
    day, leaf = found
    full = http.get(f"/photo?d={day}&f={leaf}", timeout=15.0)
    total = int(full.headers.get("content-length", len(full.content)) or len(full.content))
    if total < 200:
        pytest.skip(f"photo too small to range-test ({total} B)")
    r = http.get(
        f"/photo?d={day}&f={leaf}",
        headers={"Range": "bytes=0-99"},
        timeout=15.0,
    )
    assert r.status_code == 206, f"Range → {r.status_code}"
    cr = r.headers.get("content-range", "")
    assert cr == f"bytes 0-99/{total}", f"bad Content-Range: {cr!r}"
    assert len(r.content) == 100, f"slice len {len(r.content)} != 100"
    assert r.content == full.content[:100], "range bytes mismatch"


def test_photo_bad_params_rejected(http):
    """Traversal / malformed params are 4xx, never 5xx or a file leak."""
    for bad in (
        "/photo?d=../etc&f=passwd",
        "/photo?d=2026-06-04&f=../../secrets.h",
        "/photo?f=a/b.jpg",
    ):
        r = http.get(bad, timeout=10.0)
        assert 400 <= r.status_code < 500, f"{bad} → {r.status_code}"


# ── autoprune config + telemetry ───────────────────────────────────────────


def test_sd_config_knobs(cfg):
    """sd_autoprune / sd_min_free / sd_keep_days accept + echo values."""
    assert cfg("sd_autoprune", "OFF") == "OFF"
    assert cfg("sd_autoprune", "ON") == "ON"
    assert cfg("sd_min_free", 15) == "15"
    assert cfg("sd_min_free", 10) == "10"  # restore default
    assert cfg("sd_keep_days", 0) == "0"


def test_selftest_exposes_prune_fields(http):
    """When a card is mounted, /selftest reports sd_pruned_files (counter)."""
    if not _sd_mounted(http):
        pytest.skip("no SD card mounted on the bench")
    st = _json(http, "/selftest")
    assert "sd_pruned_files" in st, st
    assert isinstance(st["sd_pruned_files"], int) and st["sd_pruned_files"] >= 0


def test_capture_lands_in_day_tree(http):
    """A fresh capture appears under today's day bucket (clock permitting)."""
    if not _sd_mounted(http):
        pytest.skip("no SD card mounted on the bench")
    cap = http.get("/capture", timeout=30.0)
    if cap.status_code != 200:
        pytest.skip(f"/capture returned {cap.status_code} (camera busy/absent)")
    time.sleep(1.0)
    today = datetime.date.today().strftime("%Y-%m-%d")
    idx = _json(http, "/photos.json")
    if today not in idx["days"]:
        pytest.skip(
            "board clock not on today's civil date — capture went to "
            f"a different bucket; index days={idx['days']}"
        )
    page = _json(http, f"/photos.json?day={today}")
    assert page["total"] >= 1, "today's bucket empty after a capture"


@pytest.mark.skipif(
    not os.environ.get("CB_HIL_ALLOW_PRUNE"),
    reason="destructive: deletes oldest day buckets — set CB_HIL_ALLOW_PRUNE=1 to run",
)
def test_autoprune_deletes_oldest(http, cfg):
    """Forcing a high free-space floor makes the next capture prune the
    oldest bucket — sd_pruned_files climbs. DESTRUCTIVE (opt-in)."""
    if not _sd_mounted(http):
        pytest.skip("no SD card mounted on the bench")
    before = _json(http, "/selftest").get("sd_pruned_files", 0)
    cfg("sd_autoprune", "ON")
    cfg("sd_min_free", 99)  # "keep 99% free" → prune everything but today
    try:
        http.get("/capture", timeout=30.0)
        deadline = time.time() + 30
        after = before
        while time.time() < deadline:
            after = _json(http, "/selftest").get("sd_pruned_files", before)
            if after > before:
                break
            time.sleep(2)
        assert after > before, f"sd_pruned_files did not climb ({before}→{after})"
    finally:
        cfg("sd_min_free", 10)  # restore sane default
