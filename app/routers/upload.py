"""
Image upload endpoint for the LILYGO ESP32-S3 vision node.

Security notes:
- Content-type AND magic-byte sniffing both gate what gets written to disk.
- Filenames are never taken from the client; a UUID is generated server-side
  to eliminate path-traversal and collision risks entirely.
- device_id is restricted to a safe charset since it becomes a directory
  name (blocks "../" path traversal out of the upload directory).
- Streamed to disk in chunks (aiofiles) with a hard size cap enforced
  mid-stream, so a device can't exhaust disk space with an oversized body.
"""
import logging
import re
import uuid
from pathlib import Path

import aiofiles
from fastapi import APIRouter, Depends, File, Form, UploadFile
from sqlalchemy.ext.asyncio import AsyncSession

from app.config import settings
from app.database import get_db
from app.exceptions import FileTooLargeError, UnsupportedFileTypeError
from app.models import AccessLog, AccessResult, EventType
from app.schemas import ImageUploadResponse
from app.security import verify_hardware_api_key
from app.services.line_messaging import notify_safe
from app.websocket_manager import manager

logger = logging.getLogger("parcel_box.upload")

router = APIRouter(
    prefix="/upload-image",
    tags=["upload"],
    dependencies=[Depends(verify_hardware_api_key)],
)

# Magic bytes for the formats the ESP32-S3 camera realistically produces.
_ALLOWED_SIGNATURES: dict[bytes, str] = {
    b"\xff\xd8\xff": "jpg",
    b"\x89PNG\r\n\x1a\n": "png",
}
_ALLOWED_CONTENT_TYPES = {"image/jpeg", "image/jpg", "image/png"}

# device_id becomes a directory name, so restrict it to a safe charset
# (blocks "../" path traversal out of the upload directory).
_DEVICE_ID_RE = re.compile(r"[A-Za-z0-9_-]{1,64}")


def _detect_extension(header: bytes) -> str | None:
    for signature, ext in _ALLOWED_SIGNATURES.items():
        if header.startswith(signature):
            return ext
    return None


@router.post("", response_model=ImageUploadResponse, status_code=201)
async def upload_image(
    device_id: str = Form(...),
    event_type: str = Form(default=EventType.IMAGE_CAPTURE.value),
    file: UploadFile = File(...),
    db: AsyncSession = Depends(get_db),
):
    """
    Securely persist an image captured by the vision node and log it.

    multipart/form-data fields:
      - device_id: the reporting ESP32-S3's identifier
      - event_type: optional, defaults to "image_capture"
      - file: the JPEG/PNG image
    """
    if not _DEVICE_ID_RE.fullmatch(device_id):
        raise UnsupportedFileTypeError("Invalid device_id (allowed: letters, digits, - and _)")

    if file.content_type not in _ALLOWED_CONTENT_TYPES:
        raise UnsupportedFileTypeError(
            f"Content-Type '{file.content_type}' is not an accepted image type"
        )

    # Peek at the first bytes to verify the real file type (don't trust the
    # client-supplied Content-Type header alone).
    header = await file.read(16)
    ext = _detect_extension(header)
    if ext is None:
        raise UnsupportedFileTypeError("File signature does not match a supported image format")

    device_dir: Path = settings.upload_path / "vision_captures" / device_id
    device_dir.mkdir(parents=True, exist_ok=True)

    filename = f"{uuid.uuid4().hex}.{ext}"
    destination = device_dir / filename

    max_size = settings.max_upload_size_bytes
    bytes_written = len(header)

    try:
        async with aiofiles.open(destination, "wb") as out_file:
            await out_file.write(header)
            while chunk := await file.read(1024 * 64):
                bytes_written += len(chunk)
                if bytes_written > max_size:
                    raise FileTooLargeError(
                        f"Image exceeds max size of {settings.max_upload_size_mb} MB"
                    )
                await out_file.write(chunk)
    except FileTooLargeError:
        destination.unlink(missing_ok=True)
        raise
    finally:
        await file.close()

    log = AccessLog(
        device_id=device_id,
        event_type=EventType(event_type) if event_type in EventType._value2member_map_ else EventType.IMAGE_CAPTURE,
        result=AccessResult.GRANTED,
        image_path=str(destination),
        notes="Image captured and stored",
    )
    db.add(log)
    await db.commit()
    await db.refresh(log)

    await manager.broadcast(
        {
            "source": "api",
            "event": "image_captured",
            "device_id": device_id,
            "image_path": str(destination),
            "log_id": str(log.id),
        }
    )

    # Best-effort alert; failures are logged internally and never surface here.
    await notify_safe(f"📷 New capture from {device_id}", image_path=str(destination))

    logger.info("Stored image %s (%d bytes) for device %s", destination, bytes_written, device_id)

    return ImageUploadResponse(
        file_path=str(destination),
        size_bytes=bytes_written,
        device_id=device_id,
        log_id=log.id,
    )