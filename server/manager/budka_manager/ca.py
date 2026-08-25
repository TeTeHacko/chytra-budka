"""CSR validation + leaf signing for the budka sub-CA.

Ported from metrics-bridge/enroll.py (the MQTT enrollment signer) — the
validate/sign core is kept semantically identical so both signers issue
interchangeable certificates during the migration window. Two deliberate
contract changes for the standalone stack:

  * EKU = serverAuth + clientAuth. The leaf doubles as the device's MQTT
    client identity (mosquitto `require_certificate` + `use_identity_as_
    username`), while still serving the device's own HTTPS server.
  * The expected identity comes from the CSR CN itself (bare device id or
    id.<suffix>), not from an MQTT topic — over HTTPS the authorization
    anchor is the TOFU approval + key continuity in enrollment.py, so this
    module only enforces internal consistency (every SAN must match the CN's
    device id) plus the name/curve constraints.
"""

from __future__ import annotations

import hashlib
import ipaddress
import re
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

# device_id = "cb-<6hex>" (firmware device_id.c). Exact shape, same anchor
# discipline as the MQTT signer's TOPIC_RE.
DEVICE_ID_RE = re.compile(r"^cb-[a-f0-9]{6}$")


class CSRRejected(Exception):
    """Raised when a CSR can't be safely signed — abort and log."""


@dataclass
class CAConfig:
    cert_path: Path
    key_path: Path
    validity_days: int
    permitted_suffixes: tuple[str, ...]


@dataclass
class CABundle:
    cert: x509.Certificate
    key: ec.EllipticCurvePrivateKey

    @property
    def cert_pem(self) -> bytes:
        return self.cert.public_bytes(serialization.Encoding.PEM)


