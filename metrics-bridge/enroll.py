"""enroll.py — Chytrá Budka MQTT enrollment signer.

Companion daemon to cbprom.py. Subscribes to `+/cmd/enroll` on the
broker; every CSR posted by a budka firmware boot path comes through.
Each is validated (CN matches the topic's mactail, SAN entries are
inside the sub-CA's name constraints) then signed against the issuing
sub-CA private key. The signed leaf cert PEM goes back on
`<base>/state/cert` (NOT retained — single-shot per enrollment).

See `firmware/HTTPS.md` "Enrollment protocol" for the on-wire contract.

Why this is a separate file from cbprom.py:
  - Different responsibility (writes signed credentials vs. read-only
    metric scrape). Different blast radius — a bug here forges TLS
    state, a bug there drops a Prom sample.
  - Different secret access pattern: cbprom reads MQTT creds; enroll
    additionally reads the sub-CA private key. Both can be owned by
    the cbprom group; the key file is the only one that needs ssl-cert
    membership.
  - Lets the operator restart the metrics scraper without nudging
    pending enrollments, and vice versa.

Both daemons share the same config.toml + the same Python venv. Two
systemd units pull the relevant section.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import logging
import re
import signal
import sys
import threading
import time
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib  # type: ignore[no-redef]

import paho.mqtt.client as mqtt

# Metrics are best-effort — the signer must keep working even if
# prometheus_client isn't installed (a CA outage is invisible without them,
# which is exactly the gap this adds; but signing must never depend on it).
try:
    from prometheus_client import Counter, Gauge, start_http_server

    _HAVE_PROM = True
except ModuleNotFoundError:
    _HAVE_PROM = False
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

log = logging.getLogger("cbd-enroll")

# ─── Topic + identity parsing ───────────────────────────────────────────────

# `<device_id>/cmd/enroll` → base = the full device_id (the trust anchor).
# device_id = "cb-<6hex>" (firmware device_id.c), e.g. cb-ex01. The mactail is
# pinned to an EXACT 6-hex shape so a malformed/arbitrary-length identity can't
# be accepted to sign for — that narrows the identity space an authorised-broker
# client could request. The captured `base` is the literal CN/SAN trust anchor
# used in validate_csr (no reconstruction).
TOPIC_RE = re.compile(r"^(?P<base>cb-[a-f0-9]{6})/cmd/enroll$")


# ─── Config ─────────────────────────────────────────────────────────────────


@dataclass
class CAConfig:
    cert_path: Path  # /etc/ansible-ca/sub_ca_budka.pem
    key_path: Path  # /etc/ansible-ca/sub_ca_budka.key
    validity_days: int  # default 90; renewal cadence
    permitted_suffixes: tuple[str, ...]  # ".lan", ".lan", ".local"


@dataclass
class Config:
    mqtt_host: str
    mqtt_port: int
    mqtt_user: str
    mqtt_pass: str
    ca: CAConfig
    metrics_port: int = 9879  # cbprom uses 9878; keep them distinct
    ledger_path: Path | None = None  # durable issuance audit log (append-only)


def _read_secret(section: dict[str, Any], key: str, default: str) -> str:
    """Same pattern as cbprom.py — prefer `<key>_path` over inline."""
    path = section.get(f"{key}_path")
    if path:
        return Path(path).read_text().strip()
    return str(section.get(key, default))


def load_config(path: Path) -> Config:
    with path.open("rb") as f:
        raw = tomllib.load(f)
    m = raw.get("mqtt", {})
    ca = raw.get("ca", {})
    en = raw.get("enroll", {})
    if not ca:
        raise SystemExit(
            f"{path}: missing [ca] section — see metrics-bridge/templates/"
            "config.toml.j2 for the required keys"
        )
    host = m.get("host")
    if not host:
        raise SystemExit(f"{path}: [mqtt] host is required (no implicit default)")
    return Config(
        mqtt_host=host,
        mqtt_port=int(m.get("port", 1883)),
        mqtt_user=_read_secret(m, "user", default=""),
        mqtt_pass=_read_secret(m, "password", default=""),
        metrics_port=int(en.get("metrics_port", 9879)),
        ledger_path=Path(en["ledger_path"]) if en.get("ledger_path") else None,
        ca=CAConfig(
            cert_path=Path(ca["cert_path"]),
            key_path=Path(ca["key_path"]),
            validity_days=int(ca.get("validity_days", 90)),
            permitted_suffixes=tuple(ca.get("permitted_suffixes", [".lan", ".lan", ".local"])),
        ),
    )


# ─── CSR validation ─────────────────────────────────────────────────────────


class CSRRejected(Exception):
    """Raised when a CSR can't be safely signed — abort and log."""


