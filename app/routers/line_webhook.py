"""
LINE webhook. Verifies X-Line-Signature (HMAC-SHA256 of the raw body with the
channel secret) before touching the payload, then logs the sender's userId so
an admin can link it to a box (POST /api/admin/bindings).
"""
import base64
import hashlib
import hmac
import json
import logging

from fastapi import APIRouter, HTTPException, Request

from app.config import settings

logger = logging.getLogger("parcel_box.line_webhook")

router = APIRouter(tags=["line-webhook"])


def valid_signature(body: bytes, signature: str) -> bool:
    digest = hmac.new(settings.line_channel_secret.encode(), body, hashlib.sha256).digest()
    return hmac.compare_digest(base64.b64encode(digest).decode(), signature)


@router.post("/line-webhook")
async def line_webhook(request: Request):
    if not settings.line_channel_secret:
        raise HTTPException(503, "LINE_CHANNEL_SECRET is not configured")
    body = await request.body()
    if not valid_signature(body, request.headers.get("x-line-signature", "")):
        raise HTTPException(401, "Invalid signature")
    try:
        events = json.loads(body or b"{}").get("events", [])
    except (ValueError, AttributeError):
        raise HTTPException(400, "Invalid JSON")
    for event in events:
        user_id = event.get("source", {}).get("userId")
        if user_id:
            logger.info("[LINE webhook] %s event from userId=%s", event.get("type"), user_id)
    return {"status": "ok"}
