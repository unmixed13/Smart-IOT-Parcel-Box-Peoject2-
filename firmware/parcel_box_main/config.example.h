#pragma once

// Copy this file to config.h (gitignored) and fill in real values.

// ============================ EDIT THESE ====================================
#define WIFI_SSID         "your-wifi-ssid"
#define WIFI_PASSWORD     "your-wifi-password"

#define MQTT_BROKER_HOST  "192.168.1.100"   // LAN IP of the PC running Mosquitto
#define MQTT_BROKER_PORT  1883

#define SERVER_HOST       "192.168.1.100"   // LAN IP of the PC running the FastAPI server
#define SERVER_PORT       8000
#define HARDWARE_API_KEY  "change_me"  // must match HARDWARE_API_KEY in the server's .env

#define DEVICE_ID         "box-01"          // must match a device_id you use consistently
// ============================================================================

// GPIO map — see Table 3.1
#define PIN_BUZZ_SIG        13   // Buzzer
#define PIN_CAM_TRIG        14   // Pulse to ESP32-CAM's trigger input
#define PIN_RX_SCAN         16   // GM66 QR scanner TX -> here
#define PIN_TX_SCAN         17   // GM66 QR scanner RX <- here
#define PIN_LOCK_SIG        18   // MOSFET gate (via 220-330R resistor) driving the solenoid
#define PIN_DOOR_SW         25   // Reed switch
#define PIN_UNLOCK_BTN_SIG  32   // Manual push button (fail-safe)

// Timings
#define UNLOCK_HOLD_MS         4000   // how long the solenoid stays energized
#define UNLOCK_SAFETY_TIMEOUT  8000   // hard cutoff even if something goes wrong
#define DOOR_DEBOUNCE_MS       80
#define BUTTON_DEBOUNCE_MS     80
#define CAM_TRIGGER_PULSE_MS   200
#define STATUS_PUBLISH_MS      15000  // periodic MQTT status heartbeat
#define QR_POLL_INTERVAL_MS    50

// MQTT topics
#define TOPIC_STATUS   "parcelbox/" DEVICE_ID "/status"
#define TOPIC_EVENT    "parcelbox/" DEVICE_ID "/event"
#define TOPIC_COMMAND  "parcelbox/" DEVICE_ID "/command"
