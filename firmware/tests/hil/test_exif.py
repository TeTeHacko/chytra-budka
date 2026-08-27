"""
test_exif — verify each captured JPEG carries a well-formed APP1 EXIF
segment with the analysis fields jpeg_stamp.c is supposed to emit.

Catches byte-layout regressions in the hand-rolled EXIF builder
(e.g. the inline-ASCII bug where a ≤4-byte trigger like "vad\\0" was
written as an offset and exiftool then showed `\\xb0\\0\\0\\0` instead
of "vad"). Also catches "we silently stopped emitting a tag" — a class
of regression that surface JSON in event/photo can't detect because
that payload is built separately from the file.

Pillow parses the standard tags out of the box; UserComment we read
back as raw bytes and parse the JSON payload after stripping the
8-byte "ASCII\\0\\0\\0" character-code prefix the spec requires.
"""

from __future__ import annotations

import io
import json
import os
import re
import time

import httpx
import pytest
from conftest import _mqtt_client, _MqttRecorder
from PIL import Image

# Tag IDs we care about. Pillow's TAGS dict maps id → human name; we
# verify presence and content. EXIF spec references:
#   0x010E ImageDescription, 0x010F Make, 0x0110 Model,
#   0x0112 Orientation,      0x0131 Software,    0x0132 DateTime,
#   0x8769 ExifIFDPointer (lifts the rest into a sub-IFD).
# ExifSubIFD:
#   0x9000 ExifVersion,    0x9003 DateTimeOriginal, 0x9004 CreateDate,
#   0x9010 OffsetTime,     0x9011 OffsetTimeOriginal,
#   0x9286 UserComment,    0xA434 LensModel.
REQUIRED_IFD0 = {
    0x010E: "ImageDescription",
    0x010F: "Make",
    0x0110: "Model",
    0x0112: "Orientation",
    0x0131: "Software",
    0x0132: "DateTime",
}
REQUIRED_EXIF_SUBIFD = {
    0x9000: "ExifVersion",
    0x9003: "DateTimeOriginal",
    0x9004: "DateTimeDigitized",
    0x9010: "OffsetTime",
    0x9011: "OffsetTimeOriginal",
    0x9286: "UserComment",
    0xA434: "LensModel",
}
# UserComment JSON keys we always expect — emitted unconditionally by
# jpeg_stamp.c regardless of sensor state (sensor-unavailable surfaces
# as `-1` for agc/framesize/quality, which is fine for required-set
# coverage; the conditional fields are checked separately below).
REQUIRED_UC_KEYS = {
    "seq",
    "uptime_s",
    "capture_ms",
    "agc",
    "ir",
    "framesize",
    "quality",
    "rssi",
    "heap",
}
# Battery is conditional on battery_ready() — present on a board with a
# MAX17048 wired up, absent on a board where the fuel-gauge probe
# failed at boot. The test below checks they're either both present or
# both absent (never one without the other).
BATTERY_UC_KEYS = {"vbatt", "soc"}
# mcu_c is conditional on isfinite(diag_mcu_temp_c()) — the on-die
# temp sensor occasionally wedges and returns NAN; we'd rather skip
# the field than ship a -1 sentinel into long-term analysis. Tested
# separately so a wedged temp sensor doesn't fail the required-set
# assertion on an otherwise-healthy board.


# ── helpers ───────────────────────────────────────────────────────────


def _trigger_and_fetch(mqtt_rec, http, bench_id: str) -> bytes:
    """Force a fresh capture and return the resulting JPEG bytes.

    cmd/photo is async (the device captures on its next main-loop
    iteration), so we wait for event/photo with a fresh seq before
    fetching /last.jpg. Without the seq sync /last.jpg can return a
    stale frame from before the test started.
    """
    sent_at = time.time()
    base_seq_msg = mqtt_rec.latest(f"{bench_id}/event/photo")
    base_seq = -1
    if base_seq_msg:
        try:
            base_seq = json.loads(base_seq_msg.decode()).get("seq", -1)
        except (json.JSONDecodeError, AttributeError):
            pass

    mqtt_rec._client.publish(f"{bench_id}/cmd/photo", "1", qos=1, retain=False)

    def _is_new(payload: bytes) -> bool:
        try:
            return json.loads(payload.decode()).get("seq", -1) > base_seq
        except (json.JSONDecodeError, AttributeError):
            return False

    mqtt_rec.wait_for(f"{bench_id}/event/photo", _is_new, timeout=15.0, since=sent_at)

    r = http.get("/last.jpg", timeout=10.0)
    assert r.status_code == 200, (
        f"/last.jpg returned {r.status_code} after cmd/photo: {r.text[:200]}"
    )
    return r.content


