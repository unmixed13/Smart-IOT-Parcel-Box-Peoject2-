"""
Image upload from the ESP32-CAM.

The image is matched to the delivery (Event-ID) that is waiting for a photo;
on a match the owner gets a LINE message with the image. Without a waiting
delivery the image is only stored and logged.

Security: content-type AND magic-byte checks, server-generated filenames,
directory named from the authenticated device (never client input), hard
size cap enforced while streaming.
"""
import logging
import uuid
from pathlib import Path

import aiofiles
from fastapi import APIRouter, Depends, File, Form, UploadFile
from sqlalchemy.ext.asyncio import AsyncSession

from app.config import settings
from app.database import get_db
from app.exceptions import FileTooLargeError, UnsupportedFileTypeError
from app.models import AccessLog, AccessResult, Device, EventType
from app.schemas import ImageUploadResponse
from app.security import authenticate_device
from app.services import qr_service
from app.services.sweeper import dispatch
from app.websocket_manager import manager

logger = logging.getLogger("parcel_box.upload")

router = APIRouter(prefix="/upload-image", tags=["upload"])

_ALLOWED_SIGNATURES: dict[bytes, str] = {b"\xff\xd8\xff": "jpg", b"\x89PNG\r\n\x1a\n": "png"}
_ALLOWED_CONTENT_TYPES = {"image/jpeg", "image/jpg", "image/png"}


def _detect_extension(header: bytes) -> str | None:
    for signature, ext in _ALLOWED_SIGNATURES.items():
        if header.startswith(signature):
            return ext
    return None


@router.post("", response_model=ImageUploadResponse, status_code=201)
async def upload_image(
    event_id: uuid.UUID | None = Form(default=None),
    file: UploadFile = File(...),
    device: Device = Depends(authenticate_device),
    db: AsyncSession = Depends(get_db),
):
    """multipart/form-data: `file` (JPEG/PNG) and optional `event_id` from /device/pending-capture."""
    if file.content_type not in _ALLOWED_CONTENT_TYPES:
        raise UnsupportedFileTypeError(f"Content-Type '{file.content_type}' is not an accepted image type")

    header = await file.read(16)
    ext = _detect_extension(header)
    if ext is None:
        raise UnsupportedFileTypeError("File signature does not match a supported image format")

    device_dir: Path = settings.upload_path / "vision_captures" / device.device_id
    device_dir.mkdir(parents=True, exist_ok=True)
    destination = device_dir / f"{uuid.uuid4().hex}.{ext}"

    max_size = settings.max_upload_size_bytes
    bytes_written = len(header)
    try:
        async with aiofiles.open(destination, "wb") as out_file:
            await out_file.write(header)
            while chunk := await file.read(1024 * 64):
                bytes_written += len(chunk)
                if bytes_written > max_size:
                    raise FileTooLargeError(f"Image exceeds max size of {settings.max_upload_size_mb} MB")
                await out_file.write(chunk)
    except FileTooLargeError:
        destination.unlink(missing_ok=True)
        raise
    finally:
        await file.close()

    outcome = qr_service.Outcome()
    event = await qr_service.find_awaiting_capture(db, device.box, event_id, lock=True)
    notes = "Image captured and stored"
    if event is not None:
        event.image_path = str(destination)
        event.notified = True
        notes = f"Delivery photo for event {event.id}"
        outcome.notices.append(qr_service.delivery_notice(event, str(destination)))
    log = AccessLog(
        device_id=device.device_id, event_type=EventType.IMAGE_CAPTURE, result=AccessResult.GRANTED,
        image_path=str(destination), notes=notes,
    )
    db.add(log)
    await db.commit()
    await db.refresh(log)

    outcome.broadcast.append({
        "event": "image_captured", "device_id": device.device_id, "image_path": str(destination),
        "log_id": str(log.id), "event_id": str(event.id) if event else None,
    })
    await dispatch(outcome)
    logger.info("Stored image %s (%d bytes) for device %s", destination, bytes_written, device.device_id)

    return ImageUploadResponse(
        file_path=str(destination), size_bytes=bytes_written, device_id=device.device_id,
        log_id=log.id, event_id=event.id if event else None,
    )
