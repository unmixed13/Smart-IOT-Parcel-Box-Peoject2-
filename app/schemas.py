"""Pydantic v2 request/response contracts."""
import uuid
from datetime import datetime
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field

from app.models import AccessResult, EventType, QRStatus

_ID = r"^[A-Za-z0-9_-]{1,64}$"


# --- Admin ---

class DeviceCreate(BaseModel):
    device_id: str = Field(..., pattern=_ID)
    box_id: str | None = Field(default=None, pattern=_ID)
    name: str | None = Field(default=None, max_length=120)


class DeviceRead(BaseModel):
    model_config = ConfigDict(from_attributes=True)
    device_id: str
    box_id: str | None
    name: str | None
    is_active: bool
    created_at: datetime


class DeviceCreated(DeviceRead):
    api_key: str  # shown exactly once


class BindingCreate(BaseModel):
    line_user_id: str = Field(..., min_length=5, max_length=64)
    box_id: str = Field(..., pattern=_ID)


class BindingRead(BindingCreate):
    model_config = ConfigDict(from_attributes=True)
    id: uuid.UUID
    created_at: datetime


# --- Access logs ---

class AccessLogRead(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: uuid.UUID
    device_id: str
    event_type: EventType
    result: AccessResult
    qr_code_scanned: str | None
    image_path: str | None
    notes: str | None
    created_at: datetime


# --- Device API ---

class QRVerifyRequest(BaseModel):
    qr_code: str = Field(..., min_length=1, max_length=255)


class QRVerifyResponse(BaseModel):
    granted: bool
    reason: str
    event_id: uuid.UUID | None = None
    unlock_seconds: int | None = None


class DeviceEventRequest(BaseModel):
    type: Literal["door_opened", "unlock_timeout", "locked", "door_ajar", "manual_unlock"]
    event_id: uuid.UUID | None = None  # required for everything except manual_unlock


class DeviceEventResponse(BaseModel):
    accepted: bool
    status: QRStatus | None = None
    detail: str = ""


class PendingCapture(BaseModel):
    pending: bool
    event_id: uuid.UUID | None = None
    seconds_left: int | None = None


class ImageUploadResponse(BaseModel):
    file_path: str
    size_bytes: int
    device_id: str
    log_id: uuid.UUID
    event_id: uuid.UUID | None = None


class UnlockCommand(BaseModel):
    device_id: str
    reason: str = "manual_override"


# --- LIFF ---

class QRCreateRequest(BaseModel):
    box_id: str | None = Field(default=None, pattern=_ID)


class QRCreated(BaseModel):
    event_id: uuid.UUID
    code: str
    box_id: str
    status: QRStatus
    expires_at: datetime
    qr_svg: str  # inline SVG, error-correction level M


class QRRead(BaseModel):
    model_config = ConfigDict(from_attributes=True)
    id: uuid.UUID
    box_id: str
    status: QRStatus
    created_at: datetime
    expires_at: datetime
