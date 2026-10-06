/*
  Smart Parcel Box - Main Controller (ESP32 DevKit, 38 pin)
  ============================================================================
  Thin hardware layer around box_logic.h (all decisions live there and are
  unit-tested on a PC). This file provides:

    * EspHal       - pins, millis, buzzer, coil, camera trigger
    * scanner      - GM66 on UART2, one code per line (or idle-flushed)
    * network task - runs on core 0 so Wi-Fi/HTTP can NEVER delay the lock
                     timers or the door logic running in loop() on core 1:
                       - POST /api/device/verify-qr   (3 s limit, thesis 3.2.1 (1))
                       - POST /api/device/event       (door_opened / unlock_timeout / locked /
                                                       door_ajar / tamper; retried, bounded queue)
                       - POST /api/device/event       {"type":"manual_unlock"} (push-button override)
                       - POST /api/device/heartbeat   (every 30 s; shows online + door on the dashboard)
    * watchdog     - hardware task watchdog + network-task liveness check

  The solenoid is released by hardware on any reset (10k pull-down on the gate),
  so a crash, brown-out or watchdog reboot always ends in "locked".

  Libraries (Library Manager): ArduinoJson 7.x.   (No MQTT: the thesis uses HTTP.)
  Board: "ESP32 Dev Module".  Copy config.example.h to config.h first.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy config.example.h to config.h and edit it (Wi-Fi, server address, device API key)."
#endif
#include "box_logic.h"

#define FW_VERSION "2.1.0"

// ---------------------------------------------------------------------------
// Hardware abstraction
// ---------------------------------------------------------------------------
class EspHal : public pb::Hal {
 public:
  uint32_t millis() override { return ::millis(); }
  void coil(bool on) override { digitalWrite(PIN_LOCK_SIG, on ? HIGH : LOW); }
  void buzzer(bool on) override { digitalWrite(PIN_BUZZ_SIG, on ? HIGH : LOW); }
  void camPin(bool high) override { digitalWrite(PIN_CAM_TRIG, high ? HIGH : LOW); }
  bool doorClosedRaw() override { return digitalRead(PIN_DOOR_SW) == LOW; }
  bool buttonPressedRaw() override { return digitalRead(PIN_UNLOCK_BTN_SIG) == LOW; }
  void log(const char *msg) override { Serial.printf("[%lu] %s\n", (unsigned long)::millis(), msg); }
};

// ---------------------------------------------------------------------------
// Cross-task plumbing
// ---------------------------------------------------------------------------
struct VerifyReq { char code[pb::kIdLen]; };
struct VerifyRes {
  bool reached;
  bool granted;
  char eventId[pb::kIdLen];
  uint16_t unlockSec;
  uint16_t alertSec;
};
struct ManualRes { char eventId[pb::kIdLen]; };

static QueueHandle_t verifyReqQ, verifyResQ, manualResQ;
static SemaphoreHandle_t reportMutex;
static pb::ReportQueue reportQueue;                 // guarded by reportMutex
static volatile bool manualPending = false;
static volatile uint32_t manualRequestedAt = 0;
static volatile uint32_t netAliveMs = 0;
static volatile bool snapDoorClosed = true, snapLocked = true;   // published by loop() for the heartbeat

class EspNet : public pb::Net {
 public:
  void requestVerify(const char *code) override {
    VerifyReq r;
    strncpy(r.code, code, sizeof(r.code) - 1);
    r.code[sizeof(r.code) - 1] = 0;
    xQueueOverwrite(verifyReqQ, &r);
  }
  void queueReport(pb::ReportType t, const char *eventId, uint32_t validMs) override {
    if (xSemaphoreTake(reportMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      reportQueue.push(t, eventId, millis(), validMs);
      xSemaphoreGive(reportMutex);
    }
  }
  void queueManual() override { manualRequestedAt = millis(); manualPending = true; }
};

static EspHal hal;
static EspNet net;
static pb::BoxLogic logic(hal, net);
static HardwareSerial ScannerSerial(2);

// ---------------------------------------------------------------------------
// HTTP (network task only)
// ---------------------------------------------------------------------------
// Returns the HTTP status, or <= 0 on a transport error. Body (if any) is copied
// into resp, truncated to respCap-1 bytes.
static int httpPost(const char *path, const char *body, char *resp, size_t respCap,
                    uint16_t connectMs, uint16_t readMs) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(connectMs);
  http.setTimeout(readMs);
  http.setReuse(false);
  if (!http.begin(client, SERVER_HOST, SERVER_PORT, path)) return -1;
  http.addHeader("X-API-Key", DEVICE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  const char *b = body ? body : "";
  int code = http.POST((uint8_t *)b, strlen(b));
  if (resp && respCap) {
    resp[0] = 0;
    if (code > 0 && http.getSize() >= 0 && http.getSize() < 2048) {
      String s = http.getString();
      strncpy(resp, s.c_str(), respCap - 1);
      resp[respCap - 1] = 0;
    }
  }
  http.end();
  return code;
}

static void doVerify(const VerifyReq &req) {
  char body[96], resp[512];
  snprintf(body, sizeof(body), "{\"qr_code\":\"%s\"}", req.code);
  VerifyRes res = {};
  // 1.2 s connect + 1.5 s read keeps the worst case inside the thesis' 3 s answer limit.
  int code = httpPost("/api/device/verify-qr", body, resp, sizeof(resp), 1200, 1500);
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, resp)) {
      res.reached = true;
      res.granted = doc["granted"] | false;
      const char *ev = doc["event_id"] | "";
      strncpy(res.eventId, ev, sizeof(res.eventId) - 1);
      res.unlockSec = doc["unlock_seconds"] | 0;
      res.alertSec = doc["door_open_alert_seconds"] | 0;
    }
  }
  if (code != 200) Serial.printf("[HTTP] verify-qr -> %d\n", code);
  xQueueOverwrite(verifyResQ, &res);
}

static void doManual() {
  // The server just logs it and tells the owner on LINE; there is no Event-ID for a manual
  // open (and no photo), so nothing comes back that the box needs.
  char resp[160];
  int code = httpPost("/api/device/event", "{\"type\":\"manual_unlock\"}", resp, sizeof(resp), 1500, 2500);
  if (code == 200) {
    manualPending = false;
  } else if (pb::elapsed(millis(), manualRequestedAt, 20000) || (code >= 400 && code < 500 && code != 429)) {
    manualPending = false;  // give up: the unlock itself already happened locally
  }
}

static void doReport() {
  pb::ReportQueue::Item it;
  uint32_t now = millis();
  if (xSemaphoreTake(reportMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  bool have = reportQueue.front(now, it);
  xSemaphoreGive(reportMutex);
  if (!have) return;

  char body[160], resp[128];
  if (it.eventId[0])
    snprintf(body, sizeof(body), "{\"type\":\"%s\",\"event_id\":\"%s\"}", pb::reportName(it.type), it.eventId);
  else
    snprintf(body, sizeof(body), "{\"type\":\"%s\"}", pb::reportName(it.type));
  int code = httpPost("/api/device/event", body, resp, sizeof(resp), 1500, 2500);

  bool final = (code == 200) || (code >= 400 && code < 500 && code != 429);   // 200 (even accepted:false)/401/404/422: nothing to retry
  if (xSemaphoreTake(reportMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (final) reportQueue.done(); else reportQueue.retryIn(millis(), 1000);
    xSemaphoreGive(reportMutex);
  }
  if (code != 200) Serial.printf("[HTTP] report %s -> %d\n", pb::reportName(it.type), code);
}

static void doHeartbeat() {
  char body[200];
  snprintf(body, sizeof(body),
           "{\"door\":\"%s\",\"lock\":\"%s\",\"rssi\":%d,\"uptime_s\":%lu,\"free_heap\":%u,\"fw\":\"%s\"}",
           snapDoorClosed ? "closed" : "open", snapLocked ? "locked" : "unlocked", (int)WiFi.RSSI(),
           (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(), FW_VERSION);
  httpPost("/api/device/heartbeat", body, nullptr, 0, 1500, 2500);
}

static void maintainWiFi() {
  static uint32_t lastTry = 0;
  if (WiFi.status() == WL_CONNECTED) { lastTry = millis(); return; }
  if (pb::elapsed(millis(), lastTry, 15000)) {       // non-blocking: begin() returns immediately
    lastTry = millis();
    Serial.println("[WiFi] reconnecting");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

static void netTask(void *) {
  uint32_t lastHeartbeat = 0;
  for (;;) {
    netAliveMs = millis();
    maintainWiFi();
    VerifyReq req;
    if (xQueueReceive(verifyReqQ, &req, 0) == pdTRUE) {
      if (WiFi.status() == WL_CONNECTED) doVerify(req);
      else { VerifyRes res = {}; xQueueOverwrite(verifyResQ, &res); }   // fail fast: denied + error beeps
    } else if (WiFi.status() == WL_CONNECTED) {
      if (manualPending) doManual();
      else {
        doReport();
        if (pb::elapsed(millis(), lastHeartbeat, 30000)) { lastHeartbeat = millis(); doHeartbeat(); }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------------------------------------------------------------------------
// GM66 scanner (UART2)
// ---------------------------------------------------------------------------
static void pollScanner() {
  static char buf[64];
  static uint8_t len = 0;
  static uint32_t lastByteMs = 0;
  while (ScannerSerial.available()) {
    int c = ScannerSerial.read();
    lastByteMs = millis();
    if (c == '\r' || c == '\n') {
      if (len) { buf[len] = 0; logic.onScan(buf); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = (char)c;
    } else {
      len = 0;                                        // overlong: noise, drop it
    }
  }
  if (len && pb::elapsed(millis(), lastByteMs, 80)) { // scanner configured without a terminator
    buf[len] = 0;
    logic.onScan(buf);
    len = 0;
  }
}

static void watchdogBegin(uint32_t seconds) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = seconds * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&cfg);
#else
  esp_task_wdt_init(seconds, true);
#endif
  esp_task_wdt_add(NULL);
}

// ---------------------------------------------------------------------------
void setup() {
  // Coil first: drive it LOW before anything else (the pull-down already holds it off).
  pinMode(PIN_LOCK_SIG, OUTPUT);   digitalWrite(PIN_LOCK_SIG, LOW);
  pinMode(PIN_BUZZ_SIG, OUTPUT);   digitalWrite(PIN_BUZZ_SIG, LOW);
  pinMode(PIN_CAM_TRIG, OUTPUT);   digitalWrite(PIN_CAM_TRIG, LOW);
  pinMode(PIN_DOOR_SW, INPUT_PULLUP);
  pinMode(PIN_UNLOCK_BTN_SIG, INPUT_PULLUP);

  Serial.begin(115200);
  Serial.println("\n=== Smart Parcel Box - Main Controller v" FW_VERSION " ===");
  ScannerSerial.begin(SCANNER_BAUD, SERIAL_8N1, PIN_RX_SCAN, PIN_TX_SCAN);

  verifyReqQ = xQueueCreate(1, sizeof(VerifyReq));
  verifyResQ = xQueueCreate(1, sizeof(VerifyRes));
  manualResQ = xQueueCreate(1, sizeof(ManualRes));
  reportMutex = xSemaphoreCreateMutex();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                // lowest latency for the 3 s verification window
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // non-blocking: the box works (button, door logic) without Wi-Fi

  netAliveMs = millis();
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);

  logic.begin();
  watchdogBegin(8);
}

void loop() {
  esp_task_wdt_reset();

  pollScanner();
  logic.loop();

  VerifyRes vr;
  if (xQueueReceive(verifyResQ, &vr, 0) == pdTRUE)
    logic.onVerifyResult(vr.reached, vr.granted, vr.eventId, vr.unlockSec, vr.alertSec);
  ManualRes mr;
  if (xQueueReceive(manualResQ, &mr, 0) == pdTRUE) logic.onManualEventId(mr.eventId);

  snapDoorClosed = logic.doorClosed();
  snapLocked = !logic.coilOn();

  // If the network task ever stalls, reboot - but only while idle and locked.
  if (pb::elapsed(millis(), netAliveMs, 45000) && logic.state() == pb::State::Locked && !logic.verifying()) {
    Serial.println("[SYS] network task stalled -> restart");
    delay(100);
    ESP.restart();
  }
  delay(2);
}
