"""
Endpoints called by the ESP32 main controller and the ESP32-CAM.
Authenticated with the per-device key (`X-API-Key`), see app/security.py.
"""
import logging

from fastapi import APIRouter, Depends
from sqlalchemy.ext.asyncio import AsyncSession

from app.database import get_db
from app.models import Device
from app.mqtt_client import mqtt_bridge
from app.schemas import (
    DeviceEventRequest, DeviceEventResponse, PendingCapture, QRVerifyRequest, QRVerifyResponse,
)
from app.security import authenticate_device
from app.services import qr_service
from app.services.sweeper import dispatch
from app.timeutil import as_utc, utcnow

logger = logging.getLogger("parcel_box.device")

router = APIRouter(prefix="/device", tags=["device"])


@router.post("/verify-qr", response_model=QRVerifyResponse)
async def verify_qr(body: QRVerifyRequest, device: Device = Depends(authenticate_device), db: AsyncSession = Depends(get_db)):
    """The GM66 scanned a code. Pending -> Unlocking; the reply tells the ESP32 to energise the solenoid."""
    response, out = await qr_service.verify_scan(db, device, body.qr_code)
    if response.granted:
        try:  # MQTT is an optional low-latency path; the HTTP reply is authoritative.
            await mqtt_bridge.publish_command(device.device_id, {"action": "unlock", "reason": "qr_verified"})
        except Exception as exc:
            logger.warning("MQTT unlock publish skipped: %s", exc)
    await dispatch(out)
    return response


@router.post("/event", response_model=DeviceEventResponse)
async def device_event(body: DeviceEventRequest, device: Device = Depends(authenticate_device), db: AsyncSession = Depends(get_db)):
    """door_opened | unlock_timeout | locked | door_ajar | manual_unlock."""
    response, out = await qr_service.apply_device_event(db, device, body)
    await dispatch(out)
    return response


@router.get("/pending-capture", response_model=PendingCapture)
async def pending_capture(device: Device = Depends(authenticate_device), db: AsyncSession = Depends(get_db)):
    """Polled by the ESP32-CAM after the trigger: is there a delivery waiting for a photo?"""
    ev = await qr_service.find_awaiting_capture(db, device.box)
    if ev is None:
        return PendingCapture(pending=False)
    left = int((as_utc(ev.image_deadline) - utcnow()).total_seconds())
    return PendingCapture(pending=True, event_id=ev.id, seconds_left=max(left, 0))
