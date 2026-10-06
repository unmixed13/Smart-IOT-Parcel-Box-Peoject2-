"""Verifies the LINE ID token sent by the LIFF page and returns the user's LINE userId."""
import logging

import httpx
from fastapi import Header, HTTPException, status

from app.config import settings

logger = logging.getLogger("parcel_box.liff_auth")


async def current_line_user(authorization: str = Header(default="")) -> str:
    """`Authorization: Bearer <LIFF id token>`; verified server-side with LINE."""
    if not settings.liff_channel_id:
        raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, "LIFF_CHANNEL_ID is not configured")
    scheme, _, token = authorization.partition(" ")
    if scheme.lower() != "bearer" or not token:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Missing LINE ID token")
    try:
        async with httpx.AsyncClient(timeout=10) as client:
            resp = await client.post(settings.line_verify_url, data={"id_token": token, "client_id": settings.liff_channel_id})
    except httpx.RequestError:
        raise HTTPException(status.HTTP_502_BAD_GATEWAY, "Could not reach LINE to verify the token")
    if resp.status_code != 200 or not resp.json().get("sub"):
        # LINE says why (e.g. "IdToken expired", "client_id does not match"); the token itself is never logged.
        logger.warning("LINE ID token rejected: %s %s", resp.status_code, resp.text[:200])
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Invalid LINE ID token")
    return resp.json()["sub"]