def _parse_user_comment(raw: bytes) -> dict:
    """UserComment is type UNDEFINED with an 8-byte character-code
    prefix (per EXIF 2.30 §4.6.4 / Annex). We expect "ASCII\\0\\0\\0";
    the rest is plain ASCII JSON written by jpeg_stamp.c."""
    assert len(raw) >= 8, f"UserComment too short: {raw!r}"
    prefix = raw[:8]
    assert prefix == b"ASCII\x00\x00\x00", (
        f"UserComment charset prefix should be 'ASCII\\0\\0\\0', got {prefix!r}"
    )
    body = raw[8:].rstrip(b"\x00").decode("ascii", errors="replace")
    return json.loads(body)


# ── tests ─────────────────────────────────────────────────────────────


@pytest.fixture(scope="module")
def jpeg(bench_id: str, bench_ip: str, http_basic_creds_optional) -> bytes:
    """One captured frame, shared by every test in this module.

    These assertions all inspect the SAME properties of the SAME encoder
    output — tag presence, offsets, marker order, UserComment shape. Taking a
    fresh UXGA photo for each of the ~30 of them meant ~30 capture + MQTT
    round-trip + HTTP fetch cycles to re-examine identical bytes, which was
    most of this module's runtime.

    Module-scoped, so it builds its own MQTT and HTTP clients: conftest's
    mqtt_rec and http are deliberately function-scoped (per-test isolation)
    and a broader fixture cannot depend on them.

    Anything that genuinely needs a *fresh* frame must not use this — see
    fresh_jpeg below and the tests that drive their own captures.
    """
    cli = _mqtt_client(f"hil-exif-{os.getpid()}-{int(time.time())}", {"user": "", "password": ""})
    if cli is None:
        pytest.skip("MQTT broker unreachable — cannot trigger a capture")
    rec = _MqttRecorder(cli)
    cli.subscribe(f"{bench_id}/#")
    cli.loop_start()
    time.sleep(0.5)  # let the broker replay retained event/photo for the seq base
    try:
        with httpx.Client(
            base_url=f"https://{bench_ip}",
            verify=False,
            follow_redirects=True,
            timeout=15.0,
            auth=http_basic_creds_optional,
        ) as h:
            return _trigger_and_fetch(rec, h, bench_id)
    finally:
        cli.loop_stop()
        cli.disconnect()


@pytest.fixture
def fresh_jpeg(mqtt_rec, http, bench_id: str) -> bytes:
    """A frame captured for this test alone — for assertions about recency."""
    return _trigger_and_fetch(mqtt_rec, http, bench_id)


@pytest.fixture(scope="module")
def exif_ifd0(jpeg: bytes):
    img = Image.open(io.BytesIO(jpeg))
    e = img.getexif()
    assert len(e) > 0, "JPEG has no EXIF block — APP1 segment likely missing"
    return e


@pytest.fixture(scope="module")
def exif_subifd(exif_ifd0):
    sub = exif_ifd0.get_ifd(0x8769)  # ExifIFDPointer
    assert sub, "ExifIFDPointer present in IFD0 but the sub-IFD is empty"
    return sub


def test_starts_with_app1_marker(jpeg: bytes):
    """First 4 bytes must be SOI (FFD8) + APP1 (FFE1). Sanity check
    that we didn't accidentally strip the EXIF segment somewhere in
    the firmware pipeline (caching, retry queue, MJPEG re-encode)."""
    assert jpeg[:2] == b"\xff\xd8", f"SOI missing: {jpeg[:4].hex()}"
    assert jpeg[2:4] == b"\xff\xe1", (
        f"APP1 marker missing; got {jpeg[2:4].hex()} — JPEG starts with "
        f"SOI but the second segment isn't EXIF"
    )
    # APP1 identifier must be "Exif\0\0" at offset 6.
    assert jpeg[6:12] == b"Exif\x00\x00", f"APP1 identifier not 'Exif\\0\\0': {jpeg[6:12]!r}"


