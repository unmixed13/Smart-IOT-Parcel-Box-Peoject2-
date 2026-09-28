"""
Hardware control & audit endpoints.

Everything here is behind `verify_hardware_api_key` — these are the
operator/dashboard-facing endpoints for commanding the box and reviewing
its history, not the raw device ingestion path (that's MQTT, see
app/mqtt_client.py).
"""
import logging

from fastapi import APIRouter, Depends, Query
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.database import get_db
from app.models import AccessLog
from app.mqtt_client import mqtt_bridge
from app.schemas import AccessLogRead, UnlockCommand
from app.security import verify_hardware_api_key
from app.websocket_manager import manager

logger = logging.getLogger("parcel_box.hardware")

router = APIRouter(
    prefix="/hardware",
    tags=["hardware"],
    dependencies=[Depends(verify_hardware_api_key)],
)


@router.post("/unlock", status_code=202)
async def trigger_unlock(command: UnlockCommand):
    """
    Publish an unlock command to a specific ESP32 over MQTT.

    Returns 202 Accepted — the command is fire-and-forget over MQTT; actual
    confirmation arrives asynchronously via the device's status topic and
    is broadcast to the dashboard over WebSocket.
    """
    await mqtt_bridge.publish_command(
        device_id=command.device_id,
        payload={"action": "unlock", "reason": command.reason},
    )
    await manager.broadcast(
        {"source": "api", "event": "unlock_requested", "device_id": command.device_id}
    )
    return {"status": "command_sent", "device_id": command.device_id}


@router.get("/logs", response_model=list[AccessLogRead])
async def list_access_logs(
    db: AsyncSession = Depends(get_db),
    device_id: str | None = Query(default=None),
    limit: int = Query(default=50, le=200, ge=1),
):
    """Paginated (limit-only, newest-first) access log feed for the dashboard."""
    stmt = select(AccessLog).order_by(AccessLog.created_at.desc()).limit(limit)
    if device_id:
        stmt = stmt.where(AccessLog.device_id == device_id)

    result = await db.execute(stmt)
    return result.scalars().all()
