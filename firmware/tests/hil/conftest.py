"""
Phase A HIL fixtures — talk to a powered-on bench board over the
network. Future iterations will layer pytest-embedded on top of this
to add flash/monitor; the fixtures here are designed to stay valid
either way (HTTP/MQTT fixtures don't care whether the firmware came
from a USB flash or an OTA pull).

Environment overrides:
  CB_BENCH_PORT  — udev symlink, e.g. /dev/esp32-aabbccddee01.
                   Asserted to be a symlink so a bare /dev/ttyACMn
                   (which shuffles across replug) fails fast.
  CB_BENCH_IP    — overrides the IP discovered from the symlink MAC.
                   The default IP map is hardcoded for the two bench
                   boards the user has on hand; override for a new
                   board until that map grows.
  CB_MQTT_HOST   — broker host, default cb.example.com (the fleet broker).
  CB_MQTT_PORT   — broker port, default 8883 (mTLS).
  CB_MQTT_CERT / CB_MQTT_KEY / CB_MQTT_CAFILE — client certificate for the
                   mTLS listener; defaults to server/secrets/hil-runner.*
                   plus ca_chain.pem. Issue one with
                   server/scripts/issue-client-cert.sh hil-runner. The ACL
                   scopes that identity to the bench board only.
  CB_MQTT_USER / CB_MQTT_PASS — username/password instead, for pointing the
                   suite at a plain local broker. Only used when no client
                   certificate is present.

`bench_port` asserts the symlink target IS the bench board, not the
field board, before any test runs — this is the same allowlist idea
as tools/flash_safe.sh but enforced from the test side, so a stray
CB_BENCH_PORT=/dev/esp32-aabbccddee02 can't run a test that might
reboot the field unit.
"""

from __future__ import annotations

import os
import re
import socket
import subprocess
import sys
import time
from collections.abc import Callable, Iterator
from pathlib import Path

import httpx
import paho.mqtt.client as mqtt
import pytest

# ── Bench identity ────────────────────────────────────────────────────────

# Allowed bench MAC suffixes (lowercase, no colons). Keep in sync with
# tools/devices.txt. Field boards (OTA-only) must NOT appear here.
BENCH_ALLOWLIST: dict[str, str] = {
    # Prefer the .lan hostname when possible (DHCP-stable, survives
    # router renumbering); the IP here is just the fallback for hosts
    # without local DNS. Update via `host cb-<id>.lan`.
    "aabbccddee01": "198.51.100.90",  # cb-ex01 — USB test rig
    "aabbccddee03": "198.51.100.107",  # cb-ex03 — onboarded 2026-07-10
    "aabbccddee04": "198.51.100.108",  # cb-ex04 — onboarded 2026-07-10
    "aabbccddee05": "198.51.100.109",  # cb-ex05 — onboarded 2026-07-10
}

DEFAULT_PORT = "/dev/esp32-aabbccddee01"
# The fleet broker. Tests used to default to the house HA broker on
# 192.0.2.5 — that one carries zigbee2mqtt and the rest of the home, and
# leaning on it for firmware tests was borrowing a production service. 8883 is
# the port the fleet already exposes, so testing against the real thing costs
# nothing extra; the runner authenticates with its own scoped certificate.
DEFAULT_MQTT_HOST = "cb.example.com"
DEFAULT_MQTT_PORT = 8883

_SECRETS_DIR = Path(__file__).resolve().parents[3] / "server" / "secrets"
DEFAULT_MQTT_CERT = _SECRETS_DIR / "hil-runner.pem"
DEFAULT_MQTT_KEY = _SECRETS_DIR / "hil-runner.key"
DEFAULT_MQTT_CAFILE = _SECRETS_DIR / "ca_chain.pem"


def _mqtt_auth(cli) -> None:
    """Apply mTLS (preferred) or username/password to a paho client.

    A client certificate is how the runner reaches the fleet broker. Falling
    back to username/password keeps a plain local broker usable — set
    CB_MQTT_USER/CB_MQTT_PASS and point CB_MQTT_HOST/PORT at it.
    """
    cert = Path(os.environ.get("CB_MQTT_CERT", DEFAULT_MQTT_CERT))
    key = Path(os.environ.get("CB_MQTT_KEY", DEFAULT_MQTT_KEY))
    cafile = Path(os.environ.get("CB_MQTT_CAFILE", DEFAULT_MQTT_CAFILE))
    if cert.exists() and key.exists() and cafile.exists():
        cli.tls_set(ca_certs=str(cafile), certfile=str(cert), keyfile=str(key))
        return
    user = os.environ.get("CB_MQTT_USER")
    pw = os.environ.get("CB_MQTT_PASS")
    if not user or not pw:
        pytest.skip(
            f"no MQTT client certificate at {cert} and no CB_MQTT_USER/"
            "CB_MQTT_PASS — issue one with "
            "server/scripts/issue-client-cert.sh hil-runner"
        )
    cli.username_pw_set(user, pw)


