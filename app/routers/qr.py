"""
QR verification endpoint — called by the ESP32 main controller right after
the GM66 module decodes a code, before it decides whether to fire the
solenoid locally or wait for an unlock command from the server.
"""
import logging

from fastapi import APIRouter, Depends
from sqlalchemy.ext.asyncio import AsyncSession

from app.database import get_db
from app.mqtt_client import mqtt_bridge
from app.schemas import QRVerifyRequest, QRVerifyResponse
from app.security import verify_hardware_api_key
from app.services.line_messaging import notify_safe
from app.services.qr_service import verify_qr_code
from app.websocket_manager import manager

logger = logging.getLogger("parcel_box.qr")

router = APIRouter(
    prefix="/verify-qr",
    tags=["qr"],
    dependencies=[Depends(verify_hardware_api_key)],
)


@router.post("", response_model=QRVerifyResponse)
async def verify_qr(request: QRVerifyRequest, db: AsyncSession = Depends(get_db)):
    """
    Validate a scanned QR code. On success, also publishes an MQTT unlock
    command directly to the originating device and broadcasts the event to
    the live dashboard.
    """
    response, log = await verify_qr_code(db, request.device_id, request.qr_code)

    await manager.broadcast(
        {
            "source": "api",
            "event": "qr_verified",
            "device_id": request.device_id,
            "granted": response.granted,
            "reason": response.reason,
            "log_id": str(log.id),
        }
    )

    if response.granted:
        await mqtt_bridge.publish_command(
            device_id=request.device_id,
            payload={"action": "unlock", "reason": "qr_verified"},
        )
    else:
        await notify_safe(
            f"⚠️ Denied access attempt at {request.device_id}: {response.reason}"
        )

    return response
