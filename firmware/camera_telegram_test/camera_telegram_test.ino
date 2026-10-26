// ESP32-CAM test: take a photo and send it to Telegram. No server, no main board needed.
// Board: "AI Thinker ESP32-CAM", Partition Scheme: Huge APP, Serial Monitor 115200.
// Serial commands:  p = take photo and send it to Telegram
//                   m = send a text message          f = flash LED on/off
// Copy config.example.h to config.h first (Wi-Fi + bot token + chat id).
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "config.h"

// AI Thinker ESP32-CAM pins
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22
#define FLASH_PIN 4

static bool flashOn = false;

static bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM; c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  if (psramFound()) {
    c.frame_size = FRAMESIZE_SVGA;     // 800x600
    c.jpeg_quality = 12;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    c.frame_size = FRAMESIZE_VGA;
    c.jpeg_quality = 15;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_DRAM;
  }
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) { Serial.printf("[CAM] init failed 0x%x\n", err); return false; }
  return true;
}

static bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("[WiFi] connecting to %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print('.'); }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[WiFi] failed (check SSID/password)"); return false; }
  Serial.print("[WiFi] IP "); Serial.println(WiFi.localIP());
  return true;
}

// Read the HTTP response and report whether Telegram answered "ok":true.
static bool readReply(WiFiClientSecure &cl) {
  unsigned long t0 = millis();
  String body;
  while (cl.connected() && millis() - t0 < 15000) {
    while (cl.available()) body += (char)cl.read();
    if (body.indexOf("\r\n\r\n") > 0 && body.indexOf('}') > 0) break;
    delay(10);
  }
  bool ok = body.indexOf("\"ok\":true") >= 0;
  if (!ok) Serial.println("[TG] reply: " + body.substring(0, 300));
  return ok;
}

static bool sendMessage(const String &text) {
  if (!connectWifi()) return false;
  WiFiClientSecure cl;
  cl.setInsecure();                    // test only: skips certificate check
  if (!cl.connect("api.telegram.org", 443)) { Serial.println("[TG] connect failed"); return false; }
  String body = "chat_id=" TELEGRAM_CHAT_ID "&text=" + text;
  cl.println("POST /bot" TELEGRAM_BOT_TOKEN "/sendMessage HTTP/1.1");
  cl.println("Host: api.telegram.org");
  cl.println("Content-Type: application/x-www-form-urlencoded");
  cl.println("Content-Length: " + String(body.length()));
  cl.println("Connection: close");
  cl.println();
  cl.print(body);
  bool ok = readReply(cl);
  cl.stop();
  return ok;
}

static bool sendPhoto(const String &caption) {
  if (!connectWifi()) return false;

  // The first frames after init are often dark/blurry: drop two, keep the third.
  for (int i = 0; i < 2; i++) { camera_fb_t *f = esp_camera_fb_get(); if (f) esp_camera_fb_return(f); }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { Serial.println("[CAM] capture failed"); return false; }
  Serial.printf("[CAM] photo %ux%u, %u bytes\n", fb->width, fb->height, (unsigned)fb->len);

  WiFiClientSecure cl;
  cl.setInsecure();
  if (!cl.connect("api.telegram.org", 443)) {
    Serial.println("[TG] connect failed");
    esp_camera_fb_return(fb);
    return false;
  }
  const String boundary = "----ParcelBoxBoundary";
  String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n" TELEGRAM_CHAT_ID "\r\n"
                "--" + boundary + "\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n" + caption + "\r\n"
                "--" + boundary + "\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"photo.jpg\"\r\n"
                "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  cl.println("POST /bot" TELEGRAM_BOT_TOKEN "/sendPhoto HTTP/1.1");
  cl.println("Host: api.telegram.org");
  cl.println("Content-Type: multipart/form-data; boundary=" + boundary);
  cl.println("Content-Length: " + String(head.length() + fb->len + tail.length()));
  cl.println("Connection: close");
  cl.println();
  cl.print(head);
  const uint8_t *p = fb->buf;
  size_t left = fb->len;
  while (left > 0) {                    // send in 1 kB chunks
    size_t n = left > 1024 ? 1024 : left;
    cl.write(p, n);
    p += n; left -= n;
  }
  cl.print(tail);
  esp_camera_fb_return(fb);

  bool ok = readReply(cl);
  cl.stop();
  Serial.println(ok ? "[TG] photo sent OK" : "[TG] photo FAILED");
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-CAM -> Telegram test ===");
  pinMode(FLASH_PIN, OUTPUT); digitalWrite(FLASH_PIN, LOW);
  if (!initCamera()) { Serial.println("Camera init failed - check power (5 V >= 500 mA) and ribbon cable"); return; }
  if (connectWifi()) sendMessage("ESP32-CAM test ready");
  Serial.println("Commands: p = photo to Telegram, m = text message, f = flash");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'p') sendPhoto("Parcel box test photo");
    else if (c == 'm') Serial.println(sendMessage("Hello from ESP32-CAM") ? "[TG] message sent" : "[TG] message FAILED");
    else if (c == 'f') { flashOn = !flashOn; digitalWrite(FLASH_PIN, flashOn); Serial.println(flashOn ? "flash ON" : "flash OFF"); }
  }
  delay(20);
}