@pytest.mark.parametrize("tag,name", list(REQUIRED_IFD0.items()))
def test_ifd0_tag_present(exif_ifd0, tag: int, name: str):
    assert tag in exif_ifd0, (
        f"IFD0 missing {name} (0x{tag:04X}) — present tags: {sorted(hex(t) for t in exif_ifd0)}"
    )


@pytest.mark.parametrize("tag,name", list(REQUIRED_EXIF_SUBIFD.items()))
def test_subifd_tag_present(exif_subifd, tag: int, name: str):
    assert tag in exif_subifd, (
        f"ExifSubIFD missing {name} (0x{tag:04X}) — present tags: "
        f"{sorted(hex(t) for t in exif_subifd)}"
    )


def test_make_brand(exif_ifd0):
    assert exif_ifd0[0x010F] == "Chytra Budka"


def test_model_matches_bench_id(exif_ifd0, bench_id: str):
    """Model is mqtt_topic_base(), should equal bench_id exactly."""
    assert exif_ifd0[0x0110] == bench_id


def test_software_looks_like_version(exif_ifd0):
    """git-describe style, "<sha>[-dirty]" or a tag — non-empty and
    no embedded NUL leakage (which would indicate a bad strlen)."""
    sw = exif_ifd0[0x0131]
    assert sw, "Software tag is empty"
    assert "\x00" not in sw, f"Software has embedded NUL: {sw!r}"


def test_orientation_normal(exif_ifd0):
    """cam_rotate_180 is applied on the sensor, so the JPEG comes out
    upright and Orientation should always be 1 (Normal)."""
    assert exif_ifd0[0x0112] == 1


def test_datetime_format(exif_ifd0):
    """EXIF DateTime is "YYYY:MM:DD HH:MM:SS" (20 bytes incl NUL).
    Pillow strips the NUL so we check the 19-char form."""
    dt = exif_ifd0[0x0132]
    assert re.fullmatch(r"\d{4}:\d{2}:\d{2} \d{2}:\d{2}:\d{2}", dt), (
        f"DateTime not in EXIF format: {dt!r}"
    )


def test_datetime_recent(fresh_jpeg: bytes):
    """DateTime should be within the last 5 minutes of host clock —
    catches an SNTP regression where the board boots without time and
    writes "1970:01:01 …" instead of a real timestamp.

    Uses its own capture: asserting freshness against the module's shared
    frame would only prove the module ran quickly."""
    dt = Image.open(io.BytesIO(fresh_jpeg)).getexif()[0x0132]
    capture_t = time.mktime(time.strptime(dt, "%Y:%m:%d %H:%M:%S"))
    delta = abs(time.time() - capture_t)
    assert delta < 300, (
        f"DateTime {dt} is {delta:.0f}s away from host clock — SNTP "
        f"not synced or wall clock not yet usable?"
    )


def test_subifd_datetime_matches_ifd0(exif_ifd0, exif_subifd):
    """DateTime, DateTimeOriginal, DateTimeDigitized should all
    reference the same capture moment."""
    assert exif_ifd0[0x0132] == exif_subifd[0x9003] == exif_subifd[0x9004]


def test_offset_time_format(exif_subifd):
    """+HH:MM / -HH:MM (7 bytes incl NUL). Pillow strips the NUL."""
    for tag in (0x9010, 0x9011):
        tz = exif_subifd[tag]
        assert re.fullmatch(r"[+-]\d{2}:\d{2}", tz), (
            f"OffsetTime tag 0x{tag:04X} not in +HH:MM form: {tz!r}"
        )


def test_exif_version_2_30(exif_subifd):
    """ExifVersion is UNDEFINED 4 bytes; Pillow surfaces it as a str
    or bytes depending on version. Accept either."""
    v = exif_subifd[0x9000]
    if isinstance(v, bytes):
        v = v.decode("ascii", errors="replace")
    assert v == "0230", f"ExifVersion {v!r} ≠ '0230'"


