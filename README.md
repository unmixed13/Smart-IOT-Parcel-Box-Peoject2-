# Smart IoT Parcel Box — Backend

FastAPI + MySQL + LINE Messaging API backend for the thesis "Design and
Development of a Smart IoT Parcel Box" (ESP32 + GM66 + solenoid, ESP32-CAM).

## Flow (thesis §3.2.1)

1. **Create QR** — the owner opens the LIFF page (`/liff`); the server creates a
   random-UUID QR (valid 24 h, error-correction M) in state `pending`.
2. **Scan** — ESP32 `POST /api/device/verify-qr`. Valid → `unlocking`, reply has
   `event_id` and `unlock_seconds` (10). Re-scan of the same code returns the same
   `event_id` and restarts the timer. Invalid/expired/used → `granted:false`
   (buzzer on the ESP32, alert to the admin on LINE).
3. **Door events** — ESP32 `POST /api/device/event` with `type`:
   `door_opened` (unlocking→used), `unlock_timeout` (unlocking→pending; rejected
   if already used), `locked` (door closed + locked → starts the 30 s photo wait),
   `door_ajar`, `manual_unlock` (button; logged + LINE, no photo).
4. **Photo** — ESP32-CAM polls `GET /api/device/pending-capture`, then
   `POST /api/upload-image` (multipart: `file`, `event_id`). The owner gets a LINE
   message with the photo (public URL via Tailscale Funnel → `PUBLIC_BASE_URL`);
   after 30 s without a photo the message is sent without it.
5. **Timers** (background sweeper, 1 s): `unlocking` > 15 s with no report → `used`
   (fail-secure); `pending` > 24 h → `expired`; door open > 60 s → buzzer/LINE once.

States: `pending → unlocking → used`, `pending → expired`, `unlocking → pending`.

## Setup

```bash
python -m venv venv && source venv/bin/activate   # Windows: venv\Scripts\activate
pip install -r requirements.txt
cp .env.example .env                              # fill in the values
python run_server.py                              # Windows-safe launcher (NSSM)
```
Docs: `/docs` · Health: `/health` · Dashboard: `/dashboard` · LIFF page: `/liff`
Tables are created on startup (retries until MySQL is reachable). Old
`parcels_whitelist` is no longer used and can be dropped.

### First-time provisioning (admin key in `X-API-Key`)

```bash
# one device = one key (shown once). The camera shares the box via box_id.
POST /api/admin/devices   {"device_id":"box-01"}
POST /api/admin/devices   {"device_id":"box-01-cam","box_id":"box-01"}
# link an owner (userId appears in the log when they message the bot)
POST /api/admin/bindings  {"line_user_id":"U…","box_id":"box-01"}
```
Use the **Authorize** button in `/docs` — all secured routes declare the key.

## API

| Auth | Method & path | Purpose |
|---|---|---|
| device key | `POST /api/device/verify-qr` `{qr_code}` | scan → unlock decision |
| device key | `POST /api/device/event` `{type,event_id?}` | `door_opened`, `unlock_timeout`, `locked`, `door_ajar`, `manual_unlock`, `tamper` |
| device key | `GET /api/device/pending-capture` | camera: is a photo wanted? |
| device key | `POST /api/upload-image` (multipart) / `POST /api/upload-image/raw?event_id=` (raw JPEG, used by the ESP32-CAM) | camera photo |
| device key | `POST /api/device/heartbeat` | board status for the dashboard |
| LIFF ID token | `GET /api/liff/me`, `GET/POST /api/liff/qr` | owner's boxes / QR codes |
| admin key | `/api/admin/devices`, `/api/admin/bindings` | provisioning |
| admin key | `POST /api/hardware/unlock`, `GET /api/hardware/logs` | operator |
| signature | `POST /line-webhook` | `X-Line-Signature` verified with the channel secret |
| token | `WS /ws/dashboard?token=` | live feed |

## Notes
- Run a single worker: the sweeper, MQTT bridge and WebSocket set are per-process.
- MQTT is an optional extra unlock path; the HTTP reply is authoritative.
- Tests: `pip install -r requirements-dev.txt && pytest`.
- Never commit `.env`, `*.db` or logs (see `.gitignore`).
- Firmware for both boards lives in `firmware/` (see `firmware/README.md`).

## Project limitations and suggestions for further work

### Limitations
1. **Server hosting.** The prototype server runs on a personal computer (FastAPI + MariaDB/XAMPP) and is exposed through Tailscale Funnel. The system is available only while that computer is on and connected; it is not a production deployment.
2. **Device link uses plain HTTP.** The ESP32 and ESP32-CAM firmware call the server over HTTP with a per-device API key. Traffic between the devices and the server is therefore not encrypted unless the server is reached over a trusted network or the firmware is changed to HTTPS.
3. **Local image storage.** Photos are stored on the server's local disk (`uploads/`) and served from `/media`. There is no external object storage, retention policy or backup in the prototype.
4. **Single box, single recipient.** The prototype is built and tested around one box (`box-01`) with one camera (`box-01-cam`). Multi-box and multi-tenant operation exists in the data model but has not been tested at scale.
5. **Dependence on Wi-Fi and the Internet.** The box needs Wi-Fi to verify QR codes. If the network or server is down, the lock stays locked (fail-secure) and no parcel can be received.
6. **Hardware.** The lock is switched by a single MOSFET driver from one 12 V supply; there is no battery backup, so a power cut leaves the box locked. The reed switch and button are the only door/override inputs, and the tamper detection depends on them.
7. **Camera.** The ESP32-CAM takes one photo after the door closes; image quality depends on light and the flash, and the photo is not guaranteed if Wi-Fi is weak within the 30-second wait.
8. **Test coverage.** Automated tests cover the server workflow and the firmware control logic on a PC. Long-duration, multi-user and security (penetration) testing was not performed.

### Suggestions for further work
1. Deploy the server to a cloud VPS or PaaS with a domain name and HTTPS, run it as a managed service, and add monitoring and automatic restart.
2. Move the devices to HTTPS (TLS) and consider certificate pinning and key rotation for device API keys.
3. Store images in object storage with a retention period; add database and image backups.
4. Add a backup battery / UPS for the lock and controller, and a mechanical key or emergency release.
5. Add more sensors (parcel presence/weight, door-lock state feedback, vibration for tamper detection) and a second camera angle.
6. Support multiple boxes and users with an admin interface for registering boxes and rotating device keys.
7. Add rate limiting and audit alerts on the public API, and perform a security review.
8. Evaluate the system with user trials (delivery and recipient time, success rate, notification delay) and report measured results.
