"""
Pydantic v2 schemas — the contract layer between the API and the outside
world. Kept separate from ORM models (app.models) so DB structure can evolve
independently of the public API shape.
"""
import uuid
from datetime import datetime

from pydantic import BaseModel, ConfigDict, Field

from app.models import AccessResult, EventType


# --- Whitelist ---

class ParcelWhitelistBase(BaseModel):
    qr_code: str = Field(..., min_length=4, max_length=255)
    courier_name: str = Field(..., min_length=1, max_length=120)
    tracking_number: str | None = None
    is_active: bool = True
    expires_at: datetime | None = None


class ParcelWhitelistCreate(ParcelWhitelistBase):
    pass


class ParcelWhitelistRead(ParcelWhitelistBase):
    model_config = ConfigDict(from_attributes=True)

    id: uuid.UUID
    created_at: datetime
    updated_at: datetime


# --- Access Logs ---

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


# --- QR Verification ---

class QRVerifyRequest(BaseModel):
    device_id: str = Field(..., min_length=1, max_length=80)
    qr_code: str = Field(..., min_length=1, max_length=255)


class QRVerifyResponse(BaseModel):
    granted: bool
    reason: str
    courier_name: str | None = None


# --- Image Upload ---

class ImageUploadResponse(BaseModel):
    file_path: str
    size_bytes: int
    device_id: str
    log_id: uuid.UUID


# --- MQTT Command ---

class UnlockCommand(BaseModel):
    device_id: str
    reason: str = "manual_override"
