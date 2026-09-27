# Smart IoT Parcel Box — Centralized Web Server

Production-grade, fully async FastAPI backend for an ESP32-based smart parcel
box with a GM66 QR scanner + solenoid lock, and a LILYGO ESP32-S3 vision node.

## 1. Architecture

```
smart_iot_parcel_box/
├── app/
│   ├── main.py              # FastAPI app, lifespan (DB + MQTT), CORS, routers
│   ├── config.py             # pydantic-settings — single source of config truth
│   ├── database.py           # Async SQLAlchemy engine/session/init
│   ├── models.py              # ORM: ParcelWhitelist, AccessLog
│   ├── schemas.py             # Pydantic request/response contracts
│   ├── security.py            # API key dependency (X-API-Key header)
│   ├── exceptions.py          # Domain exceptions + centralized handlers
│   ├── mqtt_client.py         # aiomqtt bridge, wired into app lifespan
│   ├── websocket_manager.py   # Dashboard broadcast connection manager
│   ├── routers/
│   │   ├── hardware.py        # POST /hardware/unlock, GET /hardware/logs
│   │   ├── upload.py          # POST /upload-image (multipart, ESP32-S3)
│   │   ├── qr.py               # POST /verify-qr (ESP32 main controller)
│   │   └── ws.py                # WS /ws/dashboard (live event stream)
│   └── services/
│       ├── qr_service.py      # QR validation business logic
│       └── line_notify.py     # httpx-based LINE Notify client
├── uploads/                    # Local image storage (gitignored in practice)
├── requirements.txt
├── .env.example
└── README.md
```

**Why this shape:** routers stay thin (HTTP concerns only), business logic
lives in `services/`, and cross-cutting infra (DB, MQTT, WebSocket manager,
security, error handling) each get their own module so nothing is tangled.
This is the same separation you'd use for a much larger fleet of devices —
it scales down cleanly to one box and scales up cleanly to thousands.

### Request / event flow

```
ESP32 (GM66 scan)  --HTTP POST /api/verify-qr-->  FastAPI  --SQL-->  access_logs
                                                        |
                                                        +--MQTT publish--> parcelbox/{id}/command (unlock)
                                                        |
                                                        +--WS broadcast--> Dashboard

ESP32-S3 (camera)  --HTTP POST /api/upload-image-->  FastAPI --file-->  uploads/
                                                        |
                                                        +--LINE Notify (async, best-effort)
                                                        +--WS broadcast--> Dashboard

ESP32 (sensors)  --MQTT publish--> parcelbox/{id}/status|event  -->  MQTTBridge
                                                        |
                                                        +--WS broadcast--> Dashboard
```

Everything is `async`/non-blocking end to end: the DB driver (`aiosqlite` /
`asyncpg`), the MQTT client (`aiomqtt`), the HTTP client (`httpx`), and file
I/O (`aiofiles`) are all async, so one worker process comfortably handles
many concurrent devices and dashboard clients without thread pools.

## 2. Setup

```bash
python -m venv venv
source venv/bin/activate          # Windows: venv\Scripts\activate
pip install -r requirements.txt

cp .env.example .env
# edit .env: set HARDWARE_API_KEY, MQTT_HOST, LINE_NOTIFY_TOKEN, etc.

uvicorn app.main:app --host 0.0.0.0 --port 8000 --reload
```

Interactive API docs: `http://localhost:8000/docs`
Health check: `GET /health` (no auth)

By default `DATABASE_URL` points at local SQLite (`aiosqlite`) for zero-setup
development. Switch to `postgresql+asyncpg://...` for production — the code
is driver-agnostic since it only uses SQLAlchemy's async API.

### Production notes
- Replace `init_db()` (which calls `create_all`) with Alembic migrations
  (`alembic init migrations`) before your schema needs to evolve safely.
- Run behind a reverse proxy (nginx/Caddy) terminating TLS; `wss://` for the
  dashboard WebSocket, `https://` for REST.
- Run with multiple Uvicorn/Gunicorn workers **only if** you move the MQTT
  bridge and WebSocket manager to a shared broker-backed pub/sub (e.g. Redis)
  — as written, the MQTT connection and WS client set are per-process.