def test_lens_model_is_a_real_sensor_name(exif_subifd):
    """LensModel is whatever esp_camera_sensor_get_info() returned for
    the live PID. Don't hardcode "OV3660" — different bench boards
    could in principle ship a different sensor. Just sanity-check
    that it looks like an OV/GC/HM/NT/SC chip name and isn't NUL or
    placeholder garbage."""
    name = exif_subifd[0xA434]
    if isinstance(name, bytes):
        name = name.decode("ascii", errors="replace")
    assert name, "LensModel empty — sensor PID unrecognized?"
    assert "\x00" not in name, f"LensModel has embedded NUL: {name!r}"
    assert re.match(r"^(OV|GC|HM|NT|SC|BF|MEGA)", name), (
        f"LensModel {name!r} doesn't look like a known camera sensor "
        f"family — esp_camera_sensor_get_info() return changed?"
    )


def test_user_comment_required_keys(exif_subifd):
    """UserComment JSON must carry the analysis fields we promised."""
    raw = exif_subifd[0x9286]
    if isinstance(raw, str):
        raw = raw.encode("latin-1")
    uc = _parse_user_comment(raw)
    missing = REQUIRED_UC_KEYS - set(uc.keys())
    assert not missing, (
        f"UserComment JSON missing required keys: {missing} (present: {sorted(uc.keys())})"
    )


def test_user_comment_field_types(exif_subifd):
    """Spot-check field types — a numeric → string regression in the
    sprintf format would surface here before it ruins a year of
    analytics."""
    raw = exif_subifd[0x9286]
    if isinstance(raw, str):
        raw = raw.encode("latin-1")
    uc = _parse_user_comment(raw)
    assert isinstance(uc["seq"], int) and uc["seq"] >= 1
    assert isinstance(uc["uptime_s"], int) and uc["uptime_s"] >= 0
    assert isinstance(uc["capture_ms"], int) and uc["capture_ms"] >= 0
    assert uc["ir"] in (0, 1)
    assert isinstance(uc["framesize"], int)
    assert isinstance(uc["quality"], int)
    # rssi can be 0 right after a fresh WiFi reconnect (firmware reports
    # before the radio has measured anything). Accept that as a sentinel.
    assert isinstance(uc["rssi"], int) and -100 < uc["rssi"] <= 0
    assert isinstance(uc["heap"], int) and uc["heap"] > 0
    # mcu_c is conditional (skipped when isfinite() fails); when emitted
    # it must be in the plausible on-die range.
    if "mcu_c" in uc:
        assert isinstance(uc["mcu_c"], (int, float)) and -40 < uc["mcu_c"] < 120


def test_battery_fields_consistent(exif_subifd):
    """vbatt + soc are either both present (battery_ready()) or both
    absent (probe failed) — never half. Catches a half-init regression
    where battery_ready() returns true but one getter returns NaN."""
    raw = exif_subifd[0x9286]
    if isinstance(raw, str):
        raw = raw.encode("latin-1")
    uc = _parse_user_comment(raw)
    present = BATTERY_UC_KEYS & set(uc.keys())
    assert present in (set(), BATTERY_UC_KEYS), (
        f"battery fields half-present: {present} (should be all of {BATTERY_UC_KEYS} or none)"
    )


# ── Low-level byte-layout tests ───────────────────────────────────────
#
# These walk the raw APP1 segment directly rather than trusting Pillow.
# Their job is to catch regressions in jpeg_stamp.c's hand-rolled TIFF
# builder — places where the value happens to come out right via
# Pillow's forgiving parser even though the bytes violate the spec.


def _find_app1(jpeg: bytes) -> tuple[int, int]:
    """Return (offset of 0xFFE1 marker, total APP1 size incl. marker)."""
    assert jpeg[2:4] == b"\xff\xe1", "second segment is not APP1"
    payload_len = (jpeg[4] << 8) | jpeg[5]  # big-endian, includes itself
    return 2, 2 + payload_len  # marker is 2 bytes


