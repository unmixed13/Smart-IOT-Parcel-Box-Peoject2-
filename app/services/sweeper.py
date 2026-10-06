"""Background task that drives the time-based QR transitions (see qr_service.sweep)."""
import asyncio
import logging

from app.config import settings
from app.database import AsyncSessionLocal
from app.services import qr_service
from app.services.line_messaging import notify_safe
from app.websocket_manager import manager

logger = logging.getLogger("parcel_box.sweeper")


async def dispatch(out: qr_service.Outcome) -> None:
    for n in out.notices:
        await notify_safe(n.text, n.image_path, n.to)
    for b in out.broadcast:
        await manager.broadcast({"source": "api", **b})


async def sweep_once() -> None:
    async with AsyncSessionLocal() as db:
        out = await qr_service.sweep(db)
    await dispatch(out)


async def run_forever() -> None:
    while True:
        try:
            await sweep_once()
        except asyncio.CancelledError:
            raise
        except Exception:
            logger.exception("sweeper iteration failed")
        await asyncio.sleep(settings.sweeper_interval_seconds)
