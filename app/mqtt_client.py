"""
Async MQTT bridge (aiomqtt) between the ESP32 fleet and the FastAPI app.

Design:
- A single long-lived background task owns the MQTT connection and runs the
  receive loop, started in the FastAPI `lifespan` context so it shares the
  event loop with the rest of the app (no threads, no blocking I/O).
- Incoming hardware messages (status/events) are persisted to `access_logs`
  and immediately broadcast to dashboard WebSocket clients.
- Outgoing commands (e.g. unlock) are published via `publish_command`,
  callable from any REST endpoint.
- Auto-reconnect with backoff so a broker restart doesn't kill the server.
"""
import asyncio
import json
import logging
from typing import Any

import aiomqtt

from app.config import settings
from app.exceptions import MQTTPublishError
from app.websocket_manager import manager

logger = logging.getLogger("parcel_box.mqtt")


class MQTTBridge:
    def __init__(self) -> None:
        self._client: aiomqtt.Client | None = None
        self._task: asyncio.Task | None = None
        self._stopping = asyncio.Event()

    async def start(self) -> None:
        """Launch the background listener task. Call once at app startup."""
        self._stopping.clear()
        self._task = asyncio.create_task(self._run_forever(), name="mqtt-listener")
        logger.info("MQTT bridge task started")

    async def stop(self) -> None:
        """Signal shutdown and await the listener task's clean exit."""
        self._stopping.set()
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
        logger.info("MQTT bridge stopped")

    async def _run_forever(self) -> None:
        """Reconnect loop: keeps the bridge alive across broker hiccups."""
        backoff = 1
        while not self._stopping.is_set():
            try:
                async with aiomqtt.Client(
                    hostname=settings.mqtt_host,
                    port=settings.mqtt_port,
                    username=settings.mqtt_username or None,
                    password=settings.mqtt_password or None,
                    identifier=settings.mqtt_client_id,
                    keepalive=settings.mqtt_keepalive,
                ) as client:
                    self._client = client
                    backoff = 1  # reset backoff after a successful connect
                    await client.subscribe(settings.mqtt_topic_status)
                    await client.subscribe(settings.mqtt_topic_event)
                    logger.info(
                        "MQTT connected to %s:%s, subscribed to %s / %s",
                        settings.mqtt_host,
                        settings.mqtt_port,
                        settings.mqtt_topic_status,
                        settings.mqtt_topic_event,
                    )
                    async for message in client.messages:
                        await self._handle_message(message)
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                self._client = None
                logger.warning("MQTT connection lost (%s). Retrying in %ss", exc, backoff)
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 30)

    async def _handle_message(self, message: aiomqtt.Message) -> None:
        """Parse an inbound hardware message and fan it out to the dashboard."""
        topic = str(message.topic)
        try:
            raw = message.payload.decode("utf-8")
            data = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError):
            logger.warning("Discarding non-JSON MQTT payload on %s", topic)
            return

        event = {
            "source": "mqtt",
            "topic": topic,
            "device_id": data.get("device_id", topic.split("/")[1] if "/" in topic else "unknown"),
            "payload": data,
        }
        logger.info("MQTT event received: %s", event)

        # Persisting to access_logs is intentionally left to the router/service
        # layer that owns DB sessions (see app/routers/hardware.py), keeping
        # this module free of a direct DB dependency. Here we just relay live
        # events to the dashboard in real time.
        await manager.broadcast(event)

    async def publish_command(self, device_id: str, payload: dict[str, Any]) -> None:
        """Publish a JSON command to a specific device's command topic."""
        if self._client is None:
            raise MQTTPublishError(f"MQTT client not connected; cannot reach device '{device_id}'")

        topic = settings.mqtt_topic_command.format(device_id=device_id)
        try:
            await self._client.publish(topic, json.dumps(payload), qos=1)
            logger.info("Published command to %s: %s", topic, payload)
        except Exception as exc:
            raise MQTTPublishError(f"Failed to publish to '{topic}': {exc}") from exc


# Module-level singleton, wired into FastAPI's lifespan in app/main.py
mqtt_bridge = MQTTBridge()