def _find_tag(tiff: bytes, ifd_offset: int, tag_id: int) -> tuple[int, int] | None:
    """Walk an IFD at `ifd_offset` (LE TIFF) looking for `tag_id`.
    Returns (entry_offset, count) when found, else None."""
    count = tiff[ifd_offset] | (tiff[ifd_offset + 1] << 8)
    for i in range(count):
        entry = ifd_offset + 2 + i * 12
        t = tiff[entry] | (tiff[entry + 1] << 8)
        if t == tag_id:
            cnt = (
                tiff[entry + 4]
                | (tiff[entry + 5] << 8)
                | (tiff[entry + 6] << 16)
                | (tiff[entry + 7] << 24)
            )
            return entry, cnt
    return None


def test_app1_length_matches_payload(jpeg: bytes):
    """The big-endian length field at offset 4-5 must equal everything
    that follows up to (but not including) the next segment marker.
    Off-by-one in the length math = downstream parsers walk into the
    JPEG body bytes treating them as more EXIF entries."""
    app1_off, app1_size = _find_app1(jpeg)
    end_off = app1_off + app1_size  # first byte AFTER APP1
    next_marker = jpeg[end_off : end_off + 2]
    # The byte right after APP1 must be a JPEG marker (0xFF then
    # something other than 0xFF/0x00) — typically FFE0 (JFIF), FFDB
    # (quant table), FFC0 (SOF) etc.
    assert next_marker[0] == 0xFF and next_marker[1] not in (0x00, 0xFF), (
        f"APP1 length claims {app1_size}B but byte 0x{end_off:04X} is "
        f"{next_marker.hex()}, not a JPEG marker — length math is off "
        f"(or another EXIF/APP segment follows immediately, which we don't emit)"
    )


def test_no_com_marker_after_app1(jpeg: bytes):
    """The old metadata path injected a JPEG COM (0xFFFE) blob right
    after SOI. Now that we use EXIF (APP1) instead, no COM should
    appear in the first segment chain. Catches a merge that
    accidentally re-introduces inject_com() alongside inject_app1."""
    app1_off, app1_size = _find_app1(jpeg)
    # Walk segments after APP1 until SOS (FFDA) — JPEG metadata segments
    # all live before image data, so this covers the whole header window.
    p = app1_off + app1_size
    while p < len(jpeg) - 1 and jpeg[p] == 0xFF and jpeg[p + 1] != 0xDA:
        marker = (jpeg[p] << 8) | jpeg[p + 1]
        assert marker != 0xFFFE, f"COM marker at offset 0x{p:04X} — inject_com regression?"
        # Skip variable-length segments. Standalone markers (FFD8/FFD9)
        # don't have a length field but we don't expect those here.
        if marker in (0xFFD8, 0xFFD9):
            break
        seg_len = (jpeg[p + 2] << 8) | jpeg[p + 3]
        p += 2 + seg_len


def test_image_description_inline_offset_per_spec(jpeg: bytes):
    """Exif § "Type and Count": ASCII payload ≤ 4 bytes (incl NUL) MUST
    sit inline in the 4-byte value field. > 4 bytes must use an offset
    into the TIFF. emit_ascii() implements this; catch a regression
    that flips the branch (the bug that initially shipped — trigger
    "vad\\0" written as offset surfaced as \\xb0\\0\\0\\0 in viewers)."""
    app1_off, _ = _find_app1(jpeg)
    tiff = jpeg[app1_off + 4 + 6 :]  # skip marker(2) + length(2) + "Exif\0\0"(6)
    # IFD0 offset = bytes 4..7 of TIFF header (LE).
    ifd0 = tiff[4] | (tiff[5] << 8) | (tiff[6] << 16) | (tiff[7] << 24)
    found = _find_tag(tiff, ifd0, 0x010E)
    assert found, "ImageDescription tag not present in IFD0"
    entry_off, n = found
    value_field = tiff[entry_off + 8 : entry_off + 12]
    if n <= 4:
        # Inline: first n bytes of value_field must be the ASCII string
        # incl. terminating NUL. Remaining padding bytes are spec-free
        # but emit_ascii zeroes them.
        assert value_field[n - 1] == 0, (
            f"inline ASCII for tag 0x010E (count={n}) missing NUL terminator: {value_field.hex()}"
        )
        # The string itself must be printable.
        s = value_field[: n - 1].decode("ascii", errors="replace")
        assert s.isprintable(), f"inline ImageDescription not printable: {s!r}"
    else:
        # Offset path: value_field is a LE u32 offset into TIFF.
        off = (
            value_field[0] | (value_field[1] << 8) | (value_field[2] << 16) | (value_field[3] << 24)
        )
        # External-data region starts at offset 188 per the layout in
        # build_exif_app1(). Anything smaller would overlap an IFD.
        assert off >= 188, (
            f"ImageDescription offset {off} points inside the IFDs "
            f"(should be ≥ 188, the external-data region start)"
        )
        # Bytes at that offset must be the NUL-terminated string.
        blob = tiff[off : off + n]
        assert blob[-1] == 0, f"offset ASCII for tag 0x010E missing NUL: {blob.hex()}"


