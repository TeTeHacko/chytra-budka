"""cbprom.py — Chytrá Budka MQTT → Prometheus bridge.

Subscribes to the per-device retained `state/*` topics published by every
budka firmware build, converts each scalar payload into a Prometheus
metric, and exposes them on `:9878/metrics` for Grafana Alloy (or any
Prometheus scraper) to pull and `remote_write` into Mimir.

Why translate MQTT → Prom on a host instead of running `/metrics` on the
ESP32:
  - The data already exists on the broker; firmware change would
    duplicate effort and burn power per scrape (WiFi wake every N s).
  - Per-device scrape targets in Alloy would need service discovery —
    here one daemon handles every budka via wildcard subscribe.
  - We can tag metrics with operator-friendly labels (`device_name=
    "budka-example-1"`) lifted from a config table, mirroring how the
    UC96 power meter exporter labels its meters. The two datasources
    join cleanly on `device_name` in Grafana.

Pattern + structure mirrors [`power-meter/uc96d.py`](../power-meter/uc96d.py)
so an operator who can read one can read the other.

Config: TOML at `/etc/cb-prom/config.toml` (override with `--config`).
"""

from __future__ import annotations

import argparse
import json
import logging
import math
import re
import signal
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib  # type: ignore[no-redef]

import paho.mqtt.client as mqtt
from prometheus_client import (
    CollectorRegistry,
    Gauge,
    Info,
    start_http_server,
)

log = logging.getLogger("cbprom")

# ─── Topic + label parsing ──────────────────────────────────────────────────

# `cb-ex01/state/temp` → mactail="ex01", subtopic="temp"
# `cb-ex01/state/cfg/vad_thr_dbfs` → mactail + subtopic with cfg/
# device_id is "cb-<6hex>" (firmware device_id.c); the `device` Prometheus label
# is the bare mactail ("ex01").
TOPIC_RE = re.compile(r"^cb-(?P<mactail>[a-f0-9]+)/state/(?P<sub>.+)$")

# `cb-ex01/diag/selftest` — fan-out one JSON payload into many
# scalar gauges. State topics are atomic per-key; selftest is a structured
# snapshot republished after every MQTT (re)connect plus on-demand from
# /selftest HTTP. Subscribed alongside state/# so SD + photo-queue health
# reaches Mimir without per-key MQTT publishes.
DIAG_SELFTEST_RE = re.compile(r"^cb-(?P<mactail>[a-f0-9]+)/diag/selftest$")

# selftest "summary" string format: "ok (12/14)" / "degraded (9/14)".
# Component counts feed cb_selftest_components_ok/total gauges so a
# Grafana panel can graph "how healthy is each box" without parsing a
# string label.
SUMMARY_RE = re.compile(r"^(?:ok|degraded)\s+\((?P<ok>\d+)/(?P<total>\d+)\)$")

# `cb-ex01/diag/boot` — JSON crash/boot context. Mapped so the
# crash-loop depth (consecutive_crashes) and coredump presence reach Prometheus
# for alerting, not just the retained MQTT topic + GlitchTip.
DIAG_BOOT_RE = re.compile(r"^cb-(?P<mactail>[a-f0-9]+)/diag/boot$")

# `cb-ex02/meter/aabbcc000002/power` — external BLE UC96 USB-C power
# meters read NATIVELY by the firmware (mqtt.c::mqtt_publish_uc96), NOT the
# retired host-side uc96d.py exporter. Unlike the single on-board INA226 solar
# sensor (state/solar_*), the field rig daisy-chains several meters, so each is
# keyed by its own MAC → a third `meter` label on top of device/device_name.
# These publish under meter/, not state/, so TOPIC_RE never matched them and the
# data only reached Home Assistant (via discovery), never Mimir.
METER_RE = re.compile(
    r"^cb-(?P<mactail>[a-f0-9]+)/meter/(?P<meter>[a-f0-9]+)/"
    r"(?P<field>voltage|current|power|energy|temperature)$"
)

# Cardinality guards. cbprom subscribes to wildcard topics on a shared LAN
# broker, so a buggy or hostile publisher could otherwise mint unbounded
# Prometheus series — one per fake device label, one gauge per fake cfg knob —
# growing the registry (memory + scrape size) without limit. Bound both.
MAX_DEVICES = 64
MAX_CFG_KNOBS = 128
MAX_METERS = 64  # cardinality guard on the per-meter MAC label (meter/<mac>/…)

