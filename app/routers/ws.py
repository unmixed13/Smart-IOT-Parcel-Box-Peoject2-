"""
Dashboard WebSocket endpoint.

Clients connect here to receive a live stream of every hardware event —
MQTT status updates, QR verification results, image captures, and manual
unlock commands — all funneled through the shared ConnectionManager.

Auth: intentionally separate from the hardware API key (that one is for
ESP32 devices, not browsers). If DASHBOARD_TOKEN is set, the client must
connect with `?token=<DASHBOARD_TOKEN>` in the URL (see dashboard.html's
"Dashboard Token" field); a missing/wrong token is rejected before the
handshake completes. Left open (with a startup warning) if unset, so local
development keeps working without extra config.
"""
import hmac
import logging

from fastapi import APIRouter, WebSocket, WebSocketDisconnect, status

from app.config import settings
from app.websocket_manager import manager

logger = logging.getLogger("parcel_box.ws_router")

router = APIRouter(tags=["websocket"])


@router.websocket("/ws/dashboard")
async def dashboard_feed(websocket: WebSocket, token: str | None = None):
    if settings.dashboard_token:
        if not token or not hmac.compare_digest(token, settings.dashboard_token):
            await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
            return

    await manager.connect(websocket)
    try:
        while True:
            # We don't expect inbound messages, but must keep receiving to
            # detect disconnects promptly; ping/pong frames are handled by
            # the underlying ASGI server automatically.
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    except Exception:
        logger.exception("Unexpected WebSocket error")
    finally:
        await manager.disconnect(websocket)