def _extract_cn(csr: x509.CertificateSigningRequest) -> str:
    """Pull the (single) CN attribute from the CSR's Subject. Firmware
    always emits CN=<device_id>[.<domain>]; anything else is a protocol
    breach and gets rejected loudly so a debugging operator sees it."""
    cns = csr.subject.get_attributes_for_oid(NameOID.COMMON_NAME)
    if len(cns) != 1:
        raise CSRRejected(f"expected exactly 1 CN, got {len(cns)}")
    return str(cns[0].value)


def _san_names(csr: x509.CertificateSigningRequest) -> list[x509.GeneralName]:
    """Return the subjectAltName entries. Missing extension is OK (the
    pre-name-constraints budka firmware used CN-only); we treat that as
    an empty list and validate CN only."""
    try:
        ext = csr.extensions.get_extension_for_class(x509.SubjectAlternativeName)
    except x509.ExtensionNotFound:
        return []
    return list(ext.value)


def _check_name_inside_constraints(name: str, suffixes: tuple[str, ...]) -> None:
    """Mirror the sub-CA's nameConstraints permitted set. The CA itself
    will reject a name outside .lan/.lan/.local at sign-time too, but
    catching it here gives a clean MQTT error response instead of a
    cryptography library exception. Suffix match is case-insensitive
    per DNS norms."""
    lower = name.lower()
    if any(lower.endswith(s.lower()) for s in suffixes):
        return
    raise CSRRejected(f"name {name!r} outside permitted constraints {suffixes!r}")


def validate_csr(csr: x509.CertificateSigningRequest, expected_base: str, ca: CAConfig) -> None:
    """Reject CSRs whose Subject + SAN don't match what this device is
    allowed to ask for. `expected_base` is the device_id parsed from the
    TOPIC, NOT from the CSR — that's the trust anchor that prevents budka A
    from getting a cert for budka B's identity by lying in its own CSR.

    Allowed names: CN and every SAN entry must either:
      - equal `<expected_base>` (bare hostname, e.g. "cb-ex01"), OR
      - end with one of the permitted suffixes (.lan / .lan /
        .local) AND start with `<expected_base>.`
      - be an IP address (we don't constrain which IP — the device has
        whatever DHCP gave it; cert is for inside-LAN consumption)
    """
    bare = expected_base

    # ── CN
    cn = _extract_cn(csr)
    if cn != bare and not cn.startswith(bare + "."):
        raise CSRRejected(f"CN {cn!r} doesn't match expected {bare!r}")
    if cn != bare:
        _check_name_inside_constraints(cn, ca.permitted_suffixes)

    # ── SAN entries
    for n in _san_names(csr):
        if isinstance(n, x509.DNSName):
            v = n.value
            if v == bare:
                continue
            if v.startswith(bare + "."):
                _check_name_inside_constraints(v, ca.permitted_suffixes)
                continue
            raise CSRRejected(f"SAN DNS {v!r} doesn't carry expected prefix {bare!r}")
        elif isinstance(n, x509.IPAddress):
            # Restrict SAN IPs to RFC 1918 (private) ranges. A
            # compromised budka claiming a SAN IP for a routable host
            # (e.g. the broker, 192.0.2.5 on a flat /16 — but more
            # importantly any 8.8.8.8-class public IP) would be a
            # forged-identity vector if any client trusted the leaf
            # via IP-based hostname verification. RFC 1918 limits the
            # surface to LAN where the trust model already lives.
            # ipaddress library understands both v4 and v6 private
            # ranges via .is_private.
            ip = n.value
            if not isinstance(ip, (ipaddress.IPv4Address, ipaddress.IPv6Address)):
                raise CSRRejected(f"SAN IP type {type(ip).__name__!r} not an address")
            if not ip.is_private:
                raise CSRRejected(
                    f"SAN IP {ip} is not in a private subnet "
                    "(RFC 1918 / fc00::/7) — refusing to sign"
                )
        else:
            raise CSRRejected(f"SAN type {type(n).__name__} not permitted ({n!r})")

    # ── Pubkey curve
    pk = csr.public_key()
    if not isinstance(pk, ec.EllipticCurvePublicKey):
        raise CSRRejected(f"non-EC pubkey ({type(pk).__name__})")
    if not isinstance(pk.curve, ec.SECP256R1):
        raise CSRRejected(f"unexpected curve {pk.curve.name!r}; want SECP256R1")