# diag/boot + state/ota fan-out gauges (pre-registered so absent() works).
DIAG_GAUGES = {
    "consecutive_crashes": "Consecutive crash-loop boots (RTC counter; 0 = healthy)",
    "coredump_present": "1 if a coredump is stored on the device awaiting extraction",
    "coredump_bytes": "Size of the stored coredump in bytes (0 if none)",
    "ota_ok": "Last OTA result: 1 = ok/idle/done, 0 = error",
}


# ─── Conversion helpers ─────────────────────────────────────────────────────
#
# All firmware payloads are ASCII text. The pub() helper in mqtt.c uses
# snprintf("%.2f"), snprintf("%d"), or the literal strings "ON"/"OFF" /
# "OPEN"/"CLOSED" / "online"/"offline". Each conversion below returns a
# float (Prometheus' native type) or None (skip this sample — preserves
# the previous value rather than poisoning the timeline with NaN).


def to_float(payload: str) -> float | None:
    try:
        v = float(payload)
    except ValueError:
        return None
    if not math.isfinite(v):
        # firmware load-clamp catches NaN/inf on the device side, but the
        # bridge runs forever and a broker quirk shouldn't push junk into
        # Mimir if one slips through.
        return None
    return v


def to_int(payload: str) -> float | None:
    try:
        return float(int(payload))
    except ValueError:
        return None


def to_bool_onoff(payload: str) -> float | None:
    """ON/OFF/true/false/1/0 → 1.0/0.0. Anything else → None."""
    p = payload.strip().upper()
    if p in ("ON", "TRUE", "1"):
        return 1.0
    if p in ("OFF", "FALSE", "0"):
        return 0.0
    return None


def to_bool_door(payload: str) -> float | None:
    """OPEN/CLOSED → 1.0/0.0 (matches HA's dev_cla=door: open=on)."""
    p = payload.strip().upper()
    if p == "OPEN":
        return 1.0
    if p == "CLOSED":
        return 0.0
    return None


def to_bool_avail(payload: str) -> float | None:
    """`online`/`offline` (firmware LWT convention) → 1.0/0.0."""
    p = payload.strip().lower()
    if p == "online":
        return 1.0
    if p == "offline":
        return 0.0
    return None


# ─── Metric definitions ─────────────────────────────────────────────────────
#
# `subtopic` is the suffix after `state/` — must match exactly what
# mqtt.c::register_topics builds. Adding a new firmware topic = adding a
# row here (and presumably the dashboard panel that uses it). The two
# canonical labels are `device` (stable MAC suffix, always present) and
# `device_name` (friendly label from config[devices]; defaults to
# `device` if the mactail isn't mapped, so the dashboard still renders).


@dataclass
class MetricDef:
    name: str  # prom_<name> after the prefix
    help: str
    kind: str  # "gauge", "counter", or "info"
    convert: Callable[[str], float | None] | None = None
    info_label: str | None = None  # for kind="info", the label key for payload


