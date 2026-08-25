"""initial: devices, enroll_requests, enroll_ledger

Revision ID: 0001
Revises:
Create Date: 2026-07-26
"""

import sqlalchemy as sa
from alembic import op

revision = "0001"
down_revision = None
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "devices",
        sa.Column("device_id", sa.String(16), primary_key=True),
        sa.Column("name", sa.String(64), nullable=True),
        sa.Column("notes", sa.Text(), nullable=True),
        sa.Column("approved", sa.Boolean(), nullable=False, default=False),
        sa.Column("pubkey_fp", sa.String(64), nullable=True),
        sa.Column("first_seen", sa.DateTime(), nullable=False),
        sa.Column("last_seen", sa.DateTime(), nullable=False),
    )
    op.create_table(
        "enroll_requests",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("device_id", sa.String(16), sa.ForeignKey("devices.device_id"), nullable=False),
        sa.Column("cn", sa.String(255), nullable=False),
        sa.Column("pubkey_fp", sa.String(64), nullable=False),
        sa.Column("csr_pem", sa.Text(), nullable=False),
        sa.Column("status", sa.String(16), nullable=False, default="pending"),
        sa.Column("reason", sa.String(64), nullable=True),
        sa.Column("created_at", sa.DateTime(), nullable=False),
        sa.Column("decided_at", sa.DateTime(), nullable=True),
        sa.Column("decided_by", sa.String(64), nullable=True),
    )
    op.create_index("ix_enroll_requests_device_id", "enroll_requests", ["device_id"])
    op.create_index("ix_enroll_requests_status", "enroll_requests", ["status"])
    op.create_table(
        "enroll_ledger",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("device_id", sa.String(16), nullable=False),
        sa.Column("cn", sa.String(255), nullable=False),
        sa.Column("serial_hex", sa.String(40), nullable=False),
        sa.Column("not_after", sa.DateTime(), nullable=False),
        sa.Column("issued_at", sa.DateTime(), nullable=False),
    )
    op.create_index("ix_enroll_ledger_device_id", "enroll_ledger", ["device_id"])
    op.create_index("ix_enroll_ledger_issued_at", "enroll_ledger", ["issued_at"])


def downgrade() -> None:
    op.drop_table("enroll_ledger")
    op.drop_table("enroll_requests")
    op.drop_table("devices")
