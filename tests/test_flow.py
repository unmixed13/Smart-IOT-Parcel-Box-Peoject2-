import base64
import hashlib
import hmac
import uuid
from datetime import timedelta

from sqlalchemy import update

from app.database import AsyncSessionLocal
from app.models import QREvent
from app.services import sweeper
from app.timeutil import utcnow
from conftest import ADMIN, sent

PNG = b"\x89PNG\r\n\x1a\n" + b"0" * 64


def sweep(c):
    """Run the sweeper on the app's own event loop (DB connections are loop-bound)."""
    c.portal.call(sweeper.sweep_once)


def shift(c, event_id, **fields):
    """Move timestamps of an event into the past to simulate elapsed time."""
    async def go():
        async with AsyncSessionLocal() as db:
            await db.execute(update(QREvent).where(QREvent.id == uuid.UUID(event_id)).values(**fields))
            await db.commit()
    c.portal.call(go)


def make_qr(c):
    r = c.post("/api/liff/qr", json={})
    assert r.status_code == 201, r.text
    assert "<svg" in r.json()["qr_svg"]
    return r.json()


def scan(c, box, code):
    return c.post("/api/device/verify-qr", json={"qr_code": code}, headers=box["esp"])


def event(c, box, type_, event_id=None):
    body = {"type": type_}
    if event_id:
        body["event_id"] = event_id
    return c.post("/api/device/event", json=body, headers=box["esp"])


def test_happy_path_with_photo(c, box):
    qr = make_qr(c)
    r = scan(c, box, qr["code"]).json()
    assert r["granted"] and r["event_id"] == qr["event_id"] and r["unlock_seconds"] == 10
    assert event(c, box, "door_opened", qr["event_id"]).json()["status"] == "used"
    denied = scan(c, box, qr["code"]).json()
    assert denied["granted"] is False and denied["reason"] == "already_used" and denied["event_id"] is None
    assert event(c, box, "locked", qr["event_id"]).json()["accepted"]

    cap = c.get("/api/device/pending-capture", headers=box["cam"]).json()
    assert cap["pending"] and cap["event_id"] == qr["event_id"]
    up = c.post("/api/upload-image", data={"event_id": qr["event_id"]}, files={"file": ("a.png", PNG, "image/png")}, headers=box["cam"])
    assert up.status_code == 201 and up.json()["event_id"] == qr["event_id"]
    assert not c.get("/api/device/pending-capture", headers=box["cam"]).json()["pending"]
    delivered = [m for m in sent if "delivered" in m["text"]]
    assert len(delivered) == 1 and delivered[0]["image"] and delivered[0]["to"] == box["user"]


def test_rescan_keeps_event_id_and_timeout_returns_to_pending(c, box):
    qr = make_qr(c)
    first = scan(c, box, qr["code"]).json()
    again = scan(c, box, qr["code"]).json()
    assert again["granted"] and again["event_id"] == first["event_id"]
    assert event(c, box, "unlock_timeout", qr["event_id"]).json()["status"] == "pending"
    assert scan(c, box, qr["code"]).json()["granted"]  # usable again


def test_fail_secure_burns_code_and_late_timeout_is_rejected(c, box):
    qr = make_qr(c)
    scan(c, box, qr["code"])
    shift(c, qr["event_id"], unlocking_since=utcnow() - timedelta(seconds=16))
    sweep(c)
    late = event(c, box, "unlock_timeout", qr["event_id"]).json()
    assert not late["accepted"] and late["status"] == "used"
    assert scan(c, box, qr["code"]).json()["reason"] == "already_used"


def test_expiry(c, box):
    qr = make_qr(c)
    shift(c, qr["event_id"], expires_at=utcnow() - timedelta(seconds=1))
    assert scan(c, box, qr["code"]).json()["reason"] == "expired"
    qr2 = make_qr(c)
    shift(c, qr2["event_id"], expires_at=utcnow() - timedelta(seconds=1))
    sweep(c)
    assert scan(c, box, qr2["code"]).json()["reason"] == "expired"


def test_invalid_codes_are_denied_and_wrong_box_is_invalid(c, box):
    assert scan(c, box, "string").json()["reason"] == "invalid_code"
    qr = make_qr(c)
    other = c.post("/api/admin/devices", json={"device_id": "other-box"}, headers=ADMIN).json()
    r = c.post("/api/device/verify-qr", json={"qr_code": qr["code"]}, headers={"X-API-Key": other["api_key"]})
    assert r.json()["reason"] == "invalid_code"


def test_no_photo_within_30s_sends_text_only(c, box):
    qr = make_qr(c)
    scan(c, box, qr["code"]); event(c, box, "door_opened", qr["event_id"]); event(c, box, "locked", qr["event_id"])
    shift(c, qr["event_id"], image_deadline=utcnow() - timedelta(seconds=1))
    assert not c.get("/api/device/pending-capture", headers=box["cam"]).json()["pending"]
    sweep(c)
    sweep(c)  # idempotent: only one message
    msgs = [m for m in sent if "delivered" in m["text"]]
    assert len(msgs) == 1 and msgs[0]["image"] is None


