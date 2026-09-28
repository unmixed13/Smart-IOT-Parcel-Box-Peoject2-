"""
TEMPORARY: LINE webhook used only to discover your LINE User ID.

The main app only ever PUSHES messages to LINE (see services/line_messaging.py)
and has no webhook, so LINE never tells us a user's ID on its own. This tiny
endpoint logs the userId of whoever messages the bot, so you can copy it into
.env as LINE_USER_ID. Safe to leave in place afterward — it does nothing
harmful, just logs and returns 200 OK.

Setup:
1. Make sure this router is included in app/main.py (see instructions below).
2. Get a public HTTPS URL that reaches this server (e.g. your existing
   Tailscale Funnel, or ngrok) pointing at http://127.0.0.1:8000/line-webhook
3. In LINE Developers Console -> your channel -> Messaging API tab ->
   "Webhook settings" -> set Webhook URL to that public HTTPS URL, then
   click "Verify" (should succeed), then toggle "Use webhook" ON.
4. Open LINE app, message your bot anything (e.g. "hi").
5. Check service.log (or console) for a line like:
     [LINE webhook] Message from userId=U1234567890abcdef1234567890abcdef: hi
6. Copy that userId into .env as LINE_USER_ID, restart the service.
"""
import logging

from fastapi import APIRouter, Request

logger = logging.getLogger("parcel_box.line_webhook")

router = APIRouter(tags=["line-webhook"])


@router.post("/line-webhook")
async def line_webhook(request: Request):
    body = await request.json()
    for event in body.get("events", []):
        user_id = event.get("source", {}).get("userId")
        text = event.get("message", {}).get("text", "<non-text message>")
        if user_id:
            logger.info("[LINE webhook] Message from userId=%s: %s", user_id, text)
    # LINE requires a fast 200 OK response regardless of content.
    return {"status": "ok"}
