"""
Authentication.

- Devices: per-device API key in `X-API-Key`. Only the SHA-256 hash is stored,
  so the key identifies the device (and its box) without a separate header.
- Admin / operator dashboard: the single `HARDWARE_API_KEY` from .env.
"""
import hashlib
import hmac

from fastapi import Depends, HTTPException, status
from fastapi.security import APIKeyHeader
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.config import settings
from app.database import get_db
from app.models import Device


# Declares the scheme so Swagger UI (/docs) shows the Authorize button.
_api_key_header = APIKeyHeader(name="X-API-Key", auto_error=False)


def hash_api_key(key: str) -> str:
    return hashlib.sha256(key.encode("utf-8")).hexdigest()


def _unauthorized() -> HTTPException:
    return HTTPException(
        status_code=status.HTTP_401_UNAUTHORIZED,
        detail="Invalid or missing API key",
        headers={"WWW-Authenticate": "API-Key"},
    )


async def verify_hardware_api_key(x_api_key: str | None = Depends(_api_key_header)) -> str:
    """Admin key check (constant time)."""
    if not x_api_key or not hmac.compare_digest(settings.hardware_api_key.encode(), x_api_key.encode()):
        raise _unauthorized()
    return x_api_key


async def authenticate_device(x_api_key: str | None = Depends(_api_key_header), db: AsyncSession = Depends(get_db)) -> Device:
    """Resolve the calling device from its own API key."""
    if not x_api_key:
        raise _unauthorized()
    device = (
        await db.execute(select(Device).where(Device.api_key_hash == hash_api_key(x_api_key)))
    ).scalar_one_or_none()
    if device is None or not device.is_active:
        raise _unauthorized()
    return device
