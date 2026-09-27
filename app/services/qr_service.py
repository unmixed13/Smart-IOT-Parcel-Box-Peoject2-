"""
QR verification business logic.

Kept separate from the router so it can be unit-tested without spinning up
FastAPI, and reused by other entry points (e.g. an MQTT-triggered scan, or
a future gRPC endpoint) without duplicating logic.
"""
from datetime import datetime, timezone

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models import AccessLog, AccessResult, EventType, ParcelWhitelist
from app.schemas import QRVerifyResponse


async def verify_qr_code(
    db: AsyncSession, device_id: str, qr_code: str
) -> tuple[QRVerifyResponse, AccessLog]:
    """
    Validate a scanned QR code against the whitelist and record the attempt.

    Returns the response to send back to the ESP32, plus the AccessLog row
    that was written (useful for the caller to trigger notifications/MQTT
    unlock commands based on the outcome).
    """
    stmt = select(ParcelWhitelist).where(ParcelWhitelist.qr_code == qr_code)
    result = await db.execute(stmt)
    entry = result.scalar_one_or_none()

    now = datetime.now(timezone.utc)

    if entry is None:
        response = QRVerifyResponse(granted=False, reason="QR code not recognized")
        log = AccessLog(
            device_id=device_id,
            event_type=EventType.QR_SCAN,
            result=AccessResult.DENIED,
            qr_code_scanned=qr_code,
            notes="No matching whitelist entry",
        )
    elif not entry.is_active:
        response = QRVerifyResponse(granted=False, reason="QR code is deactivated", courier_name=entry.courier_name)
        log = AccessLog(
            device_id=device_id,
            event_type=EventType.QR_SCAN,
            result=AccessResult.DENIED,
            qr_code_scanned=qr_code,
            whitelist_id=entry.id,
            notes="Whitelist entry inactive",
        )
    elif entry.expires_at is not None and entry.expires_at < now:
        response = QRVerifyResponse(granted=False, reason="QR code has expired", courier_name=entry.courier_name)
        log = AccessLog(
            device_id=device_id,
            event_type=EventType.QR_SCAN,
            result=AccessResult.DENIED,
            qr_code_scanned=qr_code,
            whitelist_id=entry.id,
            notes="Whitelist entry expired",
        )
    else:
        response = QRVerifyResponse(granted=True, reason="Access granted", courier_name=entry.courier_name)
        log = AccessLog(
            device_id=device_id,
            event_type=EventType.QR_SCAN,
            result=AccessResult.GRANTED,
            qr_code_scanned=qr_code,
            whitelist_id=entry.id,
            notes=f"Matched courier '{entry.courier_name}'",
        )

    db.add(log)
    await db.commit()
    await db.refresh(log)

    return response, log