def test_image_description_inline_path_forced(mqtt_rec, http, bench_id: str):
    """Force emit_ascii's INLINE branch (≤4-byte payload incl NUL) and
    verify the bytes land in the 4-byte value field, not at an offset.

    Production triggers — "mqtt"/"reed_open"/"timelapse"/"http" — are
    all >4 bytes (strlen+NUL) so every default capture takes the offset
    path. The inline branch is where the original `\\xb0\\0\\0\\0` bug
    lived (offset incorrectly written for "vad\\0" / "pir\\0"); the
    fix mandates inline storage when count ≤ 4 per Exif §"Type and
    Count".  We exercise it here by POSTing trigger="pir" (3 chars +
    NUL = 4 bytes) to /debug/capture and then walking the resulting
    JPEG's IFD0 to confirm:

      1. tag 0x010E has count == 4 (not 0 — would mean tag missing — and
         not >4 — would mean we somehow got an offset path),
      2. the 4-byte value field is exactly "pir\\0" (not the offset
         bytes that the original bug surfaced).

    The endpoint is gated on CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS; this
    test skips cleanly on a production build where it returns 404.
    """
    state_topic = f"{bench_id}/event/photo"

    sent_at = time.time()
    # httpx's data= kwarg form-encodes (sends "pir=", which the handler
    # then accepts as the trigger up to the strip — surfaced as count=5
    # in CI, not 4). content= sends the raw bytes the handler expects.
    r = http.post("/debug/capture", content=b"pir", timeout=5.0)
    if r.status_code == 404:
        pytest.skip(
            "/debug/capture not registered — CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS=n "
            "build; can't exercise the inline-ASCII branch on this firmware"
        )
    assert r.status_code == 200, f"POST /debug/capture returned {r.status_code}: {r.text[:200]}"

    # Wait for event/photo where trigger=="pir". The trigger filter
    # protects against a concurrent cmd/photo from another test runner
    # racing in and giving us the wrong frame.
    def _is_pir(payload: bytes) -> bool:
        try:
            obj = json.loads(payload.decode())
            return obj.get("trigger") == "pir"
        except (json.JSONDecodeError, AttributeError):
            return False

    mqtt_rec.wait_for(state_topic, _is_pir, timeout=15.0, since=sent_at)

    # Race: camera.c publishes event/photo (line 674) BEFORE refreshing
    # the /last.jpg PSRAM cache (line 700). On a fast tester /last.jpg
    # still holds the previous test's frame at the moment event/photo
    # lands. Poll for up to ~3 s for /last.jpg to carry our trigger —
    # either the cache catches up (expected, ~50 ms) or we conclude a
    # foreign frame raced in and bail.
    deadline = time.time() + 3.0
    entry_off = -1
    count = -1
    tiff = b""
    while time.time() < deadline:
        jpg = http.get("/last.jpg", timeout=10.0).content
        app1_off, _ = _find_app1(jpg)
        tiff = jpg[app1_off + 4 + 6 :]
        ifd0 = tiff[4] | (tiff[5] << 8) | (tiff[6] << 16) | (tiff[7] << 24)
        found = _find_tag(tiff, ifd0, 0x010E)
        if found:
            entry_off, count = found
            # count==4 (inline path) is what we're waiting for; if the
            # cache still holds the previous test's "mqtt" frame
            # (count=5, offset) we sleep + retry. Once count drops back
            # to 4 we drop out and assert on the value bytes.
            if count == 4:
                break
        time.sleep(0.1)

    # strlen("pir") + NUL = 4 → must take the inline branch. count != 4
    # after the poll window means either emit_ascii regressed (the bug
    # under test) or /last.jpg held a different frame for the whole 3 s
    # (probably a concurrent test runner — flag the contention).
    assert count == 4, (
        f"expected count==4 for inline pir\\0 trigger, got count={count} "
        f"after 3 s of polling — either emit_ascii flipped to offset "
        f"(the bug under test) or /last.jpg never settled on the pir "
        f"frame (concurrent capture?)"
    )
    value_field = bytes(tiff[entry_off + 8 : entry_off + 12])
    assert value_field == b"pir\x00", (
        f"inline value bytes wrong: {value_field!r} != b'pir\\0' — "
        f"this is the original \\xb0\\0\\0\\0 regression class"
    )