# ─── Signing ────────────────────────────────────────────────────────────────


@dataclass
class CABundle:
    cert: x509.Certificate
    key: ec.EllipticCurvePrivateKey


def load_ca(ca: CAConfig) -> CABundle:
    cert = x509.load_pem_x509_certificate(ca.cert_path.read_bytes())
    key_data = ca.key_path.read_bytes()
    key = serialization.load_pem_private_key(key_data, password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey):
        raise SystemExit(f"{ca.key_path}: expected EC P-256 private key, got {type(key).__name__}")
    return CABundle(cert=cert, key=key)


def sign_csr(
    csr: x509.CertificateSigningRequest, ca_bundle: CABundle, ca_cfg: CAConfig
) -> x509.Certificate:
    """Build a leaf cert from the CSR's Subject + SAN, sign with the
    sub-CA. Validity starts 1 minute in the past to tolerate small
    clock skew between signer + device (NTP not yet synced on a
    just-booted budka)."""
    now = datetime.now(UTC)
    serial = int.from_bytes(
        hashlib.sha256(csr.public_bytes(serialization.Encoding.DER)).digest()[:16], "big"
    )
    # Force highest bit off so the cert serial fits in a positive
    # signed-int per RFC 5280 §4.1.2.2 ("MUST be a positive integer").
    serial &= ~(1 << 127)

    builder = (
        x509.CertificateBuilder()
        .subject_name(csr.subject)
        .issuer_name(ca_bundle.cert.subject)
        .public_key(csr.public_key())
        .serial_number(serial)
        .not_valid_before(now - timedelta(minutes=1))
        .not_valid_after(now + timedelta(days=ca_cfg.validity_days))
    )

    # Carry the CSR's SAN through to the cert — but drop bare-hostname
    # DNSName entries (no dot). The sub-CA's nameConstraints permits
    # only ".lan" / ".lan" / ".local" subtrees; a bare label like
    # "cb-ex01" doesn't end in any of those and the sign
    # call would fail with "permitted subtree violation". The bare
    # hostname adds no practical value over the .local form (mDNS
    # resolves it identically) and the device firmware accepts being
    # rebuffed on this entry — see tls_enroll.c which sends a
    # superset and lets the signer decide.
    sans_out: list[x509.GeneralName] = []
    for n in _san_names(csr):
        if isinstance(n, x509.DNSName) and not any(
            n.value.lower().endswith(s.lower()) for s in ca_cfg.permitted_suffixes
        ):
            continue
        sans_out.append(n)
    if sans_out:
        builder = builder.add_extension(
            x509.SubjectAlternativeName(sans_out),
            critical=False,
        )

    # serverAuth EKU only — matches the sub-CA's own EKU constraint.
    builder = builder.add_extension(
        x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),
        critical=False,
    )
    builder = builder.add_extension(
        x509.KeyUsage(
            digital_signature=True,
            content_commitment=False,
            key_encipherment=False,
            data_encipherment=False,
            key_agreement=False,
            key_cert_sign=False,
            crl_sign=False,
            encipher_only=False,
            decipher_only=False,
        ),
        critical=True,
    )
    builder = builder.add_extension(
        x509.BasicConstraints(ca=False, path_length=None),
        critical=True,
    )

    # Subject Key Identifier on the leaf + Authority Key Identifier
    # pointing back to the issuing sub-CA's SKI. RFC 5280 §4.2.1.1
    # says AKI MUST appear on CA-issued certs to facilitate path
    # construction, and modern Python's ssl module enforces this
    # (raises "Missing Authority Key Identifier" in TLS verify
    # without it — bit users on HA 2024.12+ / Python 3.12). Both
    # are non-critical per RFC.
    builder = builder.add_extension(
        x509.SubjectKeyIdentifier.from_public_key(csr.public_key()),
        critical=False,
    )
    # AKI: copy the issuer's existing SKI bytes verbatim so the leaf's
    # AKI keyIdentifier matches what's actually in the sub-CA's SKI
    # extension. from_issuer_subject_key_identifier() is the RFC 5280-
    # blessed path (vs. from_issuer_public_key which recomputes the
    # digest and can disagree with a sub-CA whose SKI was generated by
    # a non-SHA1 method). Our sub-CA does have an SKI — verified.
    sub_ca_ski = ca_bundle.cert.extensions.get_extension_for_class(x509.SubjectKeyIdentifier)
    builder = builder.add_extension(
        x509.AuthorityKeyIdentifier.from_issuer_subject_key_identifier(sub_ca_ski.value),
        critical=False,
    )

    return builder.sign(private_key=ca_bundle.key, algorithm=hashes.SHA256())


