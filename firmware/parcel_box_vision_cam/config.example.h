#pragma once

// Copy this file to config.h (gitignored) and fill in real values.

// ============================ EDIT THESE ====================================
#define WIFI_SSID       "your-wifi-ssid"
#define WIFI_PASSWORD   "your-wifi-password"

// Backend server (the FastAPI box you set up on Windows)
#define SERVER_HOST     "192.168.1.100"   // LAN IP of the PC running the server
#define SERVER_PORT     8000
#define UPLOAD_PATH     "/api/upload-image"
#define HARDWARE_API_KEY "change_me"  // must match HARDWARE_API_KEY in .env

#define DEVICE_ID       "lilygo-01"   // becomes the upload folder name — letters/digits/_/- only
// ============================================================================

// ESP32 main controller sends a pulse on its CAM_TRIG pin (GPIO14) into this
// pin when the door closes, telling this board to take a photo.
#define PIN_TRIGGER_IN     13
#define TRIGGER_COOLDOWN_MS 3000

// Onboard white flash LED (AI-Thinker ESP32-CAM)
#define FLASH_LED_PIN       4

// AI-Thinker ESP32-CAM pin map (standard — do not change unless your board
// is a different ESP32-CAM variant)
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