# ── firmware's own EXIF reader (exif_read.c) — read/write round-trip ───
# The native test (tests/native/test_exif.c) round-trips the reader against a
# hand-built blob; these check it against the REAL jpeg_stamp.c writer on
# hardware, surfaced through the /last.json + /photo/exif + /view endpoints.


def test_last_json_matches_pillow(mqtt_rec, http, bench_id: str):
    """/last.json (parsed on-device by exif_read.c) must agree with what
    Pillow reads out of the very same frame — the firmware reader vs an
    independent parser, against the actual writer."""
    _trigger_and_fetch(mqtt_rec, http, bench_id)
    j = http.get("/last.json", timeout=10.0).json()
    raw = http.get("/last.jpg", timeout=10.0).content  # same cached frame
    assert j.get("exif") is True, f"/last.json: {j}"

    img = Image.open(io.BytesIO(raw))
    e = img.getexif()
    sub = e.get_ifd(0x8769)
    assert j["model"] == e[0x0110] == bench_id
    assert j["software"] == e[0x0131]
    assert j["trigger"] == e[0x010E]
    assert j["datetime"] == sub[0x9003]  # firmware reader == Pillow

    tel = j.get("telemetry")
    assert isinstance(tel, dict), f"telemetry should be a JSON object: {tel!r}"
    for k in ("seq", "agc", "ir", "rssi", "heap"):
        assert k in tel, f"telemetry missing {k}: {tel}"


def test_photo_exif_endpoint(mqtt_rec, http, bench_id: str):
    """/photo/exif?d=&f= parses a *stored* photo's EXIF. Pick the newest file
    from /photos.json and assert the JSON reports a real clock + telemetry."""
    _trigger_and_fetch(mqtt_rec, http, bench_id)  # ensure ≥1 photo on the card
    days = http.get("/photos.json", timeout=10.0).json()["days"]
    assert days, "no photo day buckets on the card"
    day = days[0]
    files = http.get(f"/photos.json?day={day}", timeout=10.0).json()["files"]
    assert files, f"day {day} has no files"
    f = files[0]["f"]

    j = http.get(f"/photo/exif?d={day}&f={f}", timeout=10.0).json()
    assert j.get("exif") is True, f"/photo/exif {day}/{f}: {j}"
    assert j["datetime"], "datetime empty"
    assert isinstance(j.get("telemetry"), dict)


def test_view_page_renders(mqtt_rec, http, bench_id: str):
    """/view?d=&f= returns the HTML viewer (image + EXIF table)."""
    _trigger_and_fetch(mqtt_rec, http, bench_id)
    days = http.get("/photos.json", timeout=10.0).json()["days"]
    day = days[0]
    files = http.get(f"/photos.json?day={day}", timeout=10.0).json()["files"]
    f = files[0]["f"]
    r = http.get(f"/view?d={day}&f={f}", timeout=10.0)
    assert r.status_code == 200, f"/view {r.status_code}: {r.text[:200]}"
    body = r.text
    assert "EXIF metadata" in body, "viewer missing the EXIF table heading"
    assert f"/photo?d={day}&amp;f={f}" in body, "viewer missing the <img> source"
