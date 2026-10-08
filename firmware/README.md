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

## ESP32-CAM bring-up test (do these in order)
Wiring: 5 V (>= 1 A) and GND from the buck, `TRIG` from the main board's GPIO14 to the camera's GPIO13, common GND.
Flashing: needs a USB-UART adapter (or the ESP32-CAM-MB base), GPIO0 to GND while uploading, remove it and press RST to run.
Register the camera first: `POST /api/admin/devices {"device_id":"box-01-cam","box_id":"box-01"}`, put its key in the camera's `config.h`.

| # | Test | How | Pass |
|---|---|---|---|
| C1 | Camera + PSRAM | open Serial Monitor 115200 after RST | `[SYS] PSRAM found`, `[CAM] OV2640 ready` |
| C2 | Wi-Fi + server + key | type `p` | `[BENCH] captured ... bytes`, `[HTTP] upload ... -> 201`, `[BENCH] OK`; dashboard shows `CAPTURE`; Access Log `Image captured and stored` |
| C3 | Image quality | open the file under `uploads\vision_captures\box-01-cam\` | sharp, correct colours (green/pink = weak 5 V supply) |
| C4 | Trigger line | main board: scan a valid QR, open and close the door | camera Serial shows `[TRIGGER] GPIO trigger` after the door closes |
| C5 | Full flow | same as C4 | `[CAM] event ... waiting`, `upload -> 201`, LINE gets "A parcel was delivered" **with the photo**; Access Log note `Delivery photo for event ...` |
| C6 | No-photo path | disconnect the camera's 5 V, repeat the delivery | after `IMAGE_WAIT_SECONDS` LINE gets the text-only message |
| C7 | Spurious trigger | touch the TRIG wire to 3V3 with no delivery pending | `no event waiting for a photo - nothing to do`, no photo |

Typical failures: `upload -> 401` wrong key (or the main board's key), `-> 415` not a JPEG, `capture failed` / reboots = 5 V supply too weak,
`[HTTP] pending -> 401` camera registered without the right `box_id`.

### Stacked ESP32-CAM-MB base: no breakout
With the module plugged into the MB base its pins (5V, GND, GPIO13) are hidden, so the TRIG wire cannot be attached.
- **Phase 1 - module on the base (USB):** tests C1-C3, and the delivery-to-LINE path without the wire: simulate the main board in `/docs`
  (verify-qr -> door_opened -> locked) and type `c` in the camera's Serial Monitor.
- **Phase 2 - the wire:** flash the camera on the base first, then unplug the module, power it from 5 V (own supply, not through the main board's USB if it browns out)
  and wire `5V`, `GND`, `IO13` (module header labels). No Serial is needed: the onboard red LED blinks **once at boot** and **twice when a TRIG pulse arrives**;
  a delivery then fires the flash and LINE gets the photo. GPIO0 must NOT be grounded while running.