# Subtopic-keyed registry. cfg/* is handled separately (dynamic).
METRICS: dict[str, MetricDef] = {
    # Availability (LWT) — gauge, 1=online, 0=offline.
    "availability": MetricDef(
        "online", "Device online (1) or LWT-flagged offline (0)", "gauge", to_bool_avail
    ),
    # Battery + power
    "soc": MetricDef("battery_soc_percent", "MAX17048 fuel gauge SOC", "gauge", to_float),
    "v_bat": MetricDef("battery_voltage_volts", "Battery terminal voltage", "gauge", to_float),
    "crate": MetricDef(
        "battery_charge_rate_pct_per_hour",
        "MAX17048 estimated charge/discharge rate",
        "gauge",
        to_float,
    ),
    "solar_v": MetricDef("solar_voltage_volts", "INA226 solar input voltage", "gauge", to_float),
    "solar_i": MetricDef("solar_current_amps", "INA226 solar input current", "gauge", to_float),
    "solar_p": MetricDef("solar_power_watts", "INA226 solar input power", "gauge", to_float),
    # Env
    "temp": MetricDef(
        "temperature_celsius_inside", "SHT41 ambient temp inside enclosure", "gauge", to_float
    ),
    "humidity": MetricDef(
        "humidity_percent_inside", "SHT41 ambient humidity inside", "gauge", to_float
    ),
    "temp_ext": MetricDef(
        "temperature_celsius_outside", "SHT41 ambient temp outside (bus1)", "gauge", to_float
    ),
    "humidity_ext": MetricDef(
        "humidity_percent_outside", "SHT41 ambient humidity outside (bus1)", "gauge", to_float
    ),
    "mcu_temp": MetricDef(
        "mcu_temperature_celsius", "ESP32-S3 internal temp sensor", "gauge", to_float
    ),
    # Connectivity + uptime
    "rssi": MetricDef("wifi_rssi_dbm", "WiFi RSSI to AP", "gauge", to_float),
    "uptime_s": MetricDef("uptime_seconds", "Seconds since last boot", "gauge", to_float),
    "heap_free": MetricDef("heap_free_bytes", "Internal SRAM free heap", "gauge", to_float),
    # Audio
    "rms_dbfs": MetricDef("audio_rms_dbfs", "Last computed audio RMS in dBFS", "gauge", to_float),
    "burst_count": MetricDef(
        "audio_burst_total",
        "Total VAD-triggered audio bursts since boot "
        "(resets on reboot; HA total_increasing recovers)",
        "counter",
        to_int,
    ),
    "chunks_sent": MetricDef(
        "audio_chunks_sent_total", "Audio relay chunks sent over TCP since boot", "counter", to_int
    ),
    "streaming": MetricDef(
        "audio_streaming", "Currently streaming audio relay (1/0)", "gauge", to_bool_onoff
    ),
    # Camera + capture
    "capture_count": MetricDef(
        "photo_capture_total", "Total photo captures since boot", "counter", to_int
    ),
    "ambient_agc": MetricDef(
        "camera_agc_gain", "OV3660 reported AGC gain (0=bright .. 30=dark)", "gauge", to_float
    ),
    # PIR / Reed
    "motion": MetricDef(
        "motion_active", "PIR motion pending consume (1/0)", "gauge", to_bool_onoff
    ),
    "motion_count": MetricDef(
        "motion_total", "Total PIR motion events since boot", "counter", to_int
    ),
    "reed": MetricDef(
        "reed_open", "Reed switch / door: 1=open (magnet away), 0=closed", "gauge", to_bool_door
    ),
    "reed_count": MetricDef(
        "reed_events_total", "Reed state-change events since boot", "counter", to_int
    ),
    # Info-only (string payload becomes a label, value always 1).
    # Prometheus can't aggregate these arithmetically, but Grafana
    # table panels + `cb_mode_info{mode="continuous"}` filters work.
    # NB: prometheus_client.Info auto-appends `_info` to the metric
    # name, so the `name` field below must NOT carry that suffix or
    # we'd emit `cb_mode_info_info` (double suffix).
    "mode": MetricDef("mode", "Current ModeFsm state", "info", info_label="mode"),
    "reset_reason": MetricDef(
        "reset_reason", "Reset cause from diag_capture_boot", "info", info_label="reason"
    ),
    "fw_version": MetricDef(
        "fw_version",
        "Running firmware version (raw JSON blob "
        "as published by firmware mqtt.c — parse "
        "for git_sha + build date)",
        "info",
        info_label="version",
    ),
}

# Prefix every metric so a flat Mimir browse like `cb_*` filters us
# cleanly without colliding with uc96 (`uc96_*`) or HA (`hass_*`).
METRIC_PREFIX = "cb_"

# meter/<mac>/<field> family. Carries an extra `meter` label (the meter's BT
# MAC) on top of device/device_name, so these can't share self.gauges (2 labels)
# — registered into self.meter_gauges (3 labels). All payloads are plain ASCII
# floats/ints (mqtt.c uses %.2f/%.3f/%d), so to_float parses every field; energy
# is the meter's own accumulating Wh counter (monotonic across budka reboots —
# the meter, not the firmware, owns it — so PromQL rate()/increase() apply).
METER_METRICS: dict[str, tuple[str, str]] = {
    "voltage": ("meter_voltage_volts", "UC96 BLE meter bus voltage"),
    "current": ("meter_current_amps", "UC96 BLE meter bus current"),
    "power": ("meter_power_watts", "UC96 BLE meter power (V*I)"),
    "energy": ("meter_energy_wh", "UC96 BLE meter accumulated energy (monotonic; use rate())"),
    "temperature": ("meter_temperature_celsius", "UC96 BLE meter internal temperature"),
}


