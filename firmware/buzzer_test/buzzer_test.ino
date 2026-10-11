// Buzzer bench test (ESP32 Dev, buzzer I/O on GPIO13, VCC on 5V/VIN, GND on GND). Serial 115200, "New Line".
//   t = 2700 Hz tone for 1 s   s = sweep 1500..4000 Hz   f<number> = tone at that Hz (e.g. f3000)
//   h = pin HIGH   l = pin LOW   (an ACTIVE buzzer beeps steadily on one of these; a PASSIVE one only clicks)
#define BUZ_PIN 13
#define CH 4

void toneOn(uint32_t hz) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BUZ_PIN, hz, 8); ledcWriteTone(BUZ_PIN, hz);
#else
  ledcSetup(CH, hz, 8); ledcAttachPin(BUZ_PIN, CH); ledcWriteTone(CH, hz);
#endif
}
void toneOff(bool idleHigh) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcDetach(BUZ_PIN);
#else
  ledcDetachPin(BUZ_PIN);
#endif
  pinMode(BUZ_PIN, OUTPUT); digitalWrite(BUZ_PIN, idleHigh ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200); delay(300);
  toneOff(true);   // low-level-trigger module: HIGH = silent
  Serial.println("\nBuzzer test: t / s / f<Hz> / h / l");
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 't') { Serial.println("tone 2700 Hz"); toneOn(2700); delay(1000); toneOff(true); }
  else if (c == 's') { Serial.println("sweep"); for (int f = 1500; f <= 4000; f += 100) { toneOn(f); delay(80); } toneOff(true); }
  else if (c == 'f') { int hz = Serial.parseInt(); if (hz >= 100 && hz <= 10000) { Serial.printf("tone %d Hz\n", hz); toneOn(hz); delay(1000); toneOff(true); } }
  else if (c == 'h') { toneOff(true); Serial.println("pin HIGH"); }
  else if (c == 'l') { toneOff(false); Serial.println("pin LOW"); }
}
