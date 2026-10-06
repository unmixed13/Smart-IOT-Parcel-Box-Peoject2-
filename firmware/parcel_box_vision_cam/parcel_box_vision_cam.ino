/*
  Smart Parcel Box - Vision Node (ESP32-CAM, AI-Thinker)
  ============================================================================
  Design document 3.2.1 (3):
    1. The main board locks the door, sends its "locked" report (with the
       Event-ID) to the server, and pulses CAM_TRIG (200 ms HIGH).
    2. This board asks the server which event is waiting for a photo
       (GET /api/device/pending-capture) - it never needs the Event-ID over the wire.
       The trigger can arrive before the report does, so it polls for up to 12 s.
    3. It fires the flash, grabs a JPEG and uploads it as a RAW image/jpeg body to
       POST /api/upload-image/raw?event_id=...  (retries while the 30 s window is open).
    4. If the server never receives a photo it notifies the owner without one.

  A spurious trigger (e.g. GPIO14 of the main ESP32 glitches during its boot) is
  harmless: with no event waiting, /device/pending-capture answers pending:false and no
  photo is taken.

  Wiring: main GPIO14 -> [1k optional] -> GPIO13 here, common GND, 5 V supply
  that can deliver >= 1 A peaks (camera + Wi-Fi + flash; brown-outs show up as
  random resets and green/pink frames).

  Arduino IDE: Board "AI Thinker ESP32-CAM", Partition "Huge APP (3MB No OTA)".
  No extra libraries. Copy config.example.h to config.h first.
*/

#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy config.example.h to config.h and edit it (Wi-Fi, server address, device API key)."
#endif

#define FW_VERSION "2.1.0"

volatile bool triggerPending = false;
static uint32_t lastTriggerMs = 0;
static uint32_t lastHeartbeatMs = 0;
static uint32_t lastWifiTryMs = 0;
static bool cameraReady = false;

static inline bool elapsedMs(uint32_t now, uint32_t since, uint32_t dur) { return (uint32_t)(now - since) >= dur; }

void IRAM_ATTR onTrigger() { triggerPending = true; }

// ---------------------------------------------------------------------------
static bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  // 10 MHz XCLK: 20 MHz gave green/pink casts on some boards (kept from the tested prototype).
  c.xclk_freq_hz = 10000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_XGA;       // 1024x768, ~100 KB
  c.jpeg_quality = 12;
  if (psramFound()) {
    c.fb_location = CAMERA_FB_IN_PSRAM; c.fb_count = 2; c.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    Serial.println("[CAM] No PSRAM - using SVGA");
    c.fb_location = CAMERA_FB_IN_DRAM; c.fb_count = 1; c.frame_size = FRAMESIZE_SVGA;
    c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) { Serial.printf("[CAM] init failed: 0x%x\n", err); return false; }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {                            // modest gain: the flash does the work in a dark box
    s->set_brightness(s, 1);
    s->set_gainceiling(s, GAINCEILING_2X);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
  }
  Serial.println("[CAM] OV2640 ready");
  return true;
}

static void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) { lastWifiTryMs = millis(); return; }
  if (elapsedMs(millis(), lastWifiTryMs, 10000)) {    // non-blocking retry
    lastWifiTryMs = millis();
    Serial.println("[WiFi] reconnecting");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
static int httpGet(const char *pathAndQuery, char *resp, size_t cap) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(3000);
  http.setReuse(false);
  if (!http.begin(client, SERVER_HOST, SERVER_PORT, pathAndQuery)) return -1;
  http.addHeader("X-API-Key", DEVICE_API_KEY);
  int code = http.GET();
  resp[0] = 0;
  if (code > 0 && http.getSize() >= 0 && http.getSize() < 1024) {
    String s = http.getString();
    strncpy(resp, s.c_str(), cap - 1);
    resp[cap - 1] = 0;
  }
  http.end();
  return code;
}

// Parses {"pending":true,"event_id":"<uuid>","seconds_left":N}. When nothing is waiting the server
// answers {"pending":false,"event_id":null,"seconds_left":null}. No JSON library needed.
static bool parsePending(const char *json, char *eventId, int *expiresIn) {
  if (!strstr(json, "\"pending\":true")) return false;
  const char *k = strstr(json, "\"event_id\":\"");
  if (!k) return false;                                   // null or missing: nothing waiting
  k += 12;
  const char *end = strchr(k, '"');
  if (!end || (end - k) != 36) return false;              // must be a UUID
  memcpy(eventId, k, 36);
  eventId[36] = 0;
  *expiresIn = 30;
  const char *e = strstr(json, "\"seconds_left\":");
  if (e) *expiresIn = atoi(e + 15);
  return true;
}

static bool fetchPending(char *eventId, int *expiresIn) {
  char resp[256];
  int code = httpGet("/api/device/pending-capture", resp, sizeof(resp));
  if (code != 200) { if (code != 404) Serial.printf("[HTTP] pending -> %d\n", code); return false; }
  return parsePending(resp, eventId, expiresIn);
}

// Raw JPEG body, exactly what the server's /api/upload-image/raw expects.
static int uploadJpeg(const uint8_t *jpg, size_t len, const char *eventId, char *resp, size_t cap) {
  char path[96];
  snprintf(path, sizeof(path), "/api/upload-image/raw?event_id=%s", eventId);
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(8000);
  http.setReuse(false);
  if (!http.begin(client, SERVER_HOST, SERVER_PORT, path)) return -1;
  http.addHeader("X-API-Key", DEVICE_API_KEY);
  http.addHeader("Content-Type", "image/jpeg");
  uint32_t t0 = millis();
  int code = http.POST((uint8_t *)jpg, len);
  resp[0] = 0;
  if (code > 0 && http.getSize() >= 0 && http.getSize() < 512) {
    String s = http.getString();
    strncpy(resp, s.c_str(), cap - 1);
    resp[cap - 1] = 0;
  }
  http.end();
  Serial.printf("[HTTP] upload %u bytes -> %d in %lu ms\n", (unsigned)len, code, (unsigned long)(millis() - t0));
  return code;
}

