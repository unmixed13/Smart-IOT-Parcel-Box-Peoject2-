// Solenoid lock bench test (ESP32 Dev, IRLZ44N on GPIO18). Serial Monitor 115200, "New Line".
//   o = unlock for 2 s    l = lock now    a = auto: pulse 1 s every 6 s (x5)
// Safety: the coil is never left on longer than MAX_ON_MS (solenoids overheat if held).
#define LOCK_PIN 18
#define LOCK_ACTIVE_HIGH 1      // N-MOSFET: HIGH = coil powered = unlocked
#define MAX_ON_MS 5000

void coil(bool on) { digitalWrite(LOCK_PIN, (on == LOCK_ACTIVE_HIGH) ? HIGH : LOW); }

void pulse(uint32_t ms) {
  if (ms > MAX_ON_MS) ms = MAX_ON_MS;
  Serial.printf("UNLOCK %u ms\n", (unsigned)ms);
  coil(true); delay(ms); coil(false);
  Serial.println("LOCKED (coil off)");
}

void setup() {
  pinMode(LOCK_PIN, OUTPUT); coil(false);
  Serial.begin(115200); delay(300);
  Serial.println("\nLock test ready: o=unlock 2s  l=lock  a=auto x5");
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 'o') pulse(2000);
  else if (c == 'l') { coil(false); Serial.println("LOCKED (coil off)"); }
  else if (c == 'a') for (int i = 1; i <= 5; i++) { Serial.printf("#%d ", i); pulse(1000); delay(5000); }
}
