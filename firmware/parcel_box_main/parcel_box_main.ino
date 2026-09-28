/*
  Smart Parcel Box — Main Controller (ESP32 Dev Board)
  ============================================================================
  Owns every physical input/output on the box (Table 3.1):
    - GM66 QR scanner (UART2)      -> verify against backend, unlock on match
    - Solenoid lock via MOSFET     -> energize on unlock, auto-relock on timer
    - Reed switch (door sensor)    -> detect close, trigger vision-node photo
    - Push button (manual/fail-safe unlock)
    - Buzzer (non-blocking beeps for feedback)
    - Trigger pulse to ESP32-CAM vision node on door close

  Talks to the backend two ways:
    - HTTP POST /api/verify-qr     (scan result -> granted/denied)
    - MQTT status/event/command    (dashboard live view + remote unlock)

  Libraries needed (Library Manager):
    - PubSubClient (Nick O'Leary)
    - ArduinoJson (v7.x, Benoit Blanchon)

  Flow: scan -> verify -> unlock -> (wait) -> lock -> capture -> notify
  ============================================================================
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
HardwareSerial ScannerSerial(2); // UART2: RX=PIN_RX_SCAN, TX=PIN_TX_SCAN

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
enum class LockState { LOCKED, UNLOCKING, UNLOCKED };
LockState lockState = LockState::LOCKED;
unsigned long unlockStartMs = 0;

bool doorClosed = true;           // reed switch: LOW (INPUT_PULLUP) = closed
bool lastDoorClosed = true;
unsigned long lastDoorChangeMs = 0;

bool lastButtonPressed = false;
unsigned long lastButtonChangeMs = 0;

unsigned long lastStatusPublishMs = 0;
unsigned long lastMqttReconnectAttempt = 0;

// Non-blocking buzzer: a queue of (on_ms, off_ms) pairs isn't needed for our
// simple beep patterns — we just track when to end the current buzz.
bool buzzerActive = false;
unsigned long buzzerEndMs = 0;
int buzzerBeepsRemaining = 0;
unsigned long buzzerNextEdgeMs = 0;
bool buzzerPinHigh = false;

String qrLineBuffer;

// ---------------------------------------------------------------------------
// Buzzer (non-blocking): call beep(n) to queue n short beeps
// ---------------------------------------------------------------------------
void beep(int times) {
  buzzerBeepsRemaining = times;
  buzzerNextEdgeMs = millis();
  buzzerPinHigh = false;
}

void serviceBuzzer() {
  if (buzzerBeepsRemaining <= 0) return;
  unsigned long now = millis();
  if (now < buzzerNextEdgeMs) return;

  if (!buzzerPinHigh) {
    digitalWrite(PIN_BUZZ_SIG, HIGH);
    buzzerPinHigh = true;
    buzzerNextEdgeMs = now + 100; // beep on-time
  } else {
    digitalWrite(PIN_BUZZ_SIG, LOW);
    buzzerPinHigh = false;
    buzzerBeepsRemaining--;
    buzzerNextEdgeMs = now + 100; // gap between beeps
  }
}

// ---------------------------------------------------------------------------
// WiFi / MQTT
// ---------------------------------------------------------------------------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) { delay(300); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("[WiFi] OK  IP=%s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("[WiFi] Failed, will retry");
}

void publishEvent(const char *eventType, const char *notes) {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["event"] = eventType;
  doc["notes"] = notes;
  char buf[256];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(TOPIC_EVENT, (uint8_t *)buf, n);
}

void publishStatus() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["door"] = doorClosed ? "closed" : "open";
  doc["lock"] = lockState == LockState::LOCKED ? "locked" : "unlocked";
  doc["uptime_s"] = millis() / 1000;
  char buf[256];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(TOPIC_STATUS, (uint8_t *)buf, n);
}

void doUnlock(const char *reason); // fwd decl

void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) { Serial.println("[MQTT] Bad command payload"); return; }

  const char *action = doc["action"] | "";
  if (strcmp(action, "unlock") == 0) {
    const char *reason = doc["reason"] | "mqtt_command";
    Serial.printf("[MQTT] Remote unlock command (%s)\n", reason);
    doUnlock(reason);
  }
}

void connectMqtt() {
  if (mqtt.connected()) return;
  unsigned long now = millis();
  if (now - lastMqttReconnectAttempt < 3000) return; // backoff
  lastMqttReconnectAttempt = now;

  Serial.print("[MQTT] Connecting...");
  String clientId = String("esp32-") + DEVICE_ID;
  if (mqtt.connect(clientId.c_str())) {
    Serial.println(" connected");
    mqtt.subscribe(TOPIC_COMMAND);
    publishStatus();
  } else {
    Serial.printf(" failed, rc=%d\n", mqtt.state());
  }
}

// ---------------------------------------------------------------------------
// Backend HTTP: verify a scanned QR code
//
// Server response shape (see app/schemas.py QRVerifyResponse, and README
// section 7 "Firmware <-> Server Contract"):
//   { "granted": true|false, "reason": "...", "courier_name": "..." }
// There is no "result" field — read the "granted" boolean directly.
// ---------------------------------------------------------------------------
bool verifyQrWithServer(const String &qr, bool &granted) {
  HTTPClient http;
  String url = String("http://") + SERVER_HOST + ":" + String(SERVER_PORT) + "/api/verify-qr";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", HARDWARE_API_KEY);
  http.setTimeout(8000);

  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["qr_code"] = qr;
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[HTTP] verify-qr -> %d\n", code);
    http.end();
    return false;
  }

  JsonDocument resp;
  DeserializationError err = deserializeJson(resp, http.getString());
  http.end();
  if (err) return false;

  granted = resp["granted"] | false;
  return true;
}

// ---------------------------------------------------------------------------
// GM66 QR scanner: read newline-terminated codes off UART2
// ---------------------------------------------------------------------------
void pollScanner() {
  while (ScannerSerial.available()) {
    char c = ScannerSerial.read();
    if (c == '\n' || c == '\r') {
      if (qrLineBuffer.length() > 0) {
        String code = qrLineBuffer;
        qrLineBuffer = "";
        code.trim();
        Serial.printf("[SCAN] %s\n", code.c_str());

        bool granted = false;
        bool ok = verifyQrWithServer(code, granted);
        if (!ok) {
          beep(3); // error pattern
          publishEvent("qr_scan", "verify_failed_network_error");
        } else if (granted) {
          beep(1);
          publishEvent("qr_scan", "granted");
          doUnlock("qr_match");
        } else {
          beep(2); // denied pattern
          publishEvent("qr_scan", "denied");
        }
      }
    } else {
      qrLineBuffer += c;
      if (qrLineBuffer.length() > 128) qrLineBuffer = ""; // guard against garbage
    }
  }
}

// ---------------------------------------------------------------------------
// Lock control
// ---------------------------------------------------------------------------
void doUnlock(const char *reason) {
  if (lockState == LockState::UNLOCKED || lockState == LockState::UNLOCKING) return;
  Serial.printf("[LOCK] Unlocking (%s)\n", reason);
  digitalWrite(PIN_LOCK_SIG, HIGH);
  lockState = LockState::UNLOCKED;
  unlockStartMs = millis();
  publishStatus();
}

void doLock() {
  if (lockState == LockState::LOCKED) return;
  Serial.println("[LOCK] Re-locking");
  digitalWrite(PIN_LOCK_SIG, LOW);
  lockState = LockState::LOCKED;
  publishStatus();
}

void serviceLockTimer() {
  if (lockState != LockState::UNLOCKED) return;
  unsigned long elapsed = millis() - unlockStartMs;
  // Relock either after the normal hold time, or the hard safety timeout,
  // whichever the door-close logic hasn't already triggered.
  if (elapsed >= UNLOCK_SAFETY_TIMEOUT) {
    Serial.println("[LOCK] Safety timeout reached");
    doLock();
  }
}

// ---------------------------------------------------------------------------
// Trigger the vision node (ESP32-CAM) to take a photo
// ---------------------------------------------------------------------------
void triggerCamera() {
  digitalWrite(PIN_CAM_TRIG, HIGH);
  delay(CAM_TRIGGER_PULSE_MS); // short blocking pulse is fine, it's only 200ms
  digitalWrite(PIN_CAM_TRIG, LOW);
  Serial.println("[CAM] Trigger pulse sent to vision node");
}

// ---------------------------------------------------------------------------
// Door sensor (debounced)
// ---------------------------------------------------------------------------
void pollDoorSwitch() {
  static bool lastRaw = true;
  bool rawClosed = digitalRead(PIN_DOOR_SW) == LOW; // INPUT_PULLUP: closed = LOW
  unsigned long now = millis();

  if (rawClosed != lastRaw) {
    lastDoorChangeMs = now;
    lastRaw = rawClosed;
  }
  if (now - lastDoorChangeMs >= DOOR_DEBOUNCE_MS && rawClosed != doorClosed) {
    doorClosed = rawClosed;
    Serial.printf("[DOOR] %s\n", doorClosed ? "closed" : "open");
    publishEvent("door", doorClosed ? "closed" : "opened");
    publishStatus();

    if (doorClosed) {
      // Door just closed: this is our cue to auto-relock (if still unlocked)
      // and capture a photo of whatever is inside as evidence.
      if (lockState != LockState::LOCKED) doLock();
      triggerCamera();
    }
  }
}

// ---------------------------------------------------------------------------
// Manual push button (debounced) — fail-safe unlock, e.g. from inside
// ---------------------------------------------------------------------------
void pollButton() {
  static bool lastRaw = false;
  bool rawPressed = digitalRead(PIN_UNLOCK_BTN_SIG) == LOW; // INPUT_PULLUP
  unsigned long now = millis();
  if (rawPressed != lastRaw) {
    lastButtonChangeMs = now;
    lastRaw = rawPressed;
  }
  if (now - lastButtonChangeMs >= BUTTON_DEBOUNCE_MS && rawPressed != lastButtonPressed) {
    lastButtonPressed = rawPressed;
    if (rawPressed) {
      Serial.println("[BUTTON] Manual unlock pressed");
      publishEvent("manual_button", "pressed");
      doUnlock("manual_button");
    }
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Smart Parcel Box - Main Controller ===");

  pinMode(PIN_BUZZ_SIG, OUTPUT);       digitalWrite(PIN_BUZZ_SIG, LOW);
  pinMode(PIN_CAM_TRIG, OUTPUT);       digitalWrite(PIN_CAM_TRIG, LOW);
  pinMode(PIN_LOCK_SIG, OUTPUT);       digitalWrite(PIN_LOCK_SIG, LOW);
  pinMode(PIN_DOOR_SW, INPUT_PULLUP);
  pinMode(PIN_UNLOCK_BTN_SIG, INPUT_PULLUP);

  ScannerSerial.begin(9600, SERIAL_8N1, PIN_RX_SCAN, PIN_TX_SCAN);

  connectWiFi();
  mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  mqtt.setCallback(onMqttMessage);

  doorClosed = digitalRead(PIN_DOOR_SW) == LOW;
  beep(1);
  Serial.println("[SYS] Ready");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  connectMqtt();
  mqtt.loop();

  pollScanner();
  pollDoorSwitch();
  pollButton();
  serviceLockTimer();
  serviceBuzzer();

  if (millis() - lastStatusPublishMs >= STATUS_PUBLISH_MS) {
    lastStatusPublishMs = millis();
    publishStatus();
  }
}