- Rotate `HARDWARE_API_KEY` per device by moving from a single shared secret
  to a `device_api_keys` table validated in `security.py` — the dependency
  interface (`Depends(verify_hardware_api_key)`) doesn't need to change.

## 3. Database Layer

Two tables, both async SQLAlchemy 2.0 ORM models (`app/models.py`):

- **`parcels_whitelist`** — authorized QR codes/couriers. Indexed on
  `(qr_code, is_active)` for the hot-path lookup during verification.
  Supports soft-deactivation (`is_active`) and expiry (`expires_at`).
- **`access_logs`** — append-only audit trail of every scan, image capture,
  unlock command, and hardware event. Indexed on `(device_id, created_at)`
  for the dashboard's per-device timeline, and on `event_type` for filtering.
  Foreign-keyed to the whitelist entry that granted access (nullable, so
  denied/unmatched attempts are still logged).

## 4. Real-Time Communication

- **MQTT** (`app/mqtt_client.py`): a single background task, started in
  FastAPI's `lifespan`, subscribes to `parcelbox/+/status` and
  `parcelbox/+/event`, auto-reconnects with exponential backoff, and exposes
  `publish_command(device_id, payload)` for the REST layer to send unlock
  commands. Inbound messages are relayed live to the dashboard.
- **WebSocket** (`app/routers/ws.py` + `websocket_manager.py`): dashboard
  clients connect to `ws://.../ws/dashboard` and receive every MQTT event,
  QR verification result, image capture, and manual unlock as JSON, in
  real time. Dead connections are pruned automatically on send failure.

## 5. REST API Summary

All `/api/*` endpoints below require header `X-API-Key: <HARDWARE_API_KEY>`.

| Method | Path                    | Purpose                                      |
|--------|-------------------------|-----------------------------------------------|
| POST   | `/api/verify-qr`        | ESP32 sends scanned QR for validation        |
| POST   | `/api/upload-image`     | ESP32-S3 uploads a captured image            |
| POST   | `/api/hardware/unlock`  | Manually trigger unlock via MQTT             |
| GET    | `/api/hardware/logs`    | Query recent access logs                     |
| WS     | `/ws/dashboard`         | Live event stream (no API key — add session auth before public exposure) |
| GET    | `/health`               | Liveness probe (no API key)                  |

`POST /api/upload-image` validates both the declared `Content-Type` and the
file's actual magic bytes, generates a server-side UUID filename (never
trusts client-supplied names), and enforces `MAX_UPLOAD_SIZE_MB` mid-stream.

## 6. Security

- **API key auth** (`app/security.py`): `Depends(verify_hardware_api_key)`
  on every hardware-facing router, using `hmac.compare_digest` for
  constant-time comparison.
- **Centralized error handling** (`app/exceptions.py`): domain errors
  (`QRCodeExpiredError`, `FileTooLargeError`, `MQTTPublishError`, ...) map to
  correct HTTP status codes; a catch-all handler ensures no stack trace or
  internal detail ever leaks to a client. Every error response has the
  consistent shape `{"error": "<code>", "detail": "<message>"}`.
- **File upload safety**: magic-byte sniffing, server-generated filenames,
  per-device subdirectories, streamed writes with a hard size cap.

## 7. Firmware ↔ Server Contract (quick reference)

**ESP32 main controller**, after a GM66 scan:
```
POST /api/verify-qr
X-API-Key: <key>
{ "device_id": "box-01", "qr_code": "<scanned payload>" }
```
On `granted: true`, the server also publishes an MQTT unlock command to
`parcelbox/box-01/command` — the ESP32 can act on either the HTTP response
or the MQTT message, whichever arrives first, for lower latency.

**ESP32 main controller**, periodic/event MQTT publish:
```
Topic: parcelbox/box-01/status   (or .../event)
Payload: { "device_id": "box-01", "battery": 87, "door": "closed" }
```

**LILYGO ESP32-S3 vision node**, after capturing an image:
```
POST /api/upload-image   (multipart/form-data)
X-API-Key: <key>
device_id=box-01
file=<jpeg/png bytes>
```
