"""
ORM models.

- Device: an ESP32 / ESP32-CAM with its own API key (stored as a SHA-256 hash).
  Several devices (main controller + camera) can belong to one box via box_id.
- BoxBinding: which LINE user owns which box (set by an admin).
- QREvent: one delivery QR code and its life cycle (thesis table 3.1):
      pending -> unlocking -> used        pending -> expired
      unlocking -> pending (ESP32 reports timeout)
      unlocking -> used    (door opened, or no report within 15 s: fail-secure)
- AccessLog: audit trail of every event at the box.
"""
import enum
import uuid
from datetime import datetime

from sqlalchemy import Boolean, DateTime, Enum, ForeignKey, Index, Integer, String, Text, UniqueConstraint, func
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base

_MYSQL = {"mysql_charset": "utf8mb4", "mysql_collate": "utf8mb4_unicode_ci"}


class AccessResult(str, enum.Enum):
    GRANTED = "granted"
    DENIED = "denied"
    ERROR = "error"


# NOTE: values must stay <= 14 chars (the existing access_logs.event_type column is VARCHAR(14)).
class EventType(str, enum.Enum):
    QR_SCAN = "qr_scan"
    DOOR_OPEN = "door_open"
    DOOR_CLOSE = "door_close"
    TAMPER = "tamper"
    IMAGE_CAPTURE = "image_capture"
    UNLOCK_COMMAND = "unlock_command"
    HEARTBEAT = "heartbeat"
    QR_CREATED = "qr_created"
    QR_EXPIRED = "qr_expired"
    UNLOCK_TIMEOUT = "unlock_timeout"
    LOCKED = "locked"
    DOOR_AJAR = "door_ajar"
    MANUAL_UNLOCK = "manual_unlock"


class QRStatus(str, enum.Enum):
    PENDING = "pending"
    UNLOCKING = "unlocking"
    USED = "used"
    EXPIRED = "expired"


class Device(Base):
    __tablename__ = "devices"

    device_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    box_id: Mapped[str | None] = mapped_column(String(64), nullable=True)  # None -> the device is its own box
    name: Mapped[str | None] = mapped_column(String(120), nullable=True)
    api_key_hash: Mapped[str] = mapped_column(String(64), unique=True, nullable=False)
    is_active: Mapped[bool] = mapped_column(Boolean, default=True, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)

    __table_args__ = (_MYSQL,)

    @property
    def box(self) -> str:
        return self.box_id or self.device_id


class BoxBinding(Base):
    __tablename__ = "box_bindings"

    id: Mapped[uuid.UUID] = mapped_column(primary_key=True, default=uuid.uuid4)
    line_user_id: Mapped[str] = mapped_column(String(64), nullable=False)
    box_id: Mapped[str] = mapped_column(String(64), nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)

    __table_args__ = (UniqueConstraint("line_user_id", "box_id", name="uq_binding_user_box"), _MYSQL)


class QREvent(Base):
    __tablename__ = "qr_events"

    id: Mapped[uuid.UUID] = mapped_column(primary_key=True, default=uuid.uuid4)  # the Event-ID
    code: Mapped[str] = mapped_column(String(36), unique=True, nullable=False)  # UUID string in the QR
    box_id: Mapped[str] = mapped_column(String(64), nullable=False)
    line_user_id: Mapped[str] = mapped_column(String(64), nullable=False)
    status: Mapped[QRStatus] = mapped_column(Enum(QRStatus, native_enum=False), default=QRStatus.PENDING, nullable=False)

    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    expires_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    unlocking_since: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    unlock_count: Mapped[int] = mapped_column(Integer, default=0, nullable=False)
    used_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))

    door_open_since: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    ajar_alerted: Mapped[bool] = mapped_column(Boolean, default=False, nullable=False)
    lock_reported_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    image_deadline: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    image_path: Mapped[str | None] = mapped_column(String(500))
    notified: Mapped[bool] = mapped_column(Boolean, default=False, nullable=False)  # delivery notice sent

    __table_args__ = (Index("ix_qr_events_status", "status"), Index("ix_qr_events_box_user", "box_id", "line_user_id"), _MYSQL)


class AccessLog(Base):
    """Audit log of every event that occurs at the box."""

    __tablename__ = "access_logs"

    id: Mapped[uuid.UUID] = mapped_column(primary_key=True, default=uuid.uuid4)
    device_id: Mapped[str] = mapped_column(String(80), nullable=False)
    event_type: Mapped[EventType] = mapped_column(Enum(EventType, native_enum=False), nullable=False)
    result: Mapped[AccessResult] = mapped_column(
        Enum(AccessResult, native_enum=False), nullable=False, default=AccessResult.ERROR
    )
    qr_code_scanned: Mapped[str | None] = mapped_column(String(255), nullable=True)
    # Kept as a plain column (legacy FK to the removed parcels_whitelist table dropped).
    whitelist_id: Mapped[uuid.UUID | None] = mapped_column(nullable=True)
    image_path: Mapped[str | None] = mapped_column(String(500), nullable=True)
    notes: Mapped[str | None] = mapped_column(Text, nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)

    __table_args__ = (
        Index("ix_access_logs_device_created", "device_id", "created_at"),
        Index("ix_access_logs_event_type", "event_type"),
        _MYSQL,
    )
