/*
  Smart Parcel Box — Vision Node (ESP32-CAM / AI-Thinker)
  ============================================================================
  Waits for a trigger pulse from the main ESP32 controller (sent when the
  door closes), captures a JPEG, and uploads it to the FastAPI backend's
  /api/upload-image endpoint as multipart/form-data.

  Wiring to the main ESP32 controller (2 wires only):
    ESP32 main GPIO14 (CAM_TRIG) --[1k ohm optional]--> ESP32-CAM GPIO13
    ESP32 main GND                --------------------> ESP32-CAM GND

  Hardware notes for ESP32-CAM (AI-Thinker):
    - No USB port on the board — use a separate USB-to-TTL (FTDI/CP2102)
      adapter wired to 5V, GND, U0R (RX), U0T (TX).
    - To UPLOAD code: jumper GPIO0 to GND, press RESET, upload, then REMOVE
      the jumper and press RESET again to run normally.
    - Onboard white flash LED is on GPIO4 — fired automatically before each
      capture since parcel box interiors are usually dim.

  Arduino IDE settings (Tools):
    Board: "AI Thinker ESP32-CAM"
    Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)"
*/

#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "config.h"

volatile bool triggerPending = false;
unsigned long lastTriggerMs = 0;
bool cameraReady = false;

void IRAM_ATTR onTrigger() { triggerPending = true; }

// ---------------------------------------------------------------------------
bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  // 20MHz XCLK can cause signal-integrity issues on some ESP32-CAM boards/
  // cables, showing up as a green or pink color cast. 10MHz is more
  // tolerant and still plenty fast for still captures.
  c.xclk_freq_hz = 10000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_XGA;       // 1024x768
  c.jpeg_quality = 12;
  if (psramFound()) {
    c.fb_location = CAMERA_FB_IN_PSRAM; c.fb_count = 2; c.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    Serial.println("[CAM] No PSRAM detected - falling back to smaller frame size");
    c.fb_location = CAMERA_FB_IN_DRAM; c.fb_count = 1; c.frame_size = FRAMESIZE_SVGA;
    c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) { Serial.printf("[CAM] Init failed: 0x%x\n", err); return false; }

  // Keep gain modest — high gain ceiling pushed sensor noise hard enough in
  // the dark to show up as a green/purple color cast. Rely mainly on the
  // flash LED for low-light shots, not on cranking analog gain.
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 1);
    s->set_gainceiling(s, GAINCEILING_2X);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
  }

  Serial.println("[CAM] OV2640 ready");
  return true;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) { delay(400); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("[WiFi] OK  IP=%s  RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else
    Serial.println("[WiFi] Failed, will retry");
}

// ---------------------------------------------------------------------------
// POST the JPEG to the backend as multipart/form-data (fields: device_id,
// event_type, file) — matches app/routers/upload.py on the server.
// ---------------------------------------------------------------------------
bool uploadToServer(const uint8_t *jpg, size_t len) {
  const String boundary = "----Esp32CamUploadBoundary";
  String head =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"device_id\"\r\n\r\n" + String(DEVICE_ID) + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"event_type\"\r\n\r\nimage_capture\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"capture.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t total = head.length() + len + tail.length();
  uint8_t *body = psramFound() ? (uint8_t *)ps_malloc(total) : nullptr;
  if (!body) body = (uint8_t *)malloc(total);
  if (!body) { Serial.println("[HTTP] Out of memory building request body"); return false; }
  size_t o = 0;
  memcpy(body + o, head.c_str(), head.length()); o += head.length();
  memcpy(body + o, jpg, len);                    o += len;
  memcpy(body + o, tail.c_str(), tail.length());

  HTTPClient http;
  String url = String("http://") + SERVER_HOST + ":" + String(SERVER_PORT) + UPLOAD_PATH;
  http.begin(url);
  http.setTimeout(15000);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  http.addHeader("X-API-Key", HARDWARE_API_KEY);

  unsigned long t0 = millis();
  int code = http.POST(body, total);
  String resp = http.getString();
  http.end();
  free(body);

  Serial.printf("[HTTP] POST %s -> %d in %lu ms\n", url.c_str(), code, millis() - t0);
  if (code != 201) Serial.println(resp);
  return code == 201;
}

// ---------------------------------------------------------------------------
void captureAndUpload(const char *source) {
  Serial.printf("\n[TRIGGER] %s\n", source);
  if (!cameraReady) { Serial.println("[CAM] Not initialized, skipping"); return; }

  digitalWrite(FLASH_LED_PIN, HIGH);
  delay(150);

  camera_fb_t *stale = esp_camera_fb_get();
  if (stale) esp_camera_fb_return(stale);
  delay(50);
  camera_fb_t *fb = esp_camera_fb_get();

  digitalWrite(FLASH_LED_PIN, LOW);

  if (!fb) { Serial.println("[CAM] Capture failed"); return; }
  Serial.printf("[CAM] %u bytes (%ux%u)\n", (unsigned)fb->len, fb->width, fb->height);

  bool ok = false;
  for (int i = 1; i <= 3 && !ok; i++) {
    connectWiFi();
    ok = uploadToServer(fb->buf, fb->len);
    if (!ok) delay(1000 * i);
  }
  esp_camera_fb_return(fb);
  Serial.println(ok ? "[DONE] Image uploaded" : "[DONE] FAILED to upload after 3 attempts");
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== Parcel Box Vision Node (ESP32-CAM) ===");
  Serial.printf("[SYS] PSRAM %s, free=%u\n", psramFound() ? "found" : "NOT found",
                (unsigned)ESP.getFreePsram());

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  cameraReady = initCamera();
  pinMode(PIN_TRIGGER_IN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(PIN_TRIGGER_IN), onTrigger, RISING);

  connectWiFi();
  Serial.println("[SYS] Ready: waiting for trigger pulse on GPIO13 (or type 'c' here to test)");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (triggerPending) {
    triggerPending = false;
    if (millis() - lastTriggerMs >= TRIGGER_COOLDOWN_MS) {
      lastTriggerMs = millis();
      captureAndUpload("GPIO13 trigger");
    }
  }

  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == 'c' || ch == 'C') captureAndUpload("Serial 'c'");
  }

  delay(10);
}