# Diag/selftest gauges. One JSON payload from firmware fans out into this
# whole set on every selftest publish. Kept separate from METRICS because
# METRICS is keyed by state/<subtopic>, while these all share the single
# diag/selftest topic. Names + help strings live here so adding a new
# selftest field is a single-row change. `_total` suffix follows
# prometheus convention even though firmware resets on reboot — Mimir's
# rate()/increase() over Gauge handles resets the same way they do for
# the firmware-resetting counters already in METRICS.
SELFTEST_GAUGES: dict[str, str] = {
    "sd_free_percent": "SD card free space percent from selftest",
    "sd_used_megabytes": "SD card used MB from selftest",
    "sd_total_megabytes": "SD card total MB from selftest",
    "sd_ready": "SD card mounted (1) or absent/failed (0)",
    "photo_queue_depth": "Retry queue depth right now",
    "photo_queue_dropped_total": "Retry FIFO overflows since boot (firmware-resetting; use rate())",
    "photo_queue_drained_total": "Retry re-publishes since boot (firmware-resetting; use rate())",
    "photo_publish_errors_total": "MQTT publish errors for /image/photo since boot (firmware-resetting)",
    "cam_request_drops_total": "Capture requests dropped since boot (firmware-resetting)",
    "selftest_components_ok": "Selftest components healthy in the latest snapshot",
    "selftest_components_total": "Selftest components checked in the latest snapshot",
    "pir_wedged": "PIR stuck-HIGH (1=jammed sensor, 0=normal)",
    "mic_stalled": "Mic delivered no frames since last selftest in Continuous (1=stalled)",
    "cam_stalled": "Camera ready but every recent capture failed (1=wedged sensor)",
}


# ─── Daemon ─────────────────────────────────────────────────────────────────


@dataclass
class Config:
    mqtt_host: str
    mqtt_port: int
    mqtt_user: str
    mqtt_pass: str
    listen_port: int
    devices: dict[str, str] = field(default_factory=dict)
    # meter BT MAC (lowercase, no colons — as the firmware publishes it) →
    # friendly name. Mirrors `devices`; unmapped meters fall back to the MAC.
    meters: dict[str, str] = field(default_factory=dict)


def load_config(path: Path) -> Config:
    with path.open("rb") as f:
        raw = tomllib.load(f)
    m = raw.get("mqtt", {})
    e = raw.get("exporter", {})
    devices = raw.get("devices", {})
    meters = raw.get("meters", {})
    # Require the broker host explicitly — silently defaulting to the prod
    # broker IP (the old behaviour) means a config typo connects to the wrong
    # place instead of failing loudly. (12-factor III: config from the env.)
    host = m.get("host")
    if not host:
        raise SystemExit(f"{path}: [mqtt] host is required (no implicit default)")
    return Config(
        mqtt_host=host,
        mqtt_port=int(m.get("port", 1883)),
        # `_path` suffix on creds means "read this file once at startup".
        # Lets the operator keep MQTT_PASS out of the world-readable
        # config.toml and inside a 0600 secret file owned by the daemon
        # user. Falls back to the literal string for dev convenience.
        mqtt_user=_read_secret(m, "user", default=""),
        mqtt_pass=_read_secret(m, "password", default=""),
        listen_port=int(e.get("port", 9878)),
        devices=devices,
        meters=meters,
    )


def _read_secret(section: dict[str, Any], key: str, default: str) -> str:
    """Prefer `<key>_path` (file) over `<key>` (inline) so secrets stay
    out of /etc/cb-prom/config.toml's mode bits."""
    path = section.get(f"{key}_path")
    if path:
        return Path(path).read_text().strip()
    return str(section.get(key, default))


