"""
LINE Messaging API integration.

LINE Notify (the service this module used to wrap) was permanently shut
down by LINE on March 31, 2025 — all of its endpoints (notify-api.line.me,
notify-bot.line.me) now return errors. This module instead uses the LINE
Messaging API's "push message" endpoint, sent from a LINE Official Account
(Channel) directly to a specific user, which is what the project's own
design doc (2.6.4) specifies.

Key difference from the old LINE Notify flow: the Messaging API cannot
receive an uploaded image file directly. An image message must reference a
publicly reachable HTTPS URL (LINE's servers fetch it themselves) — a
localhost path will not work. If PUBLIC_BASE_URL isn't configured (e.g.
during local dev without a tunnel like ngrok/Cloudflare Tunnel), this
module gracefully falls back to a text-only notification instead of
failing the request.
"""
import logging
from pathlib import Path

import httpx

from app.config import settings

logger = logging.getLogger("parcel_box.line_messaging")

# Reused across the app's lifetime — avoids reconnect overhead per call.
_client = httpx.AsyncClient(timeout=httpx.Timeout(10.0, connect=5.0))


def _public_image_url(image_path: str) -> str | None:
    """
    Build a public HTTPS URL for a locally stored image, if possible.

    Images are served by the /media mount (see app/main.py) which exposes
    settings.upload_path. Returns None if PUBLIC_BASE_URL isn't configured
    (image messages are skipped in that case) or the path isn't inside the
    upload directory.
    """
    if not settings.public_base_url:
        return None

    try:
        relative = Path(image_path).resolve().relative_to(settings.upload_path.resolve())
    except ValueError:
        logger.warning("Image path %s is outside the upload directory; skipping image message", image_path)
        return None

    base = settings.public_base_url.rstrip("/")
    return f"{base}/media/{relative.as_posix()}"


async def send_line_message(message: str, image_path: str | None = None) -> bool:
    """
    Push a text message (and, if possible, an image message) to the
    configured LINE user via the Messaging API.

    Returns True on success, False on any failure (never raises — this is
    a best-effort side channel, not core business logic).
    """
    if not settings.line_channel_access_token or not settings.line_user_id:
        logger.debug("LINE_CHANNEL_ACCESS_TOKEN / LINE_USER_ID not set; skipping notification")
        return False

    messages: list[dict] = [{"type": "text", "text": message}]

    if image_path:
        image_url = _public_image_url(image_path)
        if image_url:
            messages.append({
                "type": "image",
                "originalContentUrl": image_url,
                "previewImageUrl": image_url,
            })
        else:
            logger.debug(
                "PUBLIC_BASE_URL not configured; sending text-only notification "
                "(image at %s was not attached)", image_path
            )

    headers = {
        "Authorization": f"Bearer {settings.line_channel_access_token}",
        "Content-Type": "application/json",
    }
    payload = {"to": settings.line_user_id, "messages": messages}

    try:
        response = await _client.post(settings.line_messaging_api_url, headers=headers, json=payload)
        response.raise_for_status()
        return True
    except httpx.HTTPStatusError as exc:
        logger.error(
            "LINE Messaging API rejected the request: %s - %s",
            exc.response.status_code,
            exc.response.text,
        )
        return False
    except httpx.RequestError as exc:
        logger.error("LINE Messaging API request failed: %s", exc)
        return False


async def notify_safe(message: str, image_path: str | None = None) -> None:
    """Fire-and-forget wrapper: logs but never raises. Use from request handlers."""
    try:
        await send_line_message(message, image_path)
    except Exception:
        logger.exception("Unexpected error sending LINE message")


async def close_line_client() -> None:
    """Call on app shutdown to release the pooled connections cleanly."""
    await _client.aclose()
