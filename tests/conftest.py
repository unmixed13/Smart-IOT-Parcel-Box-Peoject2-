import os
import sys
import tempfile
from pathlib import Path

_tmp = tempfile.mkdtemp()
os.environ.update(
    HARDWARE_API_KEY="admin-key", DATABASE_URL=f"sqlite+aiosqlite:///{_tmp}/t.db", DEBUG="false",
    UPLOAD_DIR=f"{_tmp}/uploads", LINE_CHANNEL_SECRET="secret", LIFF_CHANNEL_ID="chan",
    SWEEPER_INTERVAL_SECONDS="3600", MQTT_HOST="127.0.0.1", MQTT_PORT="1",
)
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import pytest
from fastapi.testclient import TestClient

from app.liff_auth import current_line_user
from app.main import app
from app.services import line_messaging

ADMIN = {"X-API-Key": "admin-key"}
USER = "U" + "a" * 32
sent: list[dict] = []


async def _fake_send(message, image_path=None, to=None):
    sent.append({"text": message, "image": image_path, "to": to})
    return True


@pytest.fixture()
def c(monkeypatch):
    monkeypatch.setattr(line_messaging, "send_line_message", _fake_send)
    sent.clear()
    app.dependency_overrides[current_line_user] = lambda: USER
    with TestClient(app) as client:
        yield client
    app.dependency_overrides.clear()


@pytest.fixture()
def box(c):
    """Registers box-01 (+ camera) and links USER; returns header dicts."""
    uid = os.urandom(3).hex()
    out = {}
    for name, box_id in ((f"esp-{uid}", None), (f"cam-{uid}", f"esp-{uid}")):
        r = c.post("/api/admin/devices", json={"device_id": name, "box_id": box_id}, headers=ADMIN)
        assert r.status_code == 201, r.text
        out[name] = {"X-API-Key": r.json()["api_key"]}
    esp, cam = out[f"esp-{uid}"], out[f"cam-{uid}"]
    user = "U" + uid * 5 + "0" * 2
    c.post("/api/admin/bindings", json={"line_user_id": user, "box_id": f"esp-{uid}"}, headers=ADMIN)
    app.dependency_overrides[current_line_user] = lambda: user
    return {"id": f"esp-{uid}", "esp": esp, "cam": cam, "user": user}