def test_door_ajar_alert_once(c, box):
    qr = make_qr(c)
    scan(c, box, qr["code"]); event(c, box, "door_opened", qr["event_id"])
    shift(c, qr["event_id"], door_open_since=utcnow() - timedelta(seconds=61))
    sweep(c)
    assert event(c, box, "door_ajar", qr["event_id"]).json()["accepted"] is False  # already alerted
    sweep(c)
    assert len([m for m in sent if "open for more than" in m["text"]]) == 1


def test_manual_unlock_notifies_owner_without_photo(c, box):
    assert event(c, box, "manual_unlock").json()["accepted"]
    assert any("manual override" in m["text"] and m["to"] == box["user"] for m in sent)


def test_auth(c, box):
    assert c.post("/api/device/verify-qr", json={"qr_code": "x"}, headers={"X-API-Key": "nope"}).status_code == 401
    assert c.post("/api/device/verify-qr", json={"qr_code": "x"}, headers={"X-API-Key": "admin-key"}).status_code == 401  # admin key is not a device key
    assert c.get("/api/admin/devices", headers=box["esp"]).status_code == 401
    assert c.get("/api/hardware/logs", headers=box["esp"]).status_code == 401
    assert c.get("/api/hardware/logs", headers=ADMIN).status_code == 200


def test_liff_requires_binding(c):
    from app.liff_auth import current_line_user
    from app.main import app
    app.dependency_overrides[current_line_user] = lambda: "U" + "z" * 32
    assert c.post("/api/liff/qr", json={}).status_code == 403
    assert c.get("/api/liff/me").json() == {"boxes": []}


def test_webhook_signature(c):
    body = b'{"events":[{"type":"message","source":{"userId":"Uabc"}}]}'
    sig = base64.b64encode(hmac.new(b"secret", body, hashlib.sha256).digest()).decode()
    assert c.post("/line-webhook", content=body, headers={"x-line-signature": sig}).status_code == 200
    assert c.post("/line-webhook", content=body, headers={"x-line-signature": "bad"}).status_code == 401
    bad = b"not json"
    sig2 = base64.b64encode(hmac.new(b"secret", bad, hashlib.sha256).digest()).decode()
    assert c.post("/line-webhook", content=bad, headers={"x-line-signature": sig2}).status_code == 400


def test_missing_key_is_401_and_openapi_declares_scheme(c):
    assert c.post("/api/device/verify-qr", json={"qr_code": "x"}).status_code == 401
    assert c.get("/api/hardware/logs").status_code == 401
    assert "APIKeyHeader" in c.get("/openapi.json").json()["components"]["securitySchemes"]


def test_dashboard_config_only_for_local_browser():
    import asyncio
    import httpx
    from app.main import app

    async def get(client_ip, **headers):
        t = httpx.ASGITransport(app=app, client=(client_ip, 5555))
        async with httpx.AsyncClient(transport=t, base_url="http://localhost:8888") as ac:
            return await ac.get("/dashboard-config", headers=headers)

    r = asyncio.run(get("127.0.0.1"))
    assert r.status_code == 200 and r.json()["api_key"] == "admin-key"
    # public tunnel: loopback source but public Host / forwarding headers / remote source
    assert asyncio.run(get("127.0.0.1", Host="box.example.ts.net")).status_code == 404
    assert asyncio.run(get("127.0.0.1", **{"X-Forwarded-For": "1.2.3.4"})).status_code == 404
    assert asyncio.run(get("127.0.0.1", **{"Tailscale-Funnel-Request": "?1"})).status_code == 404
    assert asyncio.run(get("203.0.113.9")).status_code == 404


def test_firmware_contract_heartbeat_tamper_raw_upload(c, box):
    # verify reply carries the alert time for the ESP32 buzzer
    qr = make_qr(c)
    r = scan(c, box, qr["code"]).json()
    assert r["door_open_alert_seconds"] == 60 and r["unlock_seconds"] == 10
    # heartbeat (free-form) and tamper (no event_id)
    assert c.post("/api/device/heartbeat", json={"door": "closed", "lock": "locked", "rssi": -60}, headers=box["esp"]).json() == {"ok": True}
    assert c.post("/api/device/heartbeat", json={}, headers={"X-API-Key": "bad"}).status_code == 401
    assert event(c, box, "tamper").json()["accepted"]
    assert any("tampering" in m["text"] and m["to"] == box["user"] for m in sent)
    # raw JPEG upload exactly as the ESP32-CAM sends it, matched via ?event_id=
    event(c, box, "door_opened", qr["event_id"]); event(c, box, "locked", qr["event_id"])
    jpg = b"\xff\xd8\xff\xe0" + b"1" * 5000
    up = c.post(f"/api/upload-image/raw?event_id={qr['event_id']}", content=jpg,
                headers={**box["cam"], "Content-Type": "image/jpeg"})
    assert up.status_code == 201 and up.json()["event_id"] == qr["event_id"] and up.json()["size_bytes"] == len(jpg)
    assert any("delivered" in m["text"] and m["image"] for m in sent)
    # bad content type / signature are refused
    assert c.post("/api/upload-image/raw", content=jpg, headers={**box["cam"], "Content-Type": "text/plain"}).status_code == 415
    assert c.post("/api/upload-image/raw", content=b"notanimage" * 10, headers={**box["cam"], "Content-Type": "image/jpeg"}).status_code == 415
