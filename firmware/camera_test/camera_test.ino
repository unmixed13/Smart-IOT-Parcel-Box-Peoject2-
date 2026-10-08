// ESP32-CAM standalone test - no server, no main board needed.
// Board: "AI Thinker ESP32-CAM", Serial Monitor 115200.
// Serial commands: p = take photo (print size)   f = flash on/off   t = watch GPIO13 (TRIG) level
// If WIFI_SSID is filled, open http://<IP printed>/ in a browser to see a live photo.
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>

#define WIFI_SSID     ""      // leave "" to skip Wi-Fi
#define WIFI_PASSWORD ""

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
#define LED_PIN 33      // red LED, active LOW
#define TRIG_PIN 13

WebServer web(80);
bool flashOn = false;

bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 10000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode = CAMERA_GRAB_LATEST;
  if (psramFound()) {
    c.frame_size = FRAMESIZE_VGA; c.jpeg_quality = 12; c.fb_count = 2; c.fb_location = CAMERA_FB_IN_PSRAM;
    Serial.println("PSRAM found");
  } else {
    c.frame_size = FRAMESIZE_QVGA; c.jpeg_quality = 15; c.fb_count = 1; c.fb_location = CAMERA_FB_IN_DRAM;
    Serial.println("NO PSRAM (low-res mode)");
  }
  esp_err_t e = esp_camera_init(&c);
  if (e != ESP_OK) { Serial.printf("Camera init FAILED 0x%x\n", e); return false; }
  Serial.println("OV2640 ready");
  return true;
}

camera_fb_t* grab() {
  camera_fb_t* fb = esp_camera_fb_get(); if (fb) esp_camera_fb_return(fb);  // drop stale frame
  return esp_camera_fb_get();
}

void shoot() {
  digitalWrite(FLASH_PIN, HIGH); delay(150);
  camera_fb_t* fb = grab();
  digitalWrite(FLASH_PIN, flashOn ? HIGH : LOW);
  if (!fb) { Serial.println("[FAIL] no frame"); return; }
  bool jpg = fb->len > 4 && fb->buf[0] == 0xFF && fb->buf[1] == 0xD8;
  Serial.printf("[OK] %ux%u, %u bytes, JPEG header %s\n", fb->width, fb->height, (unsigned)fb->len, jpg ? "valid" : "BAD");
  esp_camera_fb_return(fb);
}

void handleRoot() {
  web.send(200, "text/html", "<meta name=viewport content='width=device-width'><h3>ESP32-CAM test</h3>"
                             "<img src='/photo' style='max-width:100%'><p><a href='/'>refresh</a></p>");
}
void handlePhoto() {
  digitalWrite(FLASH_PIN, HIGH); delay(150);
  camera_fb_t* fb = grab();
  digitalWrite(FLASH_PIN, flashOn ? HIGH : LOW);
  if (!fb) { web.send(500, "text/plain", "no frame"); return; }
  web.sendHeader("Cache-Control", "no-store");
  web.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n=== ESP32-CAM test ===");
  pinMode(FLASH_PIN, OUTPUT); digitalWrite(FLASH_PIN, LOW);
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH);
  pinMode(TRIG_PIN, INPUT_PULLDOWN);
  if (!initCamera()) { Serial.println("Check: camera ribbon, power (use USB base), reboot"); return; }
  digitalWrite(LED_PIN, LOW); delay(150); digitalWrite(LED_PIN, HIGH);
  if (strlen(WIFI_SSID)) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nWi-Fi OK. Open http://%s/\n", WiFi.localIP().toString().c_str());
      web.on("/", handleRoot); web.on("/photo", handlePhoto); web.begin();
    } else Serial.println("\nWi-Fi FAILED");
  }
  Serial.println("Commands: p=photo  f=flash toggle  t=watch TRIG(GPIO13) 10s");
}

void loop() {
  web.handleClient();
  if (!Serial.available()) return;
  char ch = Serial.read();
  if (ch == 'p') shoot();
  else if (ch == 'f') { flashOn = !flashOn; digitalWrite(FLASH_PIN, flashOn); Serial.println(flashOn ? "flash ON" : "flash OFF"); }
  else if (ch == 't') {
    Serial.println("watching GPIO13 for 10 s (pulse it from the main board)...");
    int last = -1; uint32_t t0 = millis();
    while (millis() - t0 < 10000) {
      int v = digitalRead(TRIG_PIN);
      if (v != last) { Serial.printf("GPIO13 = %s\n", v ? "HIGH" : "LOW"); last = v;
        digitalWrite(LED_PIN, v ? LOW : HIGH); }
      delay(5);
    }
    Serial.println("done");
  }
}
