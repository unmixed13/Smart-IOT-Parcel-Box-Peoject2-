"""
Minimal per-IP fixed-window rate limiter for the hardware-facing API.

No external dependency: a single process-local dict is enough here (this
app is designed to run as one worker — see the "Production notes" in the
README about the MQTT bridge/WebSocket manager being per-process). Protects
the DB and MQTT bridge from a malfunctioning or compromised device flooding
the server, which is a stability concern as much as a security one.
"""
import time
from collections import defaultdict

from starlette.middleware.base import BaseHTTPMiddleware
from starlette.requests import Request
from starlette.responses import JSONResponse

from app.config import settings


class RateLimitMiddleware(BaseHTTPMiddleware):
    def __init__(self, app, limit_per_minute: int | None = None) -> None:
        super().__init__(app)
        self._limit = limit_per_minute or settings.rate_limit_per_minute
        self._window_seconds = 60
        self._hits: dict[str, list[float]] = defaultdict(list)

    async def dispatch(self, request: Request, call_next):
        if not request.url.path.startswith(settings.api_v1_prefix):
            return await call_next(request)

        client_ip = request.client.host if request.client else "unknown"
        now = time.monotonic()
        window_start = now - self._window_seconds

        hits = self._hits[client_ip]
        while hits and hits[0] < window_start:
            hits.pop(0)

        if len(hits) >= self._limit:
            return JSONResponse(
                status_code=429,
                content={"error": "rate_limited", "detail": "Too many requests, slow down"},
            )

        hits.append(now)
        return await call_next(request)