def load_ca(ca: CAConfig) -> CABundle:
    cert = x509.load_pem_x509_certificate(ca.cert_path.read_bytes())
    key = serialization.load_pem_private_key(ca.key_path.read_bytes(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey):
        raise RuntimeError(
            f"{ca.key_path}: expected EC P-256 private key, got {type(key).__name__}"
        )
    return CABundle(cert=cert, key=key)


def extract_cn(csr: x509.CertificateSigningRequest) -> str:
    cns = csr.subject.get_attributes_for_oid(NameOID.COMMON_NAME)
    if len(cns) != 1:
        raise CSRRejected(f"expected exactly 1 CN, got {len(cns)}")
    return str(cns[0].value)


def device_id_from_cn(cn: str) -> str:
    """CN is either the bare device id or `<id>.<domain>`; return the id."""
    base = cn.split(".", 1)[0]
    if not DEVICE_ID_RE.match(base):
        raise CSRRejected(f"CN {cn!r} does not carry a cb-<6hex> device id")
    return base


def pubkey_fingerprint(csr: x509.CertificateSigningRequest) -> str:
    """SHA-256 of the SubjectPublicKeyInfo DER — the key-continuity anchor."""
    spki = csr.public_key().public_bytes(
        serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    return hashlib.sha256(spki).hexdigest()


def _san_names(csr: x509.CertificateSigningRequest) -> list[x509.GeneralName]:
    try:
        ext = csr.extensions.get_extension_for_class(x509.SubjectAlternativeName)
    except x509.ExtensionNotFound:
        return []
    return list(ext.value)


def _check_name_inside_constraints(name: str, suffixes: tuple[str, ...]) -> None:
    lower = name.lower()
    if any(lower.endswith(s.lower()) for s in suffixes):
        return
    raise CSRRejected(f"name {name!r} outside permitted constraints {suffixes!r}")


def validate_csr(csr: x509.CertificateSigningRequest, expected_base: str, ca: CAConfig) -> None:
    """Reject CSRs whose Subject + SAN don't match what this device is allowed
    to ask for. Semantics identical to metrics-bridge/enroll.py::validate_csr;
    `expected_base` is derived by the caller (topic there, CN + TOFU here).
    """
    bare = expected_base

    cn = extract_cn(csr)
    if cn != bare and not cn.startswith(bare + "."):
        raise CSRRejected(f"CN {cn!r} doesn't match expected {bare!r}")
    if cn != bare:
        _check_name_inside_constraints(cn, ca.permitted_suffixes)

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
            # RFC 1918 / fc00::/7 only — a leaf claiming a routable IP would be
            # a forged-identity vector for IP-based hostname verification.
            ip = n.value
            if not isinstance(ip, (ipaddress.IPv4Address, ipaddress.IPv6Address)):
                raise CSRRejected(f"SAN IP type {type(ip).__name__!r} not an address")
            if not ip.is_private:
                raise CSRRejected(f"SAN IP {ip} is not in a private subnet — refusing to sign")
        else:
            raise CSRRejected(f"SAN type {type(n).__name__} not permitted ({n!r})")

    pk = csr.public_key()
    if not isinstance(pk, ec.EllipticCurvePublicKey):
        raise CSRRejected(f"non-EC pubkey ({type(pk).__name__})")
    if not isinstance(pk.curve, ec.SECP256R1):
        raise CSRRejected(f"unexpected curve {pk.curve.name!r}; want SECP256R1")


def sign_csr(
    csr: x509.CertificateSigningRequest,
    ca_bundle: CABundle,
    ca_cfg: CAConfig,
    subject_cn: str | None = None,
) -> x509.Certificate:
    """Build a leaf cert from the CSR's Subject + SAN, sign with the sub-CA.
    Validity starts 1 minute in the past to tolerate clock skew on a
    just-booted device (NTP not yet synced).

    `subject_cn` overrides the CSR's Subject CN — the HTTPS enrollment passes
    the bare device id here because mosquitto's `use_identity_as_username`
    turns the CN into the ACL identity, and the pattern ACLs confine `%u/#`;
    a CN like `cb-xxxxxx.lan` would silently strand the device outside its
    own topic prefix. The CSR's full name set survives in the SANs.
    """
    now = datetime.now(UTC)
    serial = int.from_bytes(
        hashlib.sha256(csr.public_bytes(serialization.Encoding.DER)).digest()[:16], "big"
    )
    # RFC 5280 §4.1.2.2: serial MUST be a positive integer.
    serial &= ~(1 << 127)

    subject = csr.subject
    if subject_cn is not None:
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, subject_cn)])

    builder = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(ca_bundle.cert.subject)
        .public_key(csr.public_key())
        .serial_number(serial)
        .not_valid_before(now - timedelta(minutes=1))
        .not_valid_after(now + timedelta(days=ca_cfg.validity_days))
    )

    # Carry the CSR's SAN through, dropping bare-hostname DNS entries: the
    # sub-CA's nameConstraints permits only the configured subtrees and a bare
    # label like "cb-ex01" would fail the sign call. The firmware sends a
    # superset and accepts being rebuffed on this entry (tls_enroll.c).
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

    # serverAuth (device's own HTTPS server) + clientAuth (MQTT mTLS identity).
    builder = builder.add_extension(
        x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH, ExtendedKeyUsageOID.CLIENT_AUTH]),
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

    # SKI on the leaf + AKI copying the issuer's SKI bytes verbatim — modern
    # Python ssl (HA 2024.12+) rejects leaves without AKI.
    builder = builder.add_extension(
        x509.SubjectKeyIdentifier.from_public_key(csr.public_key()),
        critical=False,
    )
    sub_ca_ski = ca_bundle.cert.extensions.get_extension_for_class(x509.SubjectKeyIdentifier)
    builder = builder.add_extension(
        x509.AuthorityKeyIdentifier.from_issuer_subject_key_identifier(sub_ca_ski.value),
        critical=False,
    )

    return builder.sign(private_key=ca_bundle.key, algorithm=hashes.SHA256())
