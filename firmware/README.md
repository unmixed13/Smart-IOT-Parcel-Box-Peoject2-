# Firmware ↔ server (v2.1)

Two boards, two device keys, one box.

| Board | Sketch | Device in the server |
|---|---|---|
| ESP32 DevKit (GM66, lock, door switch, button, buzzer) | `parcel_box_main` | `{"device_id":"box-01"}` |
| ESP32-CAM (AI-Thinker) | `parcel_box_vision_cam` | `{"device_id":"box-01-cam","box_id":"box-01"}` |

## Setup
1. Server running (`python run_server.py`, port 8888). Open `http://<server>:8888/docs`, **Authorize** with the admin key
   (`HARDWARE_API_KEY`), then `POST /api/admin/devices` once per board. Copy each `api_key` - it is shown only once.
2. In each sketch folder copy `config.example.h` to `config.h` and fill in Wi-Fi, `SERVER_HOST` (the PC's LAN IP) and `DEVICE_API_KEY`.
3. Arduino IDE: main = "ESP32 Dev Module" (+ ArduinoJson 7); camera = "AI Thinker ESP32-CAM", partition "Huge APP".
4. PC-side logic tests (no hardware): `bash tests/run.sh`.

## Calls (all send `X-API-Key: <that board's key>`)
| Board | Call | When |
|---|---|---|
| main | `POST /api/device/verify-qr` `{"qr_code"}` | GM66 scanned a UUID. Reply: `granted`, `event_id`, `unlock_seconds`, `door_open_alert_seconds` |
| main | `POST /api/device/event` `{"type","event_id"}` | `door_opened`, `unlock_timeout`, `locked`, `door_ajar` (retried from a bounded queue) |
| main | `POST /api/device/event` `{"type":"tamper"}` | door opened while locked |
| main | `POST /api/device/event` `{"type":"manual_unlock"}` | push button (no photo) |
| main, cam | `POST /api/device/heartbeat` | every 30 s; dashboard shows online + door |
| cam | `GET /api/device/pending-capture` | after the trigger pulse: `{"pending":true,"event_id","seconds_left"}` |
| cam | `POST /api/upload-image/raw?event_id=` | raw `image/jpeg` body; 201 on success |

Sequence per delivery: scan -> `verify-qr` (coil on 10 s) -> `door_opened` -> door closed: `locked` + trigger pulse ->
camera polls `pending-capture` -> uploads photo -> owner gets the LINE message. Times (15 s / 60 s / 30 s) are enforced by
the server as well, so a board that dies mid-sequence still ends in a safe state.
