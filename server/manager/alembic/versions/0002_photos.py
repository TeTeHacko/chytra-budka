"""photos table + archiver dedup anchors on devices

Revision ID: 0002
Revises: 0001
Create Date: 2026-07-26
"""

import sqlalchemy as sa
from alembic import op

revision = "0002"
down_revision = "0001"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "photos",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("device_id", sa.String(16), nullable=False),
        sa.Column("seq", sa.Integer(), nullable=True),
        sa.Column("day", sa.String(8), nullable=False),
        sa.Column("rel_path", sa.String(255), nullable=False, unique=True),
        sa.Column("size", sa.Integer(), nullable=False),
        sa.Column("trigger", sa.String(16), nullable=False),
        sa.Column("meta_json", sa.Text(), nullable=False),
        sa.Column("created_at", sa.DateTime(), nullable=False),
    )
    op.create_index("ix_photos_device_id", "photos", ["device_id"])
    op.create_index("ix_photos_day", "photos", ["day"])
    op.create_index("ix_photos_created_at", "photos", ["created_at"])
    op.add_column("devices", sa.Column("last_photo_seq", sa.Integer(), nullable=True))
    op.add_column("devices", sa.Column("last_photo_sha", sa.String(64), nullable=True))


def downgrade() -> None:
    op.drop_column("devices", "last_photo_sha")
    op.drop_column("devices", "last_photo_seq")
    op.drop_table("photos")
