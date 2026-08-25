"""HTTPS enrollment service: TOFU approval queue + key-continuity auto-issue.

Authorization model (replaces the MQTT signer's topic/ACL trust anchor):

  * unknown device id            -> record request, 202 pending (operator
                                    approves in UI/CLI — trust on first use)
  * approved + same pubkey       -> auto-issue (renewals, reboots)
  * approved + no pubkey on file -> first issue after approval, pin the key
  * approved + different pubkey  -> re-key (factory reset wipes the NVS key):
                                    back to pending, operator re-approves —
                                    unless the id is in CB_ENROLL_TRUSTED_DEVICES
                                    (bench boards, which re-key every HIL run)
  * denied                       -> 403

The CSR's CN carries the claimed identity; validate_csr() enforces internal
consistency (SANs, curve, name constraints) exactly like the MQTT signer.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import timedelta

from cryptography import x509
from cryptography.hazmat.primitives import serialization
from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import AsyncSession

from . import ca as ca_mod
from .ca import CABundle, CAConfig, CSRRejected
from .models import Device, EnrollLedger, EnrollRequest, utcnow

log = logging.getLogger("budka.enroll")


@dataclass
class EnrollOutcome:
    status: str  # issued | pending | denied | rejected | rate_limited
    detail: str = ""
    cert_pem: bytes | None = None


class EnrollmentService:
    def __init__(
        self,
        ca_cfg: CAConfig,
        ca_bundle: CABundle,
        max_per_hour: int = 3,
        trusted_devices: tuple[str, ...] = (),
    ) -> None:
        self.ca_cfg = ca_cfg
        self.ca = ca_bundle
        self.max_per_hour = max_per_hour
        self.trusted_devices = trusted_devices

    async def handle_csr(self, session: AsyncSession, payload: bytes) -> EnrollOutcome:
        try:
            csr = x509.load_pem_x509_csr(payload)
        except ValueError as e:
            return EnrollOutcome("rejected", f"CSR parse failed: {e}")
        if not csr.is_signature_valid:
            return EnrollOutcome("rejected", "CSR self-signature invalid")

        try:
            cn = ca_mod.extract_cn(csr)
            device_id = ca_mod.device_id_from_cn(cn)
            ca_mod.validate_csr(csr, device_id, self.ca_cfg)
        except CSRRejected as e:
            return EnrollOutcome("rejected", str(e))

        fp = ca_mod.pubkey_fingerprint(csr)

        device = await session.get(Device, device_id)
        if device is None:
            device = Device(device_id=device_id, approved=False, pubkey_fp=None)
            session.add(device)
            await self._upsert_pending(session, device_id, cn, fp, payload, reason="new-device")
            await session.commit()
            log.info("[%s] new device — enrollment pending operator approval", device_id)
            return EnrollOutcome("pending", "new device, awaiting approval")

        device.last_seen = utcnow()

        if not device.approved:
            latest = await self._latest_request(session, device_id)
            if latest is not None and latest.status == "denied" and latest.pubkey_fp == fp:
                await session.commit()
                return EnrollOutcome("denied", "enrollment denied by operator")
            await self._upsert_pending(session, device_id, cn, fp, payload, reason="new-device")
            await session.commit()
            return EnrollOutcome("pending", "awaiting approval")

        if device.pubkey_fp is not None and device.pubkey_fp != fp:
            # Re-keyed device (factory reset wipes the NVS key).
            if device_id in self.trusted_devices:
                # Bench boards re-key on every HIL run — the suite factory-resets
                # them — and they sit on the desk, so a manual re-approval each
                # time is friction without a security gain. Only ever the IDs an
                # operator listed explicitly in CB_ENROLL_TRUSTED_DEVICES.
                device.pubkey_fp = fp
                log.warning("[%s] pubkey changed — auto-re-approved (trusted device)", device_id)
            else:
                await self._upsert_pending(session, device_id, cn, fp, payload, reason="rekey")
                await session.commit()
                log.warning("[%s] pubkey changed — re-approval required", device_id)
                return EnrollOutcome("pending", "key changed, awaiting re-approval")

        # Trusted devices skip the rate limit as well as the re-approval. The
        # limit is there to stop a misbehaving board hammering the CA; a bench
        # legitimately re-keys several times an hour because the HIL suite
        # factory-resets it, and hitting the cap leaves it unable to reach the
        # broker at all for the rest of the hour.
        if device_id not in self.trusted_devices and not await self._rate_ok(session, device_id):
            await session.commit()
            log.warning("[%s] issuance rate limit hit", device_id)
            return EnrollOutcome(
                "rate_limited", f"more than {self.max_per_hour} issues in the last hour"
            )

        cert = ca_mod.sign_csr(csr, self.ca, self.ca_cfg, subject_cn=device_id)
        if device.pubkey_fp is None:
            device.pubkey_fp = fp
        session.add(
            EnrollLedger(
                device_id=device_id,
                cn=cn,
                serial_hex=format(cert.serial_number, "x"),
                not_after=cert.not_valid_after_utc.replace(tzinfo=None),
            )
        )
        await self._supersede_pending(session, device_id)
        await session.commit()

        pem = cert.public_bytes(serialization.Encoding.PEM)
        log.info(
            "[%s] signed cert (serial=%x, validity %d d)",
            device_id,
            cert.serial_number,
            self.ca_cfg.validity_days,
        )
        # Leaf first, then the issuing sub-CA — the firmware validates the
        # first certificate against its embedded sub-CA and may use the rest
        # for chain building.
        return EnrollOutcome("issued", cert_pem=pem + self.ca.cert_pem)

    # --- approval workflow (CLI now, UI in a later phase) ---

    async def approve(self, session: AsyncSession, device_id: str, actor: str = "cli") -> bool:
        device = await session.get(Device, device_id)
        latest = await self._latest_request(session, device_id)
        if device is None or latest is None or latest.status != "pending":
            return False
        latest.status = "approved"
        latest.decided_at = utcnow()
        latest.decided_by = actor
        device.approved = True
        # Pin (or re-pin after rekey) the key that was approved.
        device.pubkey_fp = latest.pubkey_fp
        await session.commit()
        return True

    async def deny(self, session: AsyncSession, device_id: str, actor: str = "cli") -> bool:
        device = await session.get(Device, device_id)
        latest = await self._latest_request(session, device_id)
        if device is None or latest is None or latest.status != "pending":
            return False
        latest.status = "denied"
        latest.decided_at = utcnow()
        latest.decided_by = actor
        device.approved = False
        await session.commit()
        return True

    async def pending(self, session: AsyncSession) -> list[EnrollRequest]:
        rows = await session.scalars(
            select(EnrollRequest)
            .where(EnrollRequest.status == "pending")
            .order_by(EnrollRequest.created_at)
        )
        return list(rows)

    # --- internals ---

    async def _latest_request(self, session: AsyncSession, device_id: str) -> EnrollRequest | None:
        return await session.scalar(
            select(EnrollRequest)
            .where(EnrollRequest.device_id == device_id)
            .order_by(EnrollRequest.id.desc())
            .limit(1)
        )

    async def _upsert_pending(
        self, session: AsyncSession, device_id: str, cn: str, fp: str, csr_pem: bytes, reason: str
    ) -> None:
        latest = await self._latest_request(session, device_id)
        if latest is not None and latest.status == "pending":
            if latest.pubkey_fp == fp:
                return  # same request re-POSTed while pending — keep the row
            latest.status = "superseded"
            latest.decided_at = utcnow()
        session.add(
            EnrollRequest(
                device_id=device_id,
                cn=cn,
                pubkey_fp=fp,
                csr_pem=csr_pem.decode(errors="replace"),
                reason=reason,
            )
        )

    async def _supersede_pending(self, session: AsyncSession, device_id: str) -> None:
        for row in await self.pending(session):
            if row.device_id == device_id:
                row.status = "superseded"
                row.decided_at = utcnow()

    async def _rate_ok(self, session: AsyncSession, device_id: str) -> bool:
        cutoff = utcnow() - timedelta(hours=1)
        n = await session.scalar(
            select(func.count())
            .select_from(EnrollLedger)
            .where(EnrollLedger.device_id == device_id, EnrollLedger.issued_at > cutoff)
        )
        return (n or 0) < self.max_per_hour
