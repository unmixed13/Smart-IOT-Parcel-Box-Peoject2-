// Solenoid lock + buzzer bench test (ESP32 DevKit, Arduino IDE, single file)
// Open Serial Monitor at 115200 baud, line ending "Newline" or "No line ending", then send:
//   u = unlock 3 s then lock again     l = lock now
//   b = beep once                      s = rising/falling siren
//   a = automatic sequence (lock -> unlock -> lock -> beep -> siren)
//   h = help
//
// Pins and polarity follow the project wiring. Change here if your build differs.
#define PIN_LOCK_SIG   18     // -> R3 220 ohm -> Q2 gate
#define PIN_BUZZ_SIG   13     // -> buzzer driver gate
#define LOCK_LEVEL     HIGH   // GPIO18 HIGH = locked, LOW = unlocked (as built)
#define BUZZER_PASSIVE 1      // 1 = passive buzzer (needs a tone), 0 = active buzzer (HIGH = on)
#define BUZZER_FREQ_HZ 2700
#define UNLOCK_MS      3000   // keep short: the solenoid heats up when powered
#define UNLOCK_LEVEL   (LOCK_LEVEL == HIGH ? LOW : HIGH)

static void logMsg(const char *m) { Serial.printf("[%lu] %s\n", millis(), m); }

static void buzzOn(int hz) {
#if BUZZER_PASSIVE
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(PIN_BUZZ_SIG, hz, 8);
    ledcWriteTone(PIN_BUZZ_SIG, hz);
  #else
    ledcSetup(0, hz, 8);
    ledcAttachPin(PIN_BUZZ_SIG, 0);
    ledcWriteTone(0, hz);
  #endif
#else
  (void)hz;
  digitalWrite(PIN_BUZZ_SIG, HIGH);
#endif
}

static void buzzOff() {
#if BUZZER_PASSIVE
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteTone(PIN_BUZZ_SIG, 0);
    ledcDetach(PIN_BUZZ_SIG);
  #else
    ledcWriteTone(0, 0);
    ledcDetachPin(PIN_BUZZ_SIG);
  #endif
#endif
  pinMode(PIN_BUZZ_SIG, OUTPUT);
  digitalWrite(PIN_BUZZ_SIG, LOW);
}

static void lockNow()   { digitalWrite(PIN_LOCK_SIG, LOCK_LEVEL);   logMsg("LOCK   (GPIO18 = locked level)"); }
static void unlockNow() { digitalWrite(PIN_LOCK_SIG, UNLOCK_LEVEL); logMsg("UNLOCK (GPIO18 = unlocked level)"); }

static void testUnlock() {
  unlockNow();
  delay(UNLOCK_MS);
  lockNow();
}

static void testBeep() {
  logMsg("BEEP");
  buzzOn(BUZZER_FREQ_HZ);
  delay(300);
  buzzOff();
}

static void testSiren() {
  logMsg("SIREN");
#if BUZZER_PASSIVE
  for (int f = 1000; f <= 3500; f += 50) { buzzOn(f); delay(10); }
  for (int f = 3500; f >= 1000; f -= 50) { buzzOn(f); delay(10); }
  buzzOff();
#else
  for (int i = 0; i < 5; i++) { buzzOn(0); delay(150); buzzOff(); delay(150); }
#endif
}

static void help() {
  Serial.println("u=unlock 3s  l=lock  b=beep  s=siren  a=auto sequence  h=help");
}

void setup() {
  // Drive the lock pin to the locked level before anything else.
  pinMode(PIN_LOCK_SIG, OUTPUT);
  digitalWrite(PIN_LOCK_SIG, LOCK_LEVEL);
  pinMode(PIN_BUZZ_SIG, OUTPUT);
  digitalWrite(PIN_BUZZ_SIG, LOW);
  Serial.begin(115200);
  delay(500);
  logMsg("hardware test ready");
  help();
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  switch (c) {
    case 'u': testUnlock(); break;
    case 'l': lockNow(); break;
    case 'b': testBeep(); break;
    case 's': testSiren(); break;
    case 'a':
      lockNow();   delay(1000);
      testUnlock();delay(1000);
      testBeep();  delay(500);
      testSiren();
      logMsg("sequence done");
      break;
    case 'h': help(); break;
    default: break;   // ignore newline / other characters
  }
}
