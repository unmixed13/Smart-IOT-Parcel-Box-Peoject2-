"""
WebSocket connection manager.

Holds the set of currently connected dashboard clients and broadcasts
JSON events to all of them. Uses an asyncio.Lock to keep the connection
set safe under concurrent connect/disconnect/broadcast operations, and
prunes dead sockets automatically on send failure.
"""
import asyncio
import json
import logging
from typing import Any

from fastapi import WebSocket

logger = logging.getLogger("parcel_box.ws")


class ConnectionManager:
    def __init__(self) -> None:
        self._connections: set[WebSocket] = set()
        self._lock = asyncio.Lock()

    async def connect(self, websocket: WebSocket) -> None:
        await websocket.accept()
        async with self._lock:
            self._connections.add(websocket)
        logger.info("Dashboard client connected (total=%d)", len(self._connections))

    async def disconnect(self, websocket: WebSocket) -> None:
        async with self._lock:
            self._connections.discard(websocket)
        logger.info("Dashboard client disconnected (total=%d)", len(self._connections))

    async def broadcast(self, event: dict[str, Any]) -> None:
        """Send a JSON-serializable event to every connected dashboard client."""
        if not self._connections:
            return

        payload = json.dumps(event, default=str)
        dead: list[WebSocket] = []

        async with self._lock:
            connections = list(self._connections)

        for ws in connections:
            try:
                await ws.send_text(payload)
            except Exception:
                dead.append(ws)

        if dead:
            async with self._lock:
                for ws in dead:
                    self._connections.discard(ws)


# Module-level singleton shared across routers and the MQTT bridge.
manager = ConnectionManager()
