"""Operator CLI for enrollment approvals (the UI arrives in a later phase).

Usage (inside the container):
    python -m budka_manager.cli pending
    python -m budka_manager.cli approve cb-ex01
    python -m budka_manager.cli deny cb-ffffff
"""

from __future__ import annotations

import argparse
import asyncio
import sys

from .ca import CAConfig, load_ca
from .db import session_factory
from .enrollment import EnrollmentService
from .settings import get_settings


def _service() -> EnrollmentService:
    settings = get_settings()
    ca_cfg = CAConfig(
        cert_path=settings.ca_cert_file,
        key_path=settings.ca_key_file,
        validity_days=settings.enroll_validity_days,
        permitted_suffixes=settings.enroll_suffix_tuple,
    )
    return EnrollmentService(
        ca_cfg,
        load_ca(ca_cfg),
        max_per_hour=settings.enroll_max_per_hour,
        trusted_devices=settings.enroll_trusted_tuple,
    )


async def _run(args: argparse.Namespace) -> int:
    service = _service()
    async with session_factory()() as session:
        if args.cmd == "pending":
            rows = await service.pending(session)
            if not rows:
                print("no pending enrollment requests")
                return 0
            for r in rows:
                print(
                    f"{r.device_id}  cn={r.cn}  reason={r.reason}  "
                    f"since={r.created_at:%Y-%m-%d %H:%M}Z  fp={r.pubkey_fp[:16]}…"
                )
            return 0
        if args.cmd == "approve":
            ok = await service.approve(session, args.device_id)
        else:
            ok = await service.deny(session, args.device_id)
        if not ok:
            print(f"{args.device_id}: no pending request found", file=sys.stderr)
            return 1
        print(f"{args.device_id}: {args.cmd}d")
        return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("pending", help="list pending enrollment requests")
    for cmd in ("approve", "deny"):
        p = sub.add_parser(cmd, help=f"{cmd} a pending request")
        p.add_argument("device_id", help="cb-<6hex>")
    args = parser.parse_args(argv)
    return asyncio.run(_run(args))


if __name__ == "__main__":
    sys.exit(main())
