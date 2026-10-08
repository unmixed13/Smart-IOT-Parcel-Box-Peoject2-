// Copy this file to config.h and fill in your values. config.h is git-ignored.
#pragma once

#define WIFI_SSID        "your-wifi-name"
#define WIFI_PASSWORD    "your-wifi-password"

#define SERVER_HOST      "192.168.1.50"   // same LAN address as the main board uses
#define SERVER_PORT      8888            // run_server.py port

// Create it in http://<server>:8888/docs -> Authorize (admin key) -> POST /api/admin/devices
//   {"device_id":"box-01-cam","box_id":"box-01"}   -> copy "api_key" (shown ONCE)
// "box_id" ties the camera to the main board's box so it sees that box's pending photo.
// This is the CAMERA's own key; it is not the main board's key.
#define DEVICE_API_KEY   "paste-the-api_key-from-the-server-here"

// Trigger input from the main board's GPIO14 (CAM_TRIG).
// GPIO13 is free on the AI-Thinker board and is not a boot-strapping pin.
// (This resolves the "[ระบุหมายเลขขา GPIO ...]" placeholder in thesis section 3.2.2 (8).)
#define PIN_TRIGGER_IN      13
#define FLASH_LED_PIN        4

// Timing
#define TRIGGER_COOLDOWN_MS   3000   // ignore re-triggers right after a capture
#define PENDING_POLL_WINDOW_MS 12000 // how long to wait for the server to know about the event
#define PENDING_POLL_EVERY_MS   500

// AI-Thinker ESP32-CAM pin map (standard)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// Image orientation (the module is usually mounted so the picture comes out upside down)
#define CAM_VFLIP   1   // 1 = flip vertically, 0 = off
#define CAM_HMIRROR 0   // 1 = mirror left-right