class Bridge:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self.registry = CollectorRegistry()
        self.gauges: dict[str, Gauge] = {}
        # Info metrics in prometheus_client need the label set fixed at
        # construction; we hold the live label dict here and rewrite on
        # change so HA-style "mode flipped from continuous to safe" is a
        # single time series, not two zombie ones.
        self.infos: dict[str, Info] = {}
        for m in METRICS.values():
            full = METRIC_PREFIX + m.name
            if m.kind == "gauge":
                self.gauges[m.name] = Gauge(
                    full, m.help, ["device", "device_name"], registry=self.registry
                )
            elif m.kind == "counter":
                # prometheus_client.Counter is monotonic; the firmware
                # resets on reboot which mqtt2prom can't pretend
                # otherwise. Use Gauge so Mimir + HA recorder handle the
                # reset via the standard `rate()`/`increase()` heuristics
                # for total_increasing (PromQL rate() handles counter
                # resets natively, but a Python Counter would reject a
                # decreasing set() call).
                self.gauges[m.name] = Gauge(
                    full,
                    m.help + " (firmware-resetting; use rate())",
                    ["device", "device_name"],
                    registry=self.registry,
                )
            elif m.kind == "info":
                self.infos[m.name] = Info(
                    full, m.help, ["device", "device_name"], registry=self.registry
                )

        # Dynamic family for state/cfg/* — one gauge per knob, mostly
        # numeric. Bools (cap_led_en, reed_enabled, ota_enabled, …)
        # become 0/1 via to_bool_onoff. Discovered lazily on first
        # message so adding a schema knob in firmware doesn't need a
        # config redeploy here.
        self.cfg_gauges: dict[str, Gauge] = {}

        # diag/selftest fan-out gauges. Pre-registered so they appear in
        # /metrics with empty series until the first payload lands —
        # Grafana panels using `absent()` won't return "unknown metric"
        # for a budka that hasn't published selftest yet.
        self.selftest_gauges: dict[str, Gauge] = {}
        for short, help_text in SELFTEST_GAUGES.items():
            self.selftest_gauges[short] = Gauge(
                METRIC_PREFIX + short, help_text, ["device", "device_name"], registry=self.registry
            )

        # diag/boot + state/ota gauges (crash-loop depth, coredump presence,
        # last-OTA result) — the highest-value reliability signals, pre-
        # registered so a Grafana alert on absent()/value works before a board
        # has published its first boot/ota.
        self.diag_gauges: dict[str, Gauge] = {}
        for short, help_text in DIAG_GAUGES.items():
            self.diag_gauges[short] = Gauge(
                METRIC_PREFIX + short, help_text, ["device", "device_name"], registry=self.registry
            )

        # meter/<mac>/* fan-out gauges. Extra `meter` label (the meter's BT MAC)
        # → a 3-label family, so kept separate from self.gauges. Pre-registered
        # so /metrics lists them (empty series) before the first meter reports.
        self.meter_gauges: dict[str, Gauge] = {}
        for mfield, (name, help_text) in METER_METRICS.items():
            self.meter_gauges[mfield] = Gauge(
                METRIC_PREFIX + name,
                help_text,
                ["device", "device_name", "meter", "name"],
                registry=self.registry,
            )
        # Freshness signal. prometheus_client gauges keep exposing their last
        # set() value forever, so a meter that drops its BLE link leaves
        # cb_meter_* FLATLINED (not absent) — a graph would look alive and no
        # absent()-based alert would fire. Stamp wall-clock on every accepted
        # meter sample so `time() - cb_meter_last_seen_timestamp_seconds`
        # measures true staleness and BudkaMeterStale can page.
        self.meter_last_seen = Gauge(
            METRIC_PREFIX + "meter_last_seen_timestamp_seconds",
            "Unix time of the last telemetry sample received from this meter",
            ["device", "device_name", "meter", "name"],
            registry=self.registry,
        )

        # Bounded set of device mactails we've accepted — cardinality guard.
        self.seen_devices: set[str] = set()
        # Same guard for the meter MAC label (one per physical UC96).
        self.seen_meters: set[str] = set()
        self._client: mqtt.Client | None = None  # set in run(); for clean SIGTERM

    # Resolve the friendly label for a mactail. Falls back to the
    # mactail itself so a freshly-added board still shows up in Grafana
    # without a config redeploy.
    def device_name(self, mactail: str) -> str:
        return self.cfg.devices.get(mactail, mactail)

    # Friendly name for a meter MAC, same fallback contract as device_name:
    # an unmapped meter still gets a usable `name` label (its MAC).
    def meter_name(self, meter_id: str) -> str:
        return self.cfg.meters.get(meter_id, meter_id)

    def _accept_device(self, mactail: str) -> bool:
        """Cardinality guard: accept a bounded number of distinct device labels.
        A rogue/buggy publisher minting fake mactails can't grow the registry
        without limit. Already-seen devices always pass."""
        if mactail in self.seen_devices:
            return True
        if len(self.seen_devices) >= MAX_DEVICES:
            log.warning("device cardinality cap (%d) reached — dropping %s", MAX_DEVICES, mactail)
            return False
        self.seen_devices.add(mactail)
        return True

    def _accept_meter(self, meter_id: str) -> bool:
        """Cardinality guard for the per-meter MAC label, mirroring
        _accept_device — a rogue publisher can't mint unbounded meter series."""
        if meter_id in self.seen_meters:
            return True
        if len(self.seen_meters) >= MAX_METERS:
            log.warning("meter cardinality cap (%d) reached — dropping %s", MAX_METERS, meter_id)
            return False
        self.seen_meters.add(meter_id)
        return True

    def handle_message(self, topic: str, payload: bytes) -> None:
        # Decode once; pass through for boolean parsers that work on
        # case-folded strings. Skip if payload isn't valid UTF-8 (no
        # firmware topic publishes binary).
        try:
            text = payload.decode("utf-8")
        except UnicodeDecodeError:
            return

        diag = DIAG_SELFTEST_RE.match(topic)
        if diag:
            mactail = diag.group("mactail")
            if not self._accept_device(mactail):
                return
            labels = {"device": mactail, "device_name": self.device_name(mactail)}
            self._handle_selftest(text, labels, topic)
            return

        boot = DIAG_BOOT_RE.match(topic)
        if boot:
            mactail = boot.group("mactail")
            if not self._accept_device(mactail):
                return
            labels = {"device": mactail, "device_name": self.device_name(mactail)}
            self._handle_diag_boot(text, labels, topic)
            return

        meter = METER_RE.match(topic)
        if meter:
            mactail = meter.group("mactail")
            if not self._accept_device(mactail):
                return
            meter_id = meter.group("meter")
            if not self._accept_meter(meter_id):
                return
            v = to_float(text)
            if v is None:
                log.debug("skip %s: unparseable %r", topic, text)
                return
            labels = {
                "device": mactail,
                "device_name": self.device_name(mactail),
                "meter": meter_id,
                "name": self.meter_name(meter_id),
            }
            self.meter_gauges[meter.group("field")].labels(**labels).set(v)
            self.meter_last_seen.labels(**labels).set(time.time())
            return

        m = TOPIC_RE.match(topic)
        if not m:
            return
        mactail = m.group("mactail")
        if not self._accept_device(mactail):
            return
        sub = m.group("sub")
        labels = {"device": mactail, "device_name": self.device_name(mactail)}

        if sub.startswith("cfg/"):
            self._handle_cfg(sub[4:], text, labels)
            return

        if sub == "ota":
            # state/ota: "done"/"idle"/"skipped"/"checking" → ok (1), "error" → 0.
            self.diag_gauges["ota_ok"].labels(**labels).set(
                0.0 if text.strip().lower() == "error" else 1.0
            )
            return

        md = METRICS.get(sub)
        if md is None:
            return
        if md.kind == "info":
            # info_label captures the string value as the only label so a
            # PromQL query like `cb_mode_info{mode="continuous"}` works.
            assert md.info_label
            self.infos[md.name].labels(**labels).info({md.info_label: text})
            return
        v = md.convert(text) if md.convert else None
        if v is None:
            log.debug("skip %s: unparseable %r", topic, text)
            return
        self.gauges[md.name].labels(**labels).set(v)

    def _handle_selftest(self, payload: str, labels: dict[str, str], topic: str) -> None:
        """diag/selftest handler — JSON fan-out to selftest_gauges.

        Firmware publishes the full snapshot on every (re)connect plus on
        on-demand /selftest hits. Missing keys (e.g. sd_used_mb when no
        card is mounted) are skipped — the previous value sticks. Use
        `absent_over_time(cb_sd_total_megabytes{device="x"}[1h])` if a
        Grafana panel wants to flag "no card has ever been seen".
        """
        try:
            obj = json.loads(payload)
        except json.JSONDecodeError:
            log.warning("selftest %s: invalid JSON (%d bytes)", topic, len(payload))
            return
        if not isinstance(obj, dict):
            return

        def set_gauge(short: str, val: float) -> None:
            self.selftest_gauges[short].labels(**labels).set(val)

        # Components healthy/total — derived from the "summary" string.
        summary = obj.get("summary", "")
        sm = SUMMARY_RE.match(summary) if isinstance(summary, str) else None
        if sm:
            set_gauge("selftest_components_ok", float(sm.group("ok")))
            set_gauge("selftest_components_total", float(sm.group("total")))

        # `sd` boolean is always emitted by firmware (it's in the checks
        # table). The MB/percent fields only appear when the card is
        # mounted — so sd_ready is the reliable "is there a card right
        # now" signal, the capacity ones are nice-to-have.
        sd_ok = obj.get("sd")
        if isinstance(sd_ok, bool):
            set_gauge("sd_ready", 1.0 if sd_ok else 0.0)
        for key, short in (
            ("sd_free_pct", "sd_free_percent"),
            ("sd_used_mb", "sd_used_megabytes"),
            ("sd_total_mb", "sd_total_megabytes"),
        ):
            v = obj.get(key)
            if isinstance(v, (int, float)):
                set_gauge(short, float(v))

        # Photo retry-queue + capture-drop counters. All firmware-
        # resetting; treated as gauges so a reboot mid-day surfaces as a
        # downward step rather than a paho-side schema error.
        for key, short in (
            ("photo_queue_depth", "photo_queue_depth"),
            ("photo_queue_dropped", "photo_queue_dropped_total"),
            ("photo_queue_drained", "photo_queue_drained_total"),
            ("photo_publish_errors", "photo_publish_errors_total"),
            ("cam_request_drops", "cam_request_drops_total"),
        ):
            v = obj.get(key)
            if isinstance(v, (int, float)):
                set_gauge(short, float(v))

        # Stuck-HIGH PIR — distinct from "no sensor" because the boolean
        # `pir` check can't tell those apart.
        wedged = obj.get("pir_wedged")
        if isinstance(wedged, bool):
            set_gauge("pir_wedged", 1.0 if wedged else 0.0)

        # Post-boot mic/camera death — "online + ok summary but not actually
        # delivering" blind spots. The product-level SLI (is the cam producing
        # images) keys off cam_stalled + the photo counters.
        for key in ("mic_stalled", "cam_stalled"):
            v = obj.get(key)
            if isinstance(v, bool):
                set_gauge(key, 1.0 if v else 0.0)

    def _handle_diag_boot(self, payload: str, labels: dict[str, str], topic: str) -> None:
        """diag/boot handler — surfaces crash-loop depth + coredump presence so
        they're alertable in Prometheus, not just in the retained topic."""
        try:
            obj = json.loads(payload)
        except json.JSONDecodeError:
            log.warning("diag/boot %s: invalid JSON (%d bytes)", topic, len(payload))
            return
        if not isinstance(obj, dict):
            return
        cc = obj.get("consecutive_crashes")
        if isinstance(cc, (int, float)) and not isinstance(cc, bool):
            self.diag_gauges["consecutive_crashes"].labels(**labels).set(float(cc))
        cd = obj.get("coredump")
        if isinstance(cd, bool):
            self.diag_gauges["coredump_present"].labels(**labels).set(1.0 if cd else 0.0)
        cb_bytes = obj.get("coredump_bytes")
        if isinstance(cb_bytes, (int, float)) and not isinstance(cb_bytes, bool):
            self.diag_gauges["coredump_bytes"].labels(**labels).set(float(cb_bytes))

    def _handle_cfg(self, knob: str, payload: str, labels: dict[str, str]) -> None:
        """state/cfg/<knob> handler — single dynamic gauge family per knob.

        Numeric knobs (vad_thr_dbfs, reed_db_ms, …) parse via to_float.
        Bool knobs (cap_led_en, ir_led_enabled, …) parse via
        to_bool_onoff (ON/OFF). Discovery is lazy so any future SCHEMA
        addition lights up here without code change.
        """
        # Empty payload = the firmware actively cleared a deprecated
        # discovery (capture_led_enabled → cap_led_en, reed_debounce_ms
        # → reed_db_ms). Don't materialize a metric for the orphan
        # name — it would forever stale-read 0 with no underlying truth.
        if payload == "":
            return
        full = METRIC_PREFIX + "cfg_" + knob
        g = self.cfg_gauges.get(knob)
        if g is None:
            if len(self.cfg_gauges) >= MAX_CFG_KNOBS:
                log.warning(
                    "cfg knob cardinality cap (%d) reached — dropping %r", MAX_CFG_KNOBS, knob
                )
                return
            g = Gauge(
                full,
                f"NVS knob {knob} (numeric=raw, bool=ON/OFF→1/0)",
                ["device", "device_name"],
                registry=self.registry,
            )
            self.cfg_gauges[knob] = g
        # Try numeric first; fall back to bool. If neither parses (mode
        # override happens to be an int but mode_override schema also
        # supports a label-info pattern we don't surface here), skip.
        v = to_float(payload)
        if v is None:
            v = to_bool_onoff(payload)
        if v is None:
            return
        g.labels(**labels).set(v)

    # MQTT callbacks ────────────────────────────────────────────────────────
    def on_connect(self, client, userdata, flags, reason_code, properties=None):
        if reason_code != 0:
            log.error("mqtt connect failed: %s", reason_code)
            return
        log.info("mqtt connected to %s:%d", self.cfg.mqtt_host, self.cfg.mqtt_port)
        # Subscribe to every retained state topic from every device on the
        # broker; TOPIC_RE filters out non-budka traffic. MQTT spec gotcha:
        # `+` must occupy a whole level — `cb-+/state/#` is
        # INVALID (mosquitto rejects, paho raises) because `+` isn't the
        # entire level. Same surprise as in tools/mqtt.sh's comment.
        # diag/selftest carries the SD/photo-queue snapshot — handled via
        # its own JSON parser, see _handle_selftest.
        # meter/# carries the natively-read external BLE power meters
        # (meter/<mac>/{voltage,current,power,energy,temperature}); see METER_RE.
        client.subscribe(
            [
                ("+/state/#", 0),
                ("+/diag/selftest", 0),
                ("+/diag/boot", 0),
                ("+/meter/#", 0),
            ]
        )

    def on_message(self, client, userdata, msg):
        try:
            self.handle_message(msg.topic, msg.payload)
        except Exception:
            log.exception("handler failed for %s", msg.topic)

    def on_disconnect(self, client, userdata, flags, reason_code, properties=None):
        # paho's loop_forever() will reconnect with exponential backoff
        # automatically; we just log so an operator tailing the journal
        # can correlate Mimir gaps with broker hiccups.
        log.warning("mqtt disconnect: %s", reason_code)

    def run(self) -> None:
        start_http_server(self.cfg.listen_port, registry=self.registry)
        log.info("metrics on :%d/metrics", self.cfg.listen_port)
        client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id="cbprom",
            reconnect_on_failure=True,
        )
        if self.cfg.mqtt_user:
            client.username_pw_set(self.cfg.mqtt_user, self.cfg.mqtt_pass)
        client.on_connect = self.on_connect
        client.on_message = self.on_message
        client.on_disconnect = self.on_disconnect
        # connect_async (not connect): if the broker is down at startup
        # (boot-order race despite After=network-online.target), connect()
        # would raise ConnectionRefusedError and the daemon would exit
        # BEFORE loop_forever()'s reconnect logic engages — leaving systemd
        # to crash-loop the process every RestartSec. connect_async defers
        # the TCP connect into loop_forever(), which retries with backoff.
        self._client = client
        client.connect_async(self.cfg.mqtt_host, self.cfg.mqtt_port, keepalive=30)
        client.loop_forever()

    def stop(self) -> None:
        """Break loop_forever() cleanly (called from the SIGTERM handler).
        An explicit disconnect() makes paho's loop_forever return rather than
        reconnecting, so run() unwinds and the MQTT session closes tidily
        instead of being severed by interpreter exit."""
        if self._client is not None:
            self._client.disconnect()


