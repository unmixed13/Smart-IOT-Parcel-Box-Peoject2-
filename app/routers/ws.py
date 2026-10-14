"""
Dashboard WebSocket endpoint.

Clients connect here to receive a live stream of every hardware event.

Auth: separate from the hardware API key. If DASHBOARD_TOKEN is set, the
client must send {"token": "<DASHBOARD_TOKEN>"} as its FIRST message within
5 seconds (the token is never placed in the URL, so it stays out of access
logs). A missing/wrong token closes the socket with code 1008. If the token
is unset, the endpoint is open (local development).
"""
import asyncio
import hmac
import json
import logging

from fastapi import APIRouter, WebSocket, WebSocketDisconnect, status

from app.config import settings
from app.websocket_manager import manager

logger = logging.getLogger("parcel_box.ws_router")

router = APIRouter(tags=["websocket"])


@router.websocket("/ws/dashboard")
async def dashboard_feed(websocket: WebSocket):
    await websocket.accept()

    if settings.dashboard_token:
        try:
            raw = await asyncio.wait_for(websocket.receive_text(), timeout=5)
            token = json.loads(raw).get("token", "")
        except Exception as exc:
            logger.warning("Dashboard auth failed: no valid first message (%s)", type(exc).__name__)
            await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
            return
        if not isinstance(token, str) or not hmac.compare_digest(
            token, settings.dashboard_token
        ):
            logger.warning(
                "Dashboard auth failed: token mismatch (client sent %d chars, expected %d)",
                len(token) if isinstance(token, str) else -1,
                len(settings.dashboard_token),
            )
            await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
            return

    await manager.add(websocket)
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    except Exception:
        logger.exception("Unexpected WebSocket error")
    finally:
        await manager.disconnect(websocket)
