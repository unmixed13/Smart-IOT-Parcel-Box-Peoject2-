"""
Smart IoT Parcel Box — Centralized Web Server

Entrypoint that wires together:
  - Async SQLAlchemy lifecycle (init on startup, dispose on shutdown)
  - MQTT bridge lifecycle (connect/subscribe on startup, clean stop on shutdown)
  - Centralized exception handling
  - CORS for the dashboard frontend
  - REST routers (hardware, upload, qr) + WebSocket router

Run with:
    uvicorn app.main:app --host 0.0.0.0 --port 8000 --reload
"""
import logging
from contextlib import asynccontextmanager

from pathlib import Path

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from app.config import settings
from app.database import close_db, init_db
from app.exceptions import register_exception_handlers
from app.mqtt_client import mqtt_bridge
from app.rate_limit import RateLimitMiddleware
from app.routers import hardware, qr, upload, ws
from app.services.line_messaging import close_line_client

logging.basicConfig(
    level=logging.DEBUG if settings.debug else logging.INFO,
    format="%(asctime)s | %(levelname)-8s | %(name)s | %(message)s",
)
logger = logging.getLogger("parcel_box")

_INSECURE_DEFAULT_API_KEY = "my_super_secret_key_123"


def _warn_on_insecure_production_config() -> None:
    """Loud, best-effort guardrails against the most common misconfigurations."""
    if settings.environment.lower() == "production":
        if settings.debug:
            logger.warning(
                "DEBUG=true while ENVIRONMENT=production — this enables verbose SQL "
                "echo and stack traces. Set DEBUG=false in production."
            )
        if settings.hardware_api_key == _INSECURE_DEFAULT_API_KEY:
            logger.warning(
                "HARDWARE_API_KEY is still set to the placeholder example value. "
                "Generate a real secret before exposing this server."
            )
    if not settings.dashboard_token:
        logger.warning(
            "DASHBOARD_TOKEN is not set — /ws/dashboard accepts any connection with "
            "no authentication. Set DASHBOARD_TOKEN before exposing the dashboard publicly."
        )


@asynccontextmanager
async def lifespan(app: FastAPI):
    # --- Startup ---
    logger.info("Starting %s (%s)", settings.app_name, settings.environment)
    _warn_on_insecure_production_config()
    await init_db()
    await mqtt_bridge.start()
    logger.info("Startup complete")

    yield

    # --- Shutdown ---
    logger.info("Shutting down...")
    await mqtt_bridge.stop()
    await close_line_client()
    await close_db()
    logger.info("Shutdown complete")


app = FastAPI(
    title=settings.app_name,
    description="Non-blocking centralized backend for the Smart IoT Parcel Box.",
    version="1.0.0",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=settings.cors_origin_list,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)
app.add_middleware(RateLimitMiddleware)

register_exception_handlers(app)

# Exposes settings.upload_path at /media/* — required so LINE's servers can
# fetch image attachments by URL (see app/services/line_messaging.py). Only
# reachable from the outside if PUBLIC_BASE_URL points here through a
# tunnel/reverse proxy; harmless to leave mounted otherwise.
app.mount("/media", StaticFiles(directory=str(settings.upload_path)), name="media")

app.include_router(hardware.router, prefix=settings.api_v1_prefix)
app.include_router(upload.router, prefix=settings.api_v1_prefix)
app.include_router(qr.router, prefix=settings.api_v1_prefix)
app.include_router(ws.router)


@app.get("/health", tags=["meta"])
async def health_check():
    """Liveness/readiness probe — no auth required."""
    return {"status": "ok", "environment": settings.environment}


# Serves the operator dashboard same-origin as the API/WebSocket, so the
# browser page and the FastAPI backend share one origin — no CORS
# configuration needed for it to call /api/* or open /ws/dashboard.
_DASHBOARD_FILE = Path(__file__).resolve().parent.parent / "dashboard.html"


@app.get("/dashboard", tags=["meta"], include_in_schema=False)
async def dashboard():
    return FileResponse(_DASHBOARD_FILE, media_type="text/html")