# ─── Daemon ─────────────────────────────────────────────────────────────────


class Signer:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self.ca = load_ca(cfg.ca)
        # Single-thread the sign path. CSR sign is CPU-bound for ~10 ms
        # on a Pi-class CPU; serializing means a flood of enrollment
        # requests can't exhaust connections by stalling broker reads.
        self._lock = threading.Lock()
        self._client: mqtt.Client | None = None  # set in run(); for clean SIGTERM
        log.info(
            "sub-CA loaded: %s (valid until %s)",
            cfg.ca.cert_path,
            self.ca.cert.not_valid_after_utc.isoformat(),
        )

        # Observability: outcome counter + sub-CA expiry + last-success gauges.
        # Without these an enrollment outage (sub-CA lapsed, signer crashing on
        # every CSR) is invisible until someone tails journald. Best-effort.
        if _HAVE_PROM:
            self.m_requests = Counter(
                "cbd_enroll_requests_total",
                "Enrollment requests by outcome",
                ["result"],  # signed | rejected | csr_invalid | error
            )
            self.m_last_success = Gauge(
                "cbd_enroll_last_success_timestamp_seconds",
                "Unix time of the last successfully signed certificate",
            )
            m_ca = Gauge(
                "cbd_enroll_sub_ca_not_after_timestamp_seconds",
                "Sub-CA certificate expiry (unix time) — alert before it lapses",
            )
            m_ca.set(self.ca.cert.not_valid_after_utc.timestamp())
        else:
            self.m_requests = None
            self.m_last_success = None

    def _count(self, result: str) -> None:
        if self.m_requests is not None:
            self.m_requests.labels(result=result).inc()

    def _ledger(self, cn: str, cert: x509.Certificate) -> None:
        """Append a durable issuance record (serial, CN, expiry). A CA with no
        issuance ledger can't do inventory/revocation; journald lines are not a
        record. Best-effort — a ledger write failure must not block signing."""
        if not self.cfg.ledger_path:
            return
        try:
            self.cfg.ledger_path.parent.mkdir(parents=True, exist_ok=True)
            with self.cfg.ledger_path.open("a") as f:
                f.write(
                    "{} serial={:x} cn={} not_after={}\n".format(
                        datetime.now(UTC).isoformat(timespec="seconds"),
                        cert.serial_number,
                        cn,
                        cert.not_valid_after_utc.isoformat(),
                    )
                )
        except OSError as e:
            log.warning("issuance ledger write failed (%s): %s", self.cfg.ledger_path, e)

    def handle_enroll(self, client: mqtt.Client, topic: str, payload: bytes) -> None:
        m = TOPIC_RE.match(topic)
        if not m:
            return
        base = m.group("base")  # full device_id, e.g. "cb-ex01" — the anchor

        resp_topic = f"{base}/state/cert"

        try:
            csr = x509.load_pem_x509_csr(payload)
        except ValueError as e:
            log.warning("[%s] CSR parse failed: %s", base, e)
            self._count("csr_invalid")
            return
        if not csr.is_signature_valid:
            log.warning("[%s] CSR signature invalid — rejecting", base)
            self._count("csr_invalid")
            return

        try:
            with self._lock:
                validate_csr(csr, base, self.cfg.ca)
                cert = sign_csr(csr, self.ca, self.cfg.ca)
        except CSRRejected as e:
            log.warning("[%s] CSR rejected: %s", base, e)
            self._count("rejected")
            return
        except Exception:
            log.exception("[%s] sign path crashed", base)
            self._count("error")
            return

        pem = cert.public_bytes(serialization.Encoding.PEM)
        # QoS 1, NOT retained — every enrollment generates a fresh cert
        # bound to the requesting CSR's pubkey; a retained payload
        # would let a passive observer pick up the previous device's
        # cert and confuse the next boot. Single-shot semantics.
        client.publish(resp_topic, pem, qos=1, retain=False)
        self._count("signed")
        if self.m_last_success is not None:
            self.m_last_success.set(time.time())
        self._ledger(base, cert)
        log.info(
            "[%s] signed cert (serial=%x, %d B, validity %d d) → %s",
            base,
            cert.serial_number,
            len(pem),
            self.cfg.ca.validity_days,
            resp_topic,
        )

    # ── MQTT callbacks
    def on_connect(self, client, userdata, flags, reason_code, properties=None):
        if reason_code != 0:
            log.error("mqtt connect failed: %s", reason_code)
            return
        log.info("mqtt connected to %s:%d", self.cfg.mqtt_host, self.cfg.mqtt_port)
        # Wildcard per-device — every budka on this broker hits us.
        # Same `+` placement note as cbprom.py: `+` is a whole level,
        # `cb-+` is invalid syntax.
        client.subscribe("+/cmd/enroll", qos=1)

    def on_message(self, client, userdata, msg):
        try:
            self.handle_enroll(client, msg.topic, msg.payload)
        except Exception:
            log.exception("handler crashed for %s", msg.topic)

    def on_disconnect(self, client, userdata, flags, reason_code, properties=None):
        log.warning("mqtt disconnect: %s", reason_code)

    def run(self) -> None:
        # paho-mqtt 2.x requires `callback_api_version=CallbackAPIVersion.VERSION2`
        # to opt into the new on_connect/on_disconnect signatures (the
        # ones with `properties` + `reason_code`). Pyright's bundled
        # paho stubs lag behind the 2.x API; the runtime accepts the
        # kwarg fine. cbprom.py uses the same dance.
        client = mqtt.Client(  # type: ignore[call-arg]
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,  # type: ignore[attr-defined]
            client_id="cbd-enroll",
            reconnect_on_failure=True,
        )
        if self.cfg.mqtt_user:
            client.username_pw_set(self.cfg.mqtt_user, self.cfg.mqtt_pass)
        client.on_connect = self.on_connect
        client.on_message = self.on_message
        client.on_disconnect = self.on_disconnect
        if _HAVE_PROM:
            start_http_server(self.cfg.metrics_port)
            log.info("metrics on :%d/metrics", self.cfg.metrics_port)
        # connect_async (not connect): a broker-down-at-startup must not
        # raise out of run() before loop_forever()'s reconnect engages, or
        # systemd just crash-loops the signer every RestartSec. loop_forever
        # drives the deferred connect with backoff. See cbprom.py.
        self._client = client
        client.connect_async(self.cfg.mqtt_host, self.cfg.mqtt_port, keepalive=30)
        client.loop_forever()

    def stop(self) -> None:
        """Break loop_forever() cleanly on SIGTERM (explicit disconnect, so the
        MQTT session closes rather than being severed by interpreter exit)."""
        if self._client is not None:
            self._client.disconnect()


# ─── Entrypoint ─────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    desc = (__doc__ or "Chytrá Budka MQTT enrollment signer").split("\n", 1)[0]
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
    signer = Signer(cfg)

    def _sigterm(_signum, _frame):
        log.info("SIGTERM — disconnecting")
        signer.stop()

    signal.signal(signal.SIGTERM, _sigterm)

    try:
        signer.run()
    except KeyboardInterrupt:
        log.info("KeyboardInterrupt — exiting")
    return 0


if __name__ == "__main__":
    sys.exit(main())