SECRETS_H = Path(__file__).resolve().parent.parent.parent / "main" / "secrets.h"


def _parse_secrets() -> dict[str, str]:
    """Extract MQTT credentials from firmware/main/secrets.h.

    The file is `#define KEY "value"` style. We only care about
    MQTT_USER and MQTT_PASSWORD here; everything else is ignored.
    """
    if not SECRETS_H.is_file():
        return {}
    out: dict[str, str] = {}
    pat = re.compile(r'^\s*#define\s+(\w+)\s+"([^"]*)"')
    for line in SECRETS_H.read_text().splitlines():
        m = pat.match(line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


@pytest.fixture(scope="session")
def bench_port() -> str:
    """Validated symlink path to the bench board's USB serial.

    Fails fast if the path isn't a symlink (raw ttyACMn shuffles) or
    if its MAC tail isn't in the bench allowlist. This is the same
    allowlist idea as tools/flash_safe.sh — defensive against a
    distracted env var pointing at the field board.
    """
    port = os.environ.get("CB_BENCH_PORT", DEFAULT_PORT)
    if not os.path.exists(port):
        pytest.skip(f"bench port {port} not present — is the board plugged in?")
    if not os.path.islink(port):
        pytest.fail(
            f"{port} is not a symlink. Use /dev/esp32-<mac> per the udev rule "
            "in memory/bench_boards.md; raw /dev/ttyACMn shuffles across replug."
        )
    name = os.path.basename(port)
    m = re.match(r"^esp32-([0-9a-f]{12})$", name)
    if not m:
        pytest.fail(f"{port} basename '{name}' does not match esp32-<12-hex-mac>")
    mac = m.group(1)
    if mac not in BENCH_ALLOWLIST:
        pytest.fail(
            f"MAC {mac} is not on the HIL bench allowlist. Field boards stay "
            "off this list so tests can't accidentally reboot a deployed unit."
        )
    return port


@pytest.fixture(scope="session")
def bench_mac(request) -> str:
    """Bench MAC tail (12 hex, lowercase, no colons).

    Primary source: the USB symlink basename from `bench_port`. Fallback:
    `CB_BENCH_MAC=<12hex>` env var — lets tests that only need WiFi /
    MQTT / HTTP run when the bench is powered but not plugged in over
    USB (e.g. battery-only deployment under continuous observation).
    Either way the MAC must be on the allowlist.
    """
    env_mac = os.environ.get("CB_BENCH_MAC", "").lower()
    if env_mac:
        if env_mac not in BENCH_ALLOWLIST:
            pytest.fail(f"CB_BENCH_MAC={env_mac!r} is not on the bench allowlist.")
        return env_mac
    # No env override → derive from the USB port symlink.
    port: str = request.getfixturevalue("bench_port")
    return os.path.basename(port).removeprefix("esp32-")


@pytest.fixture(scope="session")
def bench_id(bench_mac: str) -> str:
    """Per-device MQTT root, e.g. cb-ex01."""
    # firmware/main/device_id.c renders the last 3 MAC bytes as 6 lowercase
    # hex (no separator) → suffix; id = "cb-<suffix>". bench_mac is the 12-hex
    # MAC, so the last 3 bytes are chars 6..12.
    tail = bench_mac[6:12]
    return f"cb-{tail}"


@pytest.fixture(scope="session")
def bench_ip(bench_mac: str, provisioned_sta: str | None) -> str:
    """IPv4 of the bench.

    In the lifecycle run this is the DHCP address the board picked up *after*
    the provision step (`provisioned_sta` discovers it by MAC). CB_BENCH_IP
    overrides; the allowlist entry is the last-resort fallback for ad-hoc runs
    against an already-provisioned board on the legacy VLAN.
    """
    if os.environ.get("CB_BENCH_IP"):
        return os.environ["CB_BENCH_IP"]
    if provisioned_sta:
        return provisioned_sta
    return BENCH_ALLOWLIST.get(bench_mac, "")


# ── HTTP ──────────────────────────────────────────────────────────────────


@pytest.fixture(scope="session")
def http_basic_creds_optional() -> tuple[str, str] | None:
    """Web-admin (user, pass) if set to real values, else None (gate disabled).
    Unlike http_basic_creds this does NOT skip — used to auto-auth the `http`
    client so it works whether or not the gate is on."""
    s = _parse_secrets()
    u = os.environ.get("CB_HTTP_USER") or s.get("HTTP_BASIC_USER")
    p = os.environ.get("CB_HTTP_PASS") or s.get("HTTP_BASIC_PASS")
    if (
        u
        and p
        and not u.startswith(("your-", "placeholder-"))
        and not p.startswith(("your-", "placeholder-"))
    ):
        return (u, p)
    return None


@pytest.fixture
def http(bench_ip: str, http_basic_creds_optional) -> Iterator[httpx.Client]:
    """Client for the bench web server as a logged-in operator sees it.

    Targets HTTPS and follows the :80→:443 redirect that an enrolled board
    serves (verify=False — the leaf is signed by the private rfa CA the test
    host doesn't trust), and carries the web-admin basic-auth creds so gated
    endpoints (/selftest, /capture, /stream.mjpg, …) return 200 for the smoke /
    contract tests. Tests that need UNauthenticated or scheme-specific behaviour
    use the `https` fixture with an explicit auth= instead.

    Function-scoped — each test gets a fresh connection pool. 5 s default
    timeout; longer ops (capture, OTA dry-run) pass timeout= explicitly.
    """
    with httpx.Client(
        base_url=f"https://{bench_ip}",
        verify=False,
        timeout=10.0,
        follow_redirects=True,
        auth=http_basic_creds_optional,
    ) as client:
        yield client


@pytest.fixture
def https(bench_ip: str) -> Iterator[httpx.Client]:
    """httpx.Client over HTTPS (:443), for tests that need the basic-auth
    gate active (the gate is HTTPS-only). verify=False because the leaf is
    signed by the per-fleet private rfa CA the test host doesn't trust;
    follow_redirects so a :80 hit lands on :443. Skips if HTTPS isn't up
    (board not enrolled — gate inactive, nothing to test)."""
    client = httpx.Client(
        base_url=f"https://{bench_ip}",
        verify=False,
        timeout=12.0,
        follow_redirects=True,
    )
    try:
        client.get("/", timeout=8.0)
    except httpx.HTTPError as e:
        client.close()
        pytest.skip(f"HTTPS not reachable on {bench_ip}:443 ({e}) — board not enrolled?")
    yield client
    client.close()


# ── AP-mode (onboarding portal) join, on the HIL host's wlan0 ──────────────
#
# The recovery / unprovisioned AP portal lives at 172.31.4.1 and is only
# reachable by JOINING the bench's SoftAP. With a single wlan0 that means
# disconnecting the host from its station LAN for the duration — invasive, so
# it's confined to the `ap_join` fixture used by @ap_mode tests. nmcli does the
# switching; the prior connection is restored on teardown. Everything skips
# gracefully if nmcli is absent or the AP can't be joined (e.g. the bench is
# currently provisioned/STA, not in AP mode).

AP_PORTAL_URL = "http://172.31.4.1"


def _nm(*args: str, timeout: float = 30.0) -> subprocess.CompletedProcess:
    return subprocess.run(["nmcli", *args], capture_output=True, text=True, timeout=timeout)


def _wifi_device() -> str | None:
    """The machine's WiFi interface, whatever NetworkManager calls it.

    This used to be hardcoded to "wlan0". On a predictable-names host it is
    wls17 / wlp3s0 / …, so the lookup below always came back empty, `prev` was
    always None, and the post-test reconnect became a no-op — the runner stayed
    parked on the bench's own AP. Every HTTP test then failed with "no route to
    host" while the board was perfectly healthy.
    """
    try:
        r = _nm("-t", "-f", "DEVICE,TYPE", "device")
    except (FileNotFoundError, subprocess.SubprocessError):
        return None
    for line in r.stdout.splitlines():
        dev, _, typ = line.rpartition(":")
        if typ == "wifi":
            return dev
    return None


def _active_wifi_con() -> str | None:
    """Name of the connection currently up on the WiFi interface."""
    dev = _wifi_device()
    if not dev:
        return None
    try:
        r = _nm("-t", "-f", "NAME,DEVICE", "connection", "show", "--active")
    except (FileNotFoundError, subprocess.SubprocessError):
        return None
    for line in r.stdout.splitlines():
        name, _, d = line.rpartition(":")  # rpartition: names may contain ':'
        if d == dev:
            return name
    return None


def _station_con() -> str | None:
    """The connection to return to after visiting a bench AP.

    Not simply "whatever was active": a run that died mid-test leaves the host
    on a bench AP, and restoring *that* would strand it again. Prefer the
    station network the boards are provisioned onto (CB_PROVISION_SSID, which
    is also the LAN the HTTP tests need to reach), and only fall back to the
    active connection when it is not itself a bench AP.
    """
    ssid = os.environ.get("CB_PROVISION_SSID")
    if ssid:
        try:
            r = _nm("-t", "-f", "NAME,TYPE", "connection", "show")
            for line in r.stdout.splitlines():
                name, _, typ = line.rpartition(":")
                if "wireless" in typ and name == ssid:
                    return name
        except (FileNotFoundError, subprocess.SubprocessError):
            pass
    active = _active_wifi_con()
    if active and not re.match(r"^(cb-[0-9a-f]{6}|chytra-budka-)", active):
        return active
    return None


def _join_ap(ssid: str, password: str, tries: int = 5) -> bool:
    """Connect wlan0 to the bench AP (retry — nmcli scan cache is flaky)."""
    for _ in range(tries):
        try:
            _nm("device", "wifi", "rescan")
            time.sleep(4)
            j = _nm("-w", "25", "device", "wifi", "connect", ssid, "password", password)
        except (FileNotFoundError, subprocess.SubprocessError):
            return False
        out = (j.stdout + j.stderr).lower()
        if "successfully" in out or "activated" in out:
            return True
    return False


def _reconnect(name: str | None, tries: int = 3) -> bool:
    """Bring the station connection back up and CONFIRM it took.

    Verified, not fire-and-forget: leaving the runner on a bench AP makes every
    later HTTP test fail with "no route to host", which reads like a dead board
    rather than a host still attached to the wrong network.
    """
    if not name:
        return False
    for _ in range(tries):
        try:
            _nm("connection", "up", name)
        except (FileNotFoundError, subprocess.SubprocessError):
            return False
        time.sleep(3)
        if _active_wifi_con() == name:
            return True
    print(
        f"[hil] WARNING: could not restore WiFi connection {name!r}; "
        "the runner may still be on the bench AP",
        file=sys.stderr,
    )
    return False


def _read_onboard_ap_pass(port: str, timeout: float = 45.0) -> str | None:
    """Read the bench's per-boot RANDOM SoftAP password from its serial console.

    A display-equipped unprovisioned bench uses a random AP password (so a fresh
    board isn't reachable on the public default) and shows it only on the OLED
    QR — unknowable to this serial-free harness. The firmware also echoes it to
    the LOCAL console every ~30 s while in the onboarding portal
    ("onboarding AP creds ... pass=<…>"), which we parse here. Best-effort:
    returns None if pyserial is absent, the port won't open, or no creds line
    appears within `timeout` (caller then falls back to the compiled default)."""
    try:
        import serial  # pyserial — optional dep (see requirements.txt)
    except ImportError:
        return None
    pat = re.compile(rb"onboarding AP creds.*pass=([a-z0-9]+)")
    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200
        ser.timeout = 1.0
        # NOTE: opening the S3's USB-serial-JTAG port RESETS the board (a known
        # quirk — see memory/project_oled_bmp388). That's fine: we read through
        # the ~15 s reboot and grab the creds line the firmware logs on the AP
        # boot (and every ~30 s after). DTR/RTS left deasserted so we at least
        # don't hold it in reset / drop it into the download stub.
        ser.dtr = False
        ser.rts = False
        ser.open()
    except Exception:
        return None
    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = ser.readline()
            except Exception:
                break
            m = pat.search(line)
            if m:
                return m.group(1).decode()
    finally:
        try:
            ser.close()
        except Exception:
            pass
    return None


def _resolve_ap_pass(port: str) -> str:
    """AP password to join the bench SoftAP: explicit CB_AP_PASS override, else
    the per-boot random password read from the bench console (display-equipped
    onboarding bench), else the compiled default (display-less bench)."""
    env = os.environ.get("CB_AP_PASS")
    if env:
        return env
    return _read_onboard_ap_pass(port) or "chytrabudka"


@pytest.fixture(scope="session")
def ap_ssid(bench_id: str) -> str:
    """The bench AP SSID. AP_SSID_FMT is "cb-%s" with the device-id suffix,
    which equals bench_id. Override with CB_AP_SSID."""
    return os.environ.get("CB_AP_SSID", bench_id)


@pytest.fixture(scope="session")
def ap_pass(bench_port: str) -> str:
    """Bench AP WPA2 password. CB_AP_PASS override → the per-boot random
    onboarding password read from the console (display-equipped bench) → the
    compiled default. See _resolve_ap_pass / _read_onboard_ap_pass."""
    return _resolve_ap_pass(bench_port)


@pytest.fixture
def ap_join(ap_ssid: str, ap_pass: str) -> Iterator[httpx.Client]:
    """Join the bench AP on wlan0 and yield an httpx.Client for the portal
    (http://172.31.4.1). Restores the prior wlan0 connection on teardown.
    Skips if the AP can't be joined (bench not in AP mode / no nmcli)."""
    prev = _station_con()
    if not _join_ap(ap_ssid, ap_pass):
        _reconnect(prev)
        pytest.skip(
            f"could not join bench AP {ap_ssid!r} — is the bench in AP/"
            "unprovisioned mode and is nmcli available on the host?"
        )
    time.sleep(3)
    client = httpx.Client(base_url=AP_PORTAL_URL, timeout=15.0)
    try:
        yield client
    finally:
        client.close()
        _reconnect(prev)


@pytest.fixture(scope="session")
def provision_creds() -> dict[str, str]:
    """Real WiFi creds for the provisioning test, from the environment. Set
    CB_PROVISION_SSID + CB_PROVISION_PSK (e.g. exported from NetworkManager
    without echoing). Skips when absent so the test never hard-codes a secret."""
    ssid = os.environ.get("CB_PROVISION_SSID")
    psk = os.environ.get("CB_PROVISION_PSK")
    if not ssid or not psk:
        pytest.skip("set CB_PROVISION_SSID + CB_PROVISION_PSK to run the provisioning test")
    return {"ssid": ssid, "psk": psk}


@pytest.fixture(scope="session")
def compile_wifi_real() -> bool:
    """True iff secrets.h has a REAL compile-time WIFI_SSID (not blank or a
    placeholder). When false, a factory reset lands the board in the
    unprovisioned AP portal — NOT back on the broker's network — so any
    STA/MQTT-recovery test that assumes the board comes back online on its own
    must skip (the HIL host is on the station LAN and can't reach the AP)."""
    ssid = _parse_secrets().get("WIFI_SSID", "")
    return bool(ssid) and not ssid.startswith(("your-", "placeholder-"))


@pytest.fixture(scope="session")
def http_basic_creds() -> dict[str, str]:
    """Web-admin (HTTP basic-auth) username/password from env or secrets.h.
    Skips when they're placeholders (gate disabled, nothing to test)."""
    s = _parse_secrets()
    u = os.environ.get("CB_HTTP_USER") or s.get("HTTP_BASIC_USER")
    p = os.environ.get("CB_HTTP_PASS") or s.get("HTTP_BASIC_PASS")
    placeholder = (
        not u
        or not p
        or u.startswith(("your-", "placeholder-"))
        or p.startswith(("your-", "placeholder-"))
    )
    if placeholder:
        pytest.skip("HTTP basic creds are placeholders — auth gate disabled")
    return {"user": u, "password": p}


# ── MQTT ──────────────────────────────────────────────────────────────────


class _MqttRecorder:
    """Records messages on subscribed topics for wait_for() lookups.

    paho's loop_start() runs callbacks on a background thread; we
    just stash every (topic, payload, timestamp) into a list under a
    lock so tests can call wait_for() to poll for the value they
    care about without writing their own threading boilerplate.
    """

    def __init__(self, client: mqtt.Client):
        self._client = client
        self._messages: list[tuple[str, bytes, float]] = []
        import threading

        self._lock = threading.Lock()
        client.on_message = self._on_message

    def _on_message(self, _c, _u, msg: mqtt.MQTTMessage) -> None:
        with self._lock:
            self._messages.append((msg.topic, msg.payload, time.time()))

    def wait_for(
        self,
        topic: str,
        predicate: Callable[[bytes], bool] = lambda _: True,
        *,
        timeout: float = 10.0,
        since: float = 0.0,
    ) -> bytes:
        """Wait until a message matching `predicate` arrives on `topic`.

        `since` lets a test ignore retained values older than a known
        timestamp (e.g. set right before publishing a cmd). Returns the
        raw payload, raises TimeoutError on timeout.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                for t, payload, ts in self._messages:
                    if t == topic and ts >= since and predicate(payload):
                        return payload
            time.sleep(0.05)
        raise TimeoutError(f"no message on {topic!r} matching predicate within {timeout}s")

    def latest(self, topic: str) -> bytes | None:
        """Latest payload seen on `topic`, or None if never seen."""
        with self._lock:
            for t, payload, _ in reversed(self._messages):
                if t == topic:
                    return payload
        return None


@pytest.fixture(scope="session")
def mqtt_creds() -> dict[str, str]:
    """Username/password pair, from env or secrets.h — may be empty.

    Only used when there is no client certificate (see _mqtt_auth), so a
    missing pair is not fatal: the fleet broker authenticates the runner with
    mTLS. _mqtt_auth is the one that decides and skips if neither is usable.
    """
    user = os.environ.get("CB_MQTT_USER")
    pw = os.environ.get("CB_MQTT_PASS")
    if not user or not pw:
        s = _parse_secrets()
        user = user or s.get("MQTT_USER")
        pw = pw or s.get("MQTT_PASSWORD")
    return {"user": user or "", "password": pw or ""}


@pytest.fixture
def mqtt_rec(
    bench_id: str, mqtt_creds: dict[str, str], provisioned_sta: str | None
) -> Iterator[_MqttRecorder]:
    """paho-mqtt client subscribed to <bench_id>/# with a _MqttRecorder.

    Function-scoped to keep tests independent — each test starts with
    only the retained messages the broker replays on subscribe, no
    leakage from a prior test's transient publishes.

    Depends on `provisioned_sta` because the session opens with a factory reset
    (reset_board): the board is in its SoftAP and off the station LAN until
    something provisions it, so a board reachable over MQTT is a precondition
    here, not an assumption. Most MQTT-only modules never ask for `bench_ip`, so
    without this they could only ever pass as part of a full run that happened to
    include test_provision — alone, they factory-reset the bench and then timed
    out waiting for a board that was sitting in AP mode. Session-scoped, so in a
    full run this is the cache entry test_provision already created (and the
    phase ordering in pytest_collection_modifyitems still runs the @ap_mode
    tests, which want the AP up, before any of this).
    """
    host = os.environ.get("CB_MQTT_HOST", DEFAULT_MQTT_HOST)
    port = int(os.environ.get("CB_MQTT_PORT", DEFAULT_MQTT_PORT))

    # paho-mqtt 2.x requires callback_api_version; we pinned >=2.1 in
    # requirements.txt. Pyright sometimes runs against stale 1.x stubs
    # that don't know about CallbackAPIVersion — silence it here, the
    # runtime check in pip install enforces the actual minimum.
    client = mqtt.Client(  # type: ignore[call-arg]
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,  # type: ignore[attr-defined]
        client_id=f"hil-{os.getpid()}-{int(time.time())}",
    )
    _mqtt_auth(client)

    try:
        client.connect(host, port, keepalive=30)
    except (OSError, socket.gaierror) as e:
        pytest.skip(f"MQTT broker {host}:{port} unreachable: {e}")

    rec = _MqttRecorder(client)
    client.subscribe(f"{bench_id}/#")
    client.loop_start()
    # Give the broker a moment to replay retained messages.
    time.sleep(0.5)
    try:
        yield rec
    finally:
        client.loop_stop()
        client.disconnect()


@pytest.fixture
def cfg(mqtt_rec: _MqttRecorder, bench_id: str):
    """Setter for cmd/cfg/<key> with state/cfg/<key> echo verification.

    Usage:
        cfg("pir_enabled", "ON")
        cfg("vad_thr_dbfs", "-40")

    Publishes the value, waits up to 15s for the retained state echo
    to confirm the device accepted + persisted it. Returns the
    decoded echo string.

    The round-trip is normally ~0.2-0.8s; the generous default absorbs
    intermittent multi-second stalls on this bench (a cfg whose apply hook
    reconfigures the wedge-prone bus1 I2C — e.g. pin_d6_fn/pin_d7_fn — or the
    prio-1 supervisor being momentarily starved). 5s was too tight and flaked
    different cfg-using tests run to run; the field round-trip is unaffected.
    """

    def _set(key: str, value: str | int | float, *, timeout: float = 15.0) -> str:
        sent_at = time.time()
        payload = str(value)
        mqtt_rec._client.publish(f"{bench_id}/cmd/cfg/{key}", payload, qos=1, retain=False)
        echo = mqtt_rec.wait_for(
            f"{bench_id}/state/cfg/{key}",
            lambda p: p.decode() == payload,
            timeout=timeout,
            since=sent_at,
        )
        return echo.decode()

    return _set


# ── Lifecycle: reset → AP phase → provision → STA phase ───────────────────
#
# The end-to-end flow the deploy gate runs (firmware/tests/hil/README.md):
#   0. reset_board (autouse) — factory-reset over MQTT → board boots
#      unprovisioned into the AP onboarding portal.
#   1. @ap_mode tests — the complete AP/onboarding suite (joins the SoftAP).
#   2. provisioned_sta — POST the station creds on /wifi, wait for STA + MQTT
#      online, discover the DHCP IP by MAC (the AP→STA transition is a test).
#   3. @sta_mode tests + everything unmarked — the complete connected suite
#      against the discovered IP.
# pytest_collection_modifyitems (below) enforces that order.


def _mqtt_client(client_id: str, mqtt_creds: dict) -> mqtt.Client | None:
    host = os.environ.get("CB_MQTT_HOST", DEFAULT_MQTT_HOST)
    port = int(os.environ.get("CB_MQTT_PORT", DEFAULT_MQTT_PORT))
    cli = mqtt.Client(  # type: ignore[call-arg]
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,  # type: ignore[attr-defined]
        client_id=client_id,
    )
    _mqtt_auth(cli)
    try:
        cli.connect(host, port, keepalive=30)
    except (OSError, socket.gaierror):
        return None
    return cli


def _wait_availability(bench_id, mqtt_creds, want: bytes, *, timeout, since=0.0) -> bool:
    """Wait until <bench_id>/state/availability == want. False on timeout.

    Keeps retrying the *connection* until the deadline instead of giving up the
    moment the first attempt fails. Callers reach here right after the host has
    hopped off the bench AP back onto the LAN, and name resolution is routinely
    still broken for a few seconds afterwards — a single failed connect used to
    return False instantly, which the fixtures then reported as "the board never
    came back", sending you to debug firmware that was already online.
    """
    deadline = time.time() + timeout
    connected_at_least_once = False
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            log_hint = " (never reached the broker at all)" if not connected_at_least_once else ""
            print(
                f"[hil] {bench_id}/state/availability != {want!r} within {timeout:.0f}s{log_hint}",
                file=sys.stderr,
            )
            return False
        cli = _mqtt_client(f"hil-wait-{os.getpid()}-{int(time.time())}", mqtt_creds)
        if cli is None:
            time.sleep(min(3.0, max(0.5, remaining)))
            continue
        connected_at_least_once = True
        rec = _MqttRecorder(cli)
        cli.subscribe(f"{bench_id}/state/availability")
        cli.loop_start()
        try:
            rec.wait_for(
                f"{bench_id}/state/availability",
                lambda p: p.strip() == want,
                timeout=deadline - time.time(),
                since=since,
            )
            return True
        except TimeoutError:
            return False
        finally:
            cli.loop_stop()
            cli.disconnect()


def _set_cfg_verified(bench_id, mqtt_creds, key, value, *, tries=6, per_try=8.0) -> bool:
    """Publish cmd/cfg/<key>=value and CONFIRM the state/cfg/<key> echo matches,
    retrying. Returns True once confirmed, False if it never echoed. Survives the
    reconnect blips that silently drop a fire-and-forget publish (the bug that
    let the bench self-OTA to a stale server image before ota_enabled=OFF took)."""
    cli = _mqtt_client(f"hil-cfg-{os.getpid()}-{int(time.time())}", mqtt_creds)
    if cli is None:
        return False
    rec = _MqttRecorder(cli)
    cli.subscribe(f"{bench_id}/state/cfg/{key}")
    cli.loop_start()
    try:
        for _ in range(tries):
            sent = time.time()
            cli.publish(f"{bench_id}/cmd/cfg/{key}", value, qos=1, retain=False)
            try:
                rec.wait_for(
                    f"{bench_id}/state/cfg/{key}",
                    lambda p: p.decode().strip() == value,
                    timeout=per_try,
                    since=sent,
                )
                return True
            except TimeoutError:
                continue
        return False
    finally:
        cli.loop_stop()
        cli.disconnect()


def _host_wlan_subnet() -> str | None:
    try:
        r = subprocess.run(
            ["ip", "-4", "addr", "show", "wlan0"], capture_output=True, text=True, timeout=5
        )
    except (FileNotFoundError, subprocess.SubprocessError):
        return None
    m = re.search(r"inet (\d+\.\d+\.\d+)\.\d+/\d+", r.stdout)
    return f"{m.group(1)}.0/24" if m else None


def _discover_ip_by_mac(mac12: str, *, tries: int = 8) -> str | None:
    """Find the bench's current IPv4 on the host's wlan0 subnet by MAC.

    The board's DHCP address on the station LAN isn't known ahead of time, so
    after provisioning we sweep the subnet (nmap if present) and match the MAC
    in `ip neigh`. mac12 e.g. 'aabbccddee01'."""
    import shutil

    mac = ":".join(mac12[i : i + 2] for i in range(0, 12, 2)).lower()
    subnet = _host_wlan_subnet()
    have_nmap = bool(shutil.which("nmap"))
    for _ in range(tries):
        if subnet and have_nmap:
            try:
                subprocess.run(
                    ["nmap", "-sn", "-n", "-T4", subnet], capture_output=True, text=True, timeout=90
                )
            except subprocess.SubprocessError:
                pass
        try:
            r = subprocess.run(["ip", "neigh"], capture_output=True, text=True, timeout=5)
            for line in r.stdout.splitlines():
                if mac in line.lower() and "FAILED" not in line:
                    return line.split()[0]
        except (FileNotFoundError, subprocess.SubprocessError):
            pass
        time.sleep(3)
    return None


def _wait_https_up(ip: str, *, timeout: float = 150.0) -> bool:
    """Poll https://ip/ until the TLS handshake succeeds.

    A factory reset wipes the device cert; the board re-enrolls on its first STA
    boot and serves plain HTTP (:80) until the new cert lands, then HTTPS (:443).
    Waiting here keeps the @sta_mode HTTPS suite from racing the re-enrollment.
    Returns True once HTTPS answers; False on timeout (the STA tests then surface
    the enrollment failure themselves)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = httpx.get(f"https://{ip}/", verify=False, timeout=8.0, follow_redirects=False)
            if r.status_code in (200, 301, 401):
                return True
        except httpx.HTTPError:
            pass
        time.sleep(4)
    return False


@pytest.fixture(scope="session", autouse=True)
def reset_board(bench_id, mqtt_creds):
    """Lifecycle phase 0 — start from a RESET, unprovisioned board.

    If the board is online (STA), publish cmd/factory_reset and wait for it to
    drop offline + reboot into the AP portal. If already offline (AP /
    unprovisioned), it's taken as already reset. Set CB_HIL_NO_RESET=1 to skip
    (ad-hoc runs against a board you don't want to wipe)."""
    if os.environ.get("CB_HIL_NO_RESET"):
        return
    if _wait_availability(bench_id, mqtt_creds, b"online", timeout=3.0):
        t0 = time.time()
        cli = _mqtt_client(f"hil-reset-{os.getpid()}", mqtt_creds)
        if cli is not None:
            cli.loop_start()
            cli.publish(f"{bench_id}/cmd/factory_reset", "1", qos=1)
            time.sleep(1.0)
            cli.loop_stop()
            cli.disconnect()
        # graceful LWT offline, then reboot + SoftAP bring-up.
        _wait_availability(bench_id, mqtt_creds, b"offline", timeout=30.0, since=t0)
        time.sleep(15)


@pytest.fixture(scope="session")
def provisioned_sta(bench_mac, bench_id, mqtt_creds, bench_port) -> str | None:
    """Lifecycle phase 2 — provision the reset board onto the station LAN.

    Joins the bench AP, POSTs the station creds (CB_PROVISION_SSID +
    CB_PROVISION_PSK) on /wifi, waits for the board to reboot into STA and come
    back MQTT-online, then discovers its DHCP IP by MAC. Returns the IP, or None
    when provisioning isn't configured (CB_BENCH_IP set, or creds absent) so
    bench_ip falls back rather than skipping the whole STA phase."""
    if os.environ.get("CB_BENCH_IP"):
        return os.environ["CB_BENCH_IP"]
    ssid = os.environ.get("CB_PROVISION_SSID")
    psk = os.environ.get("CB_PROVISION_PSK")
    if not ssid or not psk:
        # Say so loudly. Without these the board is never provisioned, so it
        # sits in AP mode while bench_ip quietly falls back to the allowlist
        # address — and every HTTP test then fails with "no route to host",
        # which looks like a broken board instead of a missing variable. They
        # live in the operator's ~/.bashrc, so a non-interactive shell (a
        # script, an agent) will not have them.
        print(
            "[hil] CB_PROVISION_SSID/CB_PROVISION_PSK not set — skipping "
            "provisioning; the board stays in AP mode and the STA-phase "
            "tests will not be meaningful",
            file=sys.stderr,
        )
        return None
    ap_ssid = os.environ.get("CB_AP_SSID", bench_id)
    ap_pass = _resolve_ap_pass(bench_port)
    prev = _station_con()
    if not _join_ap(ap_ssid, ap_pass):
        _reconnect(prev)
        pytest.skip("provisioned_sta: could not join bench AP (board not in AP mode?)")
    try:
        time.sleep(3)
        c = httpx.Client(base_url=AP_PORTAL_URL, timeout=12.0)
        try:
            c.post("/wifi", data={"ssid": ssid, "password": psk})
        except httpx.HTTPError:
            pass  # connection reset == board rebooted after staging the candidate
        finally:
            c.close()
    finally:
        _reconnect(prev)  # host back on the station LAN to observe the board
    if not _wait_availability(bench_id, mqtt_creds, b"online", timeout=180.0):
        pytest.skip(
            f"provisioned_sta: {bench_id} never reported MQTT-online within "
            "180 s of provisioning. Check the stderr line above: if it says "
            "the broker was never reached, the runner's own connectivity is "
            "the problem (it has just hopped back off the bench AP), not the "
            "board — otherwise suspect the station creds or a boot crash."
        )
    # Pin the bench to the build under test: ota_enabled defaults ON (and an
    # erase-flash resets NVS to that default), and the firmware's first OTA poll
    # fires ~120 s after boot — so on a slow provision a fire-and-forget OFF
    # loses the race and the bench self-downgrades to whatever stale image sits
    # on ota.example.com, then the whole connected suite runs against the WRONG build
    # (observed: bench pulled an old field image → /debug/* 404, OTA churn broke
    # the reboot-recovery tests). So VERIFY the OFF landed (retry until the
    # state/cfg echo confirms), well within the 120 s window. test_ota re-enables
    # it itself.
    if not _set_cfg_verified(bench_id, mqtt_creds, "ota_enabled", "OFF", tries=6, per_try=8.0):
        pytest.skip(
            "provisioned_sta: could not confirm ota_enabled=OFF — the "
            "bench may self-OTA to a stale server image mid-run"
        )
    ip = _discover_ip_by_mac(bench_mac)
    if not ip:
        pytest.skip(
            "provisioned_sta: STA online but IP not discoverable — set "
            "CB_BENCH_IP (the ARP sweep needs nmap on the host)"
        )
    # The board re-enrolls TLS on first STA boot after the reset; wait for HTTPS
    # so the connected suite runs against a fully-up board, not mid-enrollment.
    _wait_https_up(ip)
    return ip


# ── Phase ordering: AP suite → provision → STA suite ──────────────────────
def pytest_collection_modifyitems(config, items):
    """Order the lifecycle: @ap_mode first (board fresh in AP), the AP→STA
    provisioning test last in that phase, then @sta_mode + everything else
    (board on STA). Stable sort preserves collection order within a phase."""

    def phase(item) -> int:
        if item.get_closest_marker("ap_mode"):
            return 1 if "provision" in item.nodeid else 0
        return 2

    items.sort(key=phase)
