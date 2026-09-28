"""
ORM models.

- ParcelWhitelist: authorized couriers/parcels allowed to trigger an unlock
  (matched against QR payload scanned by the GM66 module).
- AccessLog: immutable audit trail of every access attempt, image capture,
  and hardware event — the source of truth for the dashboard and alerts.
"""
import enum
import uuid
from datetime import datetime

from sqlalchemy import (
    Boolean,
    DateTime,
    Enum,
    ForeignKey,
    Index,
    String,
    Text,
    func,
)
from sqlalchemy.orm import Mapped, mapped_column, relationship

from app.database import Base


class AccessResult(str, enum.Enum):
    GRANTED = "granted"
    DENIED = "denied"
    ERROR = "error"


class EventType(str, enum.Enum):
    QR_SCAN = "qr_scan"
    DOOR_OPEN = "door_open"
    DOOR_CLOSE = "door_close"
    TAMPER = "tamper"
    IMAGE_CAPTURE = "image_capture"
    UNLOCK_COMMAND = "unlock_command"
    HEARTBEAT = "heartbeat"


class ParcelWhitelist(Base):
    """Authorized QR codes / courier tokens permitted to unlock the box."""

    __tablename__ = "parcels_whitelist"

    id: Mapped[uuid.UUID] = mapped_column(
        primary_key=True, default=uuid.uuid4
    )
    qr_code: Mapped[str] = mapped_column(String(255), unique=True, nullable=False)
    courier_name: Mapped[str] = mapped_column(String(120), nullable=False)
    tracking_number: Mapped[str | None] = mapped_column(String(120), nullable=True)
    is_active: Mapped[bool] = mapped_column(Boolean, default=True, nullable=False)
    expires_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))

    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )

    access_logs: Mapped[list["AccessLog"]] = relationship(
        back_populates="whitelist_entry"
    )

    __table_args__ = (
        Index("ix_parcels_whitelist_qr_active", "qr_code", "is_active"),
        {"mysql_charset": "utf8mb4", "mysql_collate": "utf8mb4_unicode_ci"},
    )

    def __repr__(self) -> str:
        return f"<ParcelWhitelist qr_code={self.qr_code!r} active={self.is_active}>"


class AccessLog(Base):
    """Immutable audit log of every event that occurs at the box."""

    __tablename__ = "access_logs"

    id: Mapped[uuid.UUID] = mapped_column(
        primary_key=True, default=uuid.uuid4
    )
    device_id: Mapped[str] = mapped_column(String(80), nullable=False)
    event_type: Mapped[EventType] = mapped_column(
        Enum(EventType, native_enum=False), nullable=False
    )
    result: Mapped[AccessResult] = mapped_column(
        Enum(AccessResult, native_enum=False), nullable=False, default=AccessResult.ERROR
    )
    qr_code_scanned: Mapped[str | None] = mapped_column(String(255), nullable=True)
    whitelist_id: Mapped[uuid.UUID | None] = mapped_column(
        ForeignKey("parcels_whitelist.id", ondelete="SET NULL"), nullable=True
    )
    image_path: Mapped[str | None] = mapped_column(String(500), nullable=True)
    notes: Mapped[str | None] = mapped_column(Text, nullable=True)

    # Denormalized snapshot for fast dashboard queries without a join.
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )

    whitelist_entry: Mapped["ParcelWhitelist | None"] = relationship(
        back_populates="access_logs"
    )

    __table_args__ = (
        Index("ix_access_logs_device_created", "device_id", "created_at"),
        Index("ix_access_logs_event_type", "event_type"),
        {"mysql_charset": "utf8mb4", "mysql_collate": "utf8mb4_unicode_ci"},
    )

    def __repr__(self) -> str:
        return f"<AccessLog device={self.device_id} type={self.event_type} result={self.result}>"