# ─── Entrypoint ─────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    # __doc__ is None under `python -OO` (strips docstrings). Fall back
    # to a literal so argparse doesn't crash on a stripped install.
    desc = (__doc__ or "Chytrá Budka MQTT → Prometheus bridge").split("\n", 1)[0]
    parser = argparse.ArgumentParser(description=desc)
    parser.add_argument("--config", type=Path, default=Path("/etc/cb-prom/config.toml"))
    parser.add_argument(
        "--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"]
    )
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=args.log_level,
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )

    if not args.config.exists():
        log.error("config not found: %s", args.config)
        return 2
    cfg = load_config(args.config)
    bridge = Bridge(cfg)

    # SIGTERM from systemd → clean shutdown. paho's loop_forever traps
    # KeyboardInterrupt internally but not SIGTERM, so disconnect explicitly:
    # that unwinds loop_forever and closes the MQTT session, rather than
    # severing it by interpreter exit (the old sys.exit(0) dropped the socket).
    def _sigterm(_signum, _frame):
        log.info("SIGTERM — disconnecting")
        bridge.stop()

    signal.signal(signal.SIGTERM, _sigterm)

    try:
        bridge.run()
    except KeyboardInterrupt:
        log.info("KeyboardInterrupt — exiting")
    return 0


if __name__ == "__main__":
    sys.exit(main())