static void heartbeat() {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(3000);
  http.setReuse(false);
  if (!http.begin(client, SERVER_HOST, SERVER_PORT, "/api/device/heartbeat")) return;
  http.addHeader("X-API-Key", DEVICE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  char body[128];
  snprintf(body, sizeof(body), "{\"rssi\":%d,\"uptime_s\":%lu,\"free_heap\":%u,\"fw\":\"%s\"}",
           (int)WiFi.RSSI(), (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(), FW_VERSION);
  http.POST((uint8_t *)body, strlen(body));
  http.end();
}

// ---------------------------------------------------------------------------
static camera_fb_t *captureWithFlash() {
  digitalWrite(FLASH_LED_PIN, HIGH);
  delay(150);                                   // let exposure settle under the flash
  camera_fb_t *stale = esp_camera_fb_get();     // the first frame may predate the flash
  if (stale) esp_camera_fb_return(stale);
  delay(50);
  camera_fb_t *fb = esp_camera_fb_get();
  digitalWrite(FLASH_LED_PIN, LOW);
  return fb;
}

static void handleTrigger(const char *source) {
  Serial.printf("\n[TRIGGER] %s\n", source);
  if (!cameraReady) { Serial.println("[CAM] not initialised"); return; }

  // 1. Which event is waiting? (the report from the main board may still be in flight)
  char eventId[40] = "";
  int expiresIn = 0;
  uint32_t started = millis();
  bool found = false;
  while (!elapsedMs(millis(), started, PENDING_POLL_WINDOW_MS)) {
    esp_task_wdt_reset();
    maintainWiFi();
    if (WiFi.status() == WL_CONNECTED && fetchPending(eventId, &expiresIn)) { found = true; break; }
    delay(PENDING_POLL_EVERY_MS);
  }
  if (!found) { Serial.println("[CAM] no event waiting for a photo - nothing to do"); return; }
  uint32_t eventSeen = millis();
  Serial.printf("[CAM] event %s waiting, %d s left\n", eventId, expiresIn);

  // 2. Photo
  camera_fb_t *fb = captureWithFlash();
  if (!fb) { Serial.println("[CAM] capture failed"); return; }
  Serial.printf("[CAM] %u bytes (%ux%u)\n", (unsigned)fb->len, (unsigned)fb->width, (unsigned)fb->height);

  // 3. Upload while the server's window is open (+ small margin: a late photo is
  //    still attached to the history, it just no longer goes into the LINE message).
  uint32_t budgetMs = (uint32_t)(expiresIn > 0 ? expiresIn : 5) * 1000UL + 5000UL;
  bool done = false;
  for (int attempt = 1; attempt <= 4 && !done && !elapsedMs(millis(), eventSeen, budgetMs); attempt++) {
    esp_task_wdt_reset();
    maintainWiFi();
    if (WiFi.status() != WL_CONNECTED) { delay(500); continue; }
    char resp[160];
    int code = uploadJpeg(fb->buf, fb->len, eventId, resp, sizeof(resp));
    if (code == 201) { done = true; break; }
    if (code == 409 || code == 404 || code == 413 || code == 415 || code == 401 || code == 403) {
      Serial.printf("[CAM] server refused the photo (%d): %s\n", code, resp);   // retrying cannot help
      break;
    }
    delay(1000UL * attempt);                                                  // 5xx / timeout: back off
  }
  esp_camera_fb_return(fb);
  Serial.println(done ? "[DONE] photo uploaded" : "[DONE] photo NOT uploaded");
}

// ---------------------------------------------------------------------------
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

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Parcel Box Vision Node v" FW_VERSION " ===");
  Serial.printf("[SYS] PSRAM %s\n", psramFound() ? "found" : "NOT found");

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
  cameraReady = initCamera();

  pinMode(PIN_TRIGGER_IN, INPUT_PULLDOWN);     // floating input must never look like a trigger
  attachInterrupt(digitalPinToInterrupt(PIN_TRIGGER_IN), onTrigger, RISING);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  watchdogBegin(40);   // longest legitimate stretch: 12 s polling + 4 x (8 s upload + back-off), reset between steps
  Serial.println("[SYS] ready - waiting for trigger on GPIO" + String(PIN_TRIGGER_IN) + " (or type 'c' to test)");
}

void loop() {
  esp_task_wdt_reset();
  maintainWiFi();

  if (triggerPending) {
    triggerPending = false;
    if (elapsedMs(millis(), lastTriggerMs, TRIGGER_COOLDOWN_MS)) {
      lastTriggerMs = millis();
      handleTrigger("GPIO trigger");
      triggerPending = false;                  // triggers that arrived while busy are stale
    }
  }

  while (Serial.available()) {                 // bench test without the main board
    char ch = Serial.read();
    if (ch == 'c' || ch == 'C') { lastTriggerMs = millis(); handleTrigger("serial 'c'"); }
  }

  if (WiFi.status() == WL_CONNECTED && elapsedMs(millis(), lastHeartbeatMs, 30000)) {
    lastHeartbeatMs = millis();
    heartbeat();
  }
  delay(10);
}
