#include <WiFi.h>
#include <WiFiManager.h>

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("=== ESP32 WiFi Test ===");
  Serial.println("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
  
  // Minimal WiFi setup
  WiFi.mode(WIFI_STA);
  delay(100);
  
  WiFiManager wm;
  wm.setTimeout(120);
  
  Serial.println("Starting WiFiManager...");
  
  if (!wm.autoConnect("ESP32_Test")) {
    Serial.println("Failed to connect");
    ESP.restart();
  }
  
  Serial.println("WiFi connected!");
  Serial.println("IP: " + WiFi.localIP().toString());
}

void loop() {
  Serial.println("Running... Free heap: " + String(ESP.getFreeHeap()));
  delay(5000);
}
