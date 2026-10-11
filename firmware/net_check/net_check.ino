// Network check for the ESP32 (no camera, no lock). Open Serial Monitor at 115200.
// Fill in the 4 values below (same as config.h), upload, and read the result. Press EN to run again.
#include <WiFi.h>
#include <HTTPClient.h>

#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"
#define SERVER_HOST   "172.20.10.3"
#define SERVER_PORT   8888

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n=== net_check ===");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[FAIL] Wi-Fi not connected (status %d). Check SSID/password; use a 2.4 GHz network.\n", WiFi.status());
    return;
  }
  Serial.printf("[OK] Wi-Fi connected. ESP32 IP %s, gateway %s, RSSI %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
  Serial.printf("Server target: %s:%d\n", SERVER_HOST, SERVER_PORT);
  Serial.println(strncmp(WiFi.localIP().toString().c_str(), "172.20.10.", 10) == 0 ? "(ESP32 is on the 172.20.10.x hotspot subnet)" : "(ESP32 is NOT on the 172.20.10.x subnet - different network from the server?)");

  WiFiClient c;
  c.setTimeout(5);
  Serial.print("TCP connect... ");
  if (!c.connect(SERVER_HOST, SERVER_PORT, 5000)) {
    Serial.println("[FAIL] cannot open TCP connection to the server.");
    Serial.println("  -> wrong IP/port, Windows firewall, server bound to 127.0.0.1, or hotspot blocks device-to-device traffic");
    return;
  }
  Serial.println("[OK] TCP connected");
  c.stop();

  HTTPClient http; WiFiClient cl;
  http.setConnectTimeout(5000);
  if (!http.begin(cl, SERVER_HOST, SERVER_PORT, "/health")) { Serial.println("[FAIL] http.begin"); return; }
  int code = http.GET();
  Serial.printf("GET /health -> %d  %s\n", code, code > 0 ? http.getString().c_str() : http.errorToString(code).c_str());
  Serial.println(code == 200 ? "[OK] The server is reachable from the ESP32. Check the API key (401) if heartbeat still fails." : "[FAIL] see code");
  http.end();
}

void loop() {}
