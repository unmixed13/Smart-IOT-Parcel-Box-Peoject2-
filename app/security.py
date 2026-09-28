"""
API key authentication for hardware endpoints.

The ESP32 / ESP32-S3 devices authenticate with a static shared secret sent
in the `X-API-Key` header. This is intentionally simple (edge devices with
limited crypto capability) but uses constant-time comparison to avoid
timing side-channels, and is designed to be swapped for per-device keys
(stored in DB) without touching call sites — see `get_device_api_key`.
"""
import hmac

from fastapi import Header, HTTPException, status

from app.config import settings


async def verify_hardware_api_key(x_api_key: str = Header(...)) -> str:
    """
    FastAPI dependency: raises 401 if the caller's API key doesn't match.

    Usage:
        @router.post("/endpoint", dependencies=[Depends(verify_hardware_api_key)])

    Returns the validated key so callers can also do:
        api_key: str = Depends(verify_hardware_api_key)
    """
    expected = settings.hardware_api_key.encode("utf-8")
    provided = x_api_key.encode("utf-8")

    if not hmac.compare_digest(expected, provided):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid or missing API key",
            headers={"WWW-Authenticate": "API-Key"},
        )
    return x_api_key
