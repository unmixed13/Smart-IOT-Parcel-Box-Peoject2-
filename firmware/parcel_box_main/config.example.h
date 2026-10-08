// Copy this file to config.h and fill in your values. config.h is git-ignored:
// it holds the Wi-Fi password and this board's API key - never commit it.
#pragma once

// ---- Network ---------------------------------------------------------------
#define WIFI_SSID        "your-wifi-name"
#define WIFI_PASSWORD    "your-wifi-password"

// LAN address of the PC that runs the FastAPI server (give it a fixed IP / DHCP reservation).
// Plain HTTP on the LAN is fine; the device key is the only secret, so keep it out of git.
// (The Tailscale Funnel URL also reaches /api/device/*, but boards should use the LAN address.)
#define SERVER_HOST      "192.168.1.50"
#define SERVER_PORT      8888            // run_server.py port

// Create it in http://<server>:8888/docs -> Authorize (admin key) -> POST /api/admin/devices
//   {"device_id":"box-01"}        -> copy "api_key" (shown ONCE; rotate-key makes a new one)
// Each board has its OWN key - do not reuse the camera's key here.
#define DEVICE_API_KEY   "paste-the-api_key-from-the-server-here"

// ---- GPIO map: thesis Table 3.1 (ตารางที่ 3.1) ---------------------------------
#define PIN_BUZZ_SIG        13   // 2N7000 gate -> active buzzer (HIGH = on)
#define PIN_CAM_TRIG        14   // -> ESP32-CAM GPIO13 (HIGH pulse = "take the photo now")
#define PIN_RX_SCAN         16   // GM66 TX -> (voltage divider 1k/2k) -> here
#define PIN_TX_SCAN         17   // here -> GM66 RX (3.3 V is enough for TTL high)
#define PIN_LOCK_SIG        18   // IRLZ44N gate via 220 ohm; 10 k pull-down => locked on reset
#define PIN_DOOR_SW         25   // reed switch to GND, internal pull-up (LOW = closed)
#define PIN_UNLOCK_BTN_SIG  32   // push button to GND, internal pull-up (LOW = pressed)

#define SCANNER_BAUD        9600  // GM66 factory default

// Buzzer: 0 = sounds when the pin is HIGH (bare buzzer + 2N7000, or an active-HIGH module)
//         1 = sounds when the pin is LOW  (3-pin module marked "low level trigger")
// A 3-pin module (I/O, VCC, GND) is wired I/O -> GPIO13, VCC -> 5V, GND -> GND; no 2N7000 needed.
#define BUZZER_ACTIVE_LOW   0
