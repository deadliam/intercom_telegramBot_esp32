/*
  Telegram bot for intercom control via ESP8266/Wemos
  
  FUNCTIONALITY:
  - Monitor incoming calls via analog input A0 (voltage divider)
  - Automatic Telegram notifications on incoming calls
  - Intercom control via "Open" command:
    1. Press camera button (D1) for 2 seconds  
    2. Press door lock button (D2) briefly
  
  CONNECTIONS:
  - D1 (GPIO5) - PC817 optocoupler for camera button
  - D2 (GPIO4) - PC817 optocoupler for door lock button  
  - A0 - voltage divider from call indicator LED (2x10kOhm, divides 1.8V→0.9V)
  - Built-in LED (GPIO2) - status indication
  
  COMMANDS:
  - /start - main menu
  - /status - system status and analog input
  - "Open" - open intercom
  
  LED ERROR CODES:
  - 1 blink  = WiFi connection failure
  - 2 blinks = Critical low memory (< 5000 bytes)
  - 3 blinks = System stuck (no activity 5+ minutes)
  - 4 blinks = General error
  - 5 blinks = Telegram connection error
  
  LED patterns repeat every 3 seconds when error occurs
*/

#ifdef ESP32
  #include <WiFi.h>
  #include <WebServer.h>
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
#endif
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// WiFiManager and OTA libraries
#include <DNSServer.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <ElegantOTA.h>
#include <EEPROM.h>
// Local secrets (not committed). See secrets.example.h for the template
#if defined(__has_include)
  #if __has_include("../secrets.h")
    #include "../secrets.h"
    #define SECRETS_SOURCE "Real secrets from ../secrets.h"
  #elif __has_include("secrets.h")
    #include "secrets.h"
    #define SECRETS_SOURCE "Real secrets from local secrets.h"
  #else
    #include "secrets.example.h"
    #define SECRETS_SOURCE "Template from secrets.example.h - NO REAL SECRETS FOUND!"
  #endif
#else
  #include "../secrets.h"
  #define SECRETS_SOURCE "Direct include ../secrets.h"
#endif

// Replace with your network credentials
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Initialize Telegram BOT
#define BOTtoken BOT_TOKEN  // your Bot Token (from BotFather)

// Use @myidbot to find out the chat ID of an individual or a group
// Also note that you need to click "start" on a bot before it can message you
// CHAT_ID now defined in secrets.h

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// Web server and OTA setup
#ifdef ESP32
  WebServer server(80);
#else
  ESP8266WebServer server(80);
#endif

unsigned long ota_progress_millis = 0;

// Polling period (ms)
int botRequestDelay = 2000;
unsigned long lastTimeBotRan;

// EEPROM settings structure
struct IntercomSettings {
  int cameraActivationTime;    // Camera activation time in ms
  int callThreshold;          // Threshold value for A0
  int callDebounceTime;       // Call debounce time in ms  
  int doorActivationTime;     // Door activation time in ms
  char botToken[50];          // Telegram bot token
  char chatId[20];            // Telegram Chat ID
  unsigned long rebootCount;   // Count of system reboots
  unsigned long lastUptime;    // Last uptime before reboot (minutes)
};

IntercomSettings settings;
const int EEPROM_ADDR = 0;

// GPIO for onboard LED (optional)
const int ledPin = 2;
bool ledState = LOW;

// Intercom configuration
const int cameraPin = 5;       // GPIO5 (D1 on Wemos) - camera activation button 
const int doorPin = 4;         // GPIO4 (D2 on Wemos) - door lock button
const bool optocouplerActiveHigh = false; // PC817 optocouplers are usually active LOW

// Call detection configuration
const int callIndicatorPin = A0;  // Analog input for call monitoring
// Settings below are loaded from EEPROM or use default values

// Call detection variables
bool callDetected = false;
bool lastCallState = false;
unsigned long lastCallTime = 0;

// Reply keyboard with one "Open" button (persistent chat menu)
static const char replyKeyboard[] = "[[\"Open\"]]";

// ========== LED ERROR INDICATION FUNCTIONS ==========

// Blink built-in LED with error code (number of blinks)
void blinkErrorCode(int errorCode) {
  const int blinkDuration = 200;    // Short blink duration
  const int pauseBetween = 300;     // Pause between blinks
  const int pauseAfter = 2000;      // Pause after sequence
  
  Serial.print("🚨 LED Error Code: "); Serial.print(errorCode); Serial.println(" blinks");
  
  for (int i = 0; i < errorCode; i++) {
    digitalWrite(ledPin, LOW);   // Turn LED ON (built-in LED is active LOW)
    delay(blinkDuration);
    digitalWrite(ledPin, HIGH);  // Turn LED OFF
    
    if (i < errorCode - 1) {     // Pause between blinks (except after last)
      delay(pauseBetween);
    }
  }
  
  delay(pauseAfter);  // Pause after complete sequence
}

// Continuous error indication with specified code
void indicateError(int errorCode, int repeatTimes = 3) {
  Serial.print("🚨 Indicating error code "); Serial.print(errorCode);
  Serial.print(" ("); Serial.print(repeatTimes); Serial.println(" times)");
  
  for (int i = 0; i < repeatTimes; i++) {
    blinkErrorCode(errorCode);
  }
}

// ========== EEPROM AND SETTINGS FUNCTIONS ==========

void saveSettings() {
  EEPROM.put(EEPROM_ADDR, settings);
  EEPROM.commit();
  Serial.println("⚙️ Settings saved to EEPROM");
}

void loadSettings() {
  EEPROM.get(EEPROM_ADDR, settings);

  // Validate settings (check if values are reasonable)
  if (settings.cameraActivationTime < 100 || settings.cameraActivationTime > 10000) {
    // Load default settings
    settings.cameraActivationTime = 2000;  // 2 seconds for camera
    settings.callThreshold = 200;          // A0 threshold value
    settings.callDebounceTime = 5000;      // 5 seconds debounce
    settings.doorActivationTime = 200;     // 200ms for door lock
    strncpy(settings.botToken, BOT_TOKEN, sizeof(settings.botToken) - 1);
    strncpy(settings.chatId, CHAT_ID, sizeof(settings.chatId) - 1);
    settings.rebootCount = 1;              // First boot
    settings.lastUptime = 0;               // No previous uptime
    
    Serial.println("⚙️ Loading default settings");
    saveSettings();
  } else {
    // Update reboot statistics
    settings.lastUptime = millis() / 60000;  // Previous uptime in minutes (will be 0 on first boot)
    settings.rebootCount++;
    Serial.println("⚙️ Settings loaded from EEPROM");
    Serial.print("📊 Boot count: "); Serial.println(settings.rebootCount);
    Serial.print("⏱️ Last uptime: "); Serial.print(settings.lastUptime); Serial.println(" minutes");
    saveSettings(); // Save updated boot count
  }
}

// ========== OTA CALLBACK FUNCTIONS ==========

void onOTAStart() {
  Serial.println("🔄 OTA update started!");
  // Disable intercom during update
}

void onOTAProgress(size_t current, size_t final) {
  if (millis() - ota_progress_millis > 1000) {
    ota_progress_millis = millis();
    Serial.printf("🔄 OTA Progress: %u/%u bytes (%.1f%%)\n", 
                  current, final, (float)current / final * 100);
  }
}

void onOTAEnd(bool success) {
  if (success) {
    Serial.println("✅ OTA update finished successfully!");
  } else {
    Serial.println("❌ OTA update failed!");
  }
}

// Function to press camera button (timing from settings)
void pressCameraButton() {
  Serial.println("🎥 === CAMERA BUTTON PRESS START ===");
  Serial.print("Setting pin "); Serial.print(cameraPin); 
  Serial.print(" to "); Serial.println(optocouplerActiveHigh ? "HIGH" : "LOW");
  Serial.print("Duration: "); Serial.print(settings.cameraActivationTime); Serial.println("ms");
  
  digitalWrite(cameraPin, optocouplerActiveHigh ? HIGH : LOW);
  delay(settings.cameraActivationTime); // Time from settings
  digitalWrite(cameraPin, optocouplerActiveHigh ? LOW : HIGH);
  
  Serial.print("Setting pin "); Serial.print(cameraPin); 
  Serial.print(" to "); Serial.println(optocouplerActiveHigh ? "LOW" : "HIGH");
  Serial.println("🎥 === CAMERA BUTTON RELEASE ===");
}

// Function for brief door lock button press (timing from settings)
void pressDoorButton() {
  Serial.println("🚪 === DOOR BUTTON PRESS START ===");
  Serial.print("Setting pin "); Serial.print(doorPin); 
  Serial.print(" to "); Serial.println(optocouplerActiveHigh ? "HIGH" : "LOW");
  Serial.print("Duration: "); Serial.print(settings.doorActivationTime); Serial.println("ms");
  
  digitalWrite(doorPin, optocouplerActiveHigh ? HIGH : LOW);
  delay(settings.doorActivationTime); // Time from settings
  digitalWrite(doorPin, optocouplerActiveHigh ? LOW : HIGH);
  
  Serial.print("Setting pin "); Serial.print(doorPin); 
  Serial.print(" to "); Serial.println(optocouplerActiveHigh ? "LOW" : "HIGH");
  Serial.println("🚪 === DOOR BUTTON RELEASE ===");
}

// Complete intercom opening sequence
void openIntercom() {
  Serial.println("=== Starting intercom opening sequence ===");
  
  // First activate camera
  pressCameraButton();
  
  // Small pause between operations
  delay(500);
  
  // Then open door lock
  pressDoorButton();
  
  Serial.println("=== Intercom opening sequence completed ===");
}

// Function to check incoming calls
void checkIncomingCall() {
  int analogValue = analogRead(callIndicatorPin);
  bool currentCallState = (analogValue < settings.callThreshold);
  
  // Debug information every 10 seconds
  static unsigned long lastDebugTime = 0;
  if (millis() - lastDebugTime > 10000) {
    Serial.print("Call indicator A0 value: ");
    Serial.print(analogValue);
    Serial.print(" (threshold: ");
    Serial.print(settings.callThreshold);
    Serial.println(")");
    lastDebugTime = millis();
  }
  
  // New call detection
  if (currentCallState && !lastCallState) {
    // New call detected
    if (millis() - lastCallTime > settings.callDebounceTime) {
      callDetected = true;
      lastCallTime = millis();
      
      // Call notification via Telegram
      Serial.println("🔔 === INCOMING CALL DETECTED ===");
      Serial.println("📞 Someone is calling the intercom!");
      
      String callMessage = "🔔 INCOMING CALL!\n\nSomeone is calling the intercom.\nPress 'Open' to open.";
      bool sent = bot.sendMessageWithReplyKeyboard(CHAT_ID, callMessage, "", replyKeyboard, true, false, false);
      Serial.print("📤 Telegram notification: "); Serial.println(sent ? "✅ Sent" : "❌ Failed");
      
      Serial.println("================================");
    }
  }
  
  lastCallState = currentCallState;
}

// Handle new messages: only /start and one inline button "Open"
void handleNewMessages(int numNewMessages) {
  Serial.print("📩 handleNewMessages: "); Serial.println(numNewMessages);

  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    Serial.println("=== MESSAGE DEBUG ===");
    Serial.print("Chat ID: "); Serial.println(chat_id);
    Serial.print("Expected: "); Serial.println(CHAT_ID);
    Serial.print("Text: "); Serial.println(text);
    Serial.print("From: "); Serial.println(from_name);
    Serial.println("====================");

    if (chat_id != CHAT_ID) {
      Serial.println("❌ Unauthorized user - chat ID mismatch!");
      bot.sendMessage(chat_id, "Your Chat ID: " + chat_id + "\nUpdate secrets.h with this ID", "");
      continue;
    } else {
      Serial.println("✅ Authorized user - processing message");
    }

    // Normal text messages only; reply keyboard sends text "Open"
    if (text == "/start") {
      Serial.println("🏠 Processing /start command");
      String welcome = "🏠 Welcome, " + from_name + "!\n\nThis bot controls the intercom:\n• Monitor incoming calls 🔔\n• Open intercom 🚪\n\nPress 'Open' to open or /status for system status.";
      bot.sendMessageWithReplyKeyboard(chat_id, welcome, "", replyKeyboard, true, false, false);
    } else if (text == "Open") {
      Serial.println("🚪 Processing 'Open' command - INTERCOM SEQUENCE START");
      bot.sendMessage(chat_id, "🎥 Activating intercom camera...", "");
      
      // Включаем камеру (2 секунды)
      pressCameraButton();
      
      bot.sendMessage(chat_id, "🚪 Opening door lock...", "");
      
      // Пауза и открытие замка
      delay(500);
      pressDoorButton();
      
      bot.sendMessage(chat_id, "✅ Intercom opened!", "");
      Serial.println("🚪 INTERCOM SEQUENCE COMPLETED");
    } else if (text == "/status") {
      Serial.println("📊 Processing /status command");
      // Команда для проверки состояния системы
      int analogValue = analogRead(callIndicatorPin);
      String statusMessage = "🏠 Intercom bot started!\n";
      statusMessage += "IP: " + WiFi.localIP().toString() + "\n\n";
      statusMessage += "System ready to work!\n\n";
      statusMessage += "📊 SYSTEM STATUS\n\n";
      statusMessage += "🔌 Analog input A0: " + String(analogValue) + "\n";
      statusMessage += "⚡ Threshold value: " + String(settings.callThreshold) + "\n";
      statusMessage += "🚨 Call status: " + String(analogValue < settings.callThreshold ? "CALL" : "NORMAL") + "\n";
      statusMessage += "🎥 Camera time: " + String(settings.cameraActivationTime) + "ms\n";
      statusMessage += "🚪 Door time: " + String(settings.doorActivationTime) + "ms\n";
      statusMessage += "⏱️ Call debounce: " + String(settings.callDebounceTime) + "ms\n\n";
      statusMessage += "📈 SYSTEM INFO\n\n";
      statusMessage += "💾 Free memory: " + String(ESP.getFreeHeap()) + " bytes\n";
      statusMessage += "⏰ Current uptime: " + String(millis() / 60000) + " min\n";
      statusMessage += "📊 Boot count: " + String(settings.rebootCount) + "\n";
      statusMessage += "🔄 Reset reason: " + ESP.getResetReason() + "\n\n";
      statusMessage += "🌐 WEB INTERFACE\n\n";
      statusMessage += "🏠 Main: http://" + WiFi.localIP().toString() + "/\n";
      statusMessage += "⚙️ Settings: http://" + WiFi.localIP().toString() + "/settings\n";
      statusMessage += "🔄 OTA Update: http://" + WiFi.localIP().toString() + "/update\n";
      statusMessage += "🌐 mDNS: http://intercom.local/";
      
      bot.sendMessage(chat_id, statusMessage, "");
    } else {
      Serial.print("❓ Unknown command received: '"); Serial.print(text); Serial.println("'");
      bot.sendMessageWithReplyKeyboard(chat_id, "🏠 To open the intercom press 'Open' button\n\nAvailable commands:\n/start - main menu\n/status - system status", "", replyKeyboard, true, false, false);
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialize EEPROM for settings
  EEPROM.begin(sizeof(IntercomSettings));
  loadSettings();
  
  Serial.println("=== CONFIGURATION DEBUG ===");
  Serial.print("Secrets loaded from: "); Serial.println(SECRETS_SOURCE);
  Serial.print("WiFi SSID: "); Serial.println(WIFI_SSID);
  Serial.print("WiFi Password: "); Serial.println(WIFI_PASSWORD);
  Serial.print("Bot Token: "); Serial.println(BOT_TOKEN);
  Serial.print("Chat ID: "); Serial.println(CHAT_ID);
  
  Serial.println("=== RESET DIAGNOSIS ===");
  Serial.print("🔄 Reset reason: "); Serial.println(ESP.getResetReason());
  Serial.print("💾 Free heap: "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");
  Serial.print("⚡ CPU frequency: "); Serial.print(ESP.getCpuFreqMHz()); Serial.println(" MHz");
  Serial.print("🔌 VCC: "); Serial.print(ESP.getVcc()); Serial.println(" mV (if ADC_VCC enabled)");
  
  // Check if real data was loaded
  if (String(WIFI_SSID) == "YOUR_WIFI_SSID") {
    Serial.println("🚨 WARNING: Using template values! Real secrets.h not loaded!");
  } else {
    Serial.println("✅ Real secrets loaded successfully");
  }
  
  // Important information for debugging
  Serial.println("💬 SYSTEM INFO:");
  Serial.print("Chat ID: "); Serial.println(CHAT_ID);
  Serial.print("Board: "); 
  #ifdef ESP8266
    Serial.println("ESP8266");
  #endif
  #ifdef ARDUINO_ESP8266_WEMOS_D1MINI
    Serial.println("Wemos D1 Mini");
  #elif defined(ARDUINO_ESP8266_NODEMCU)
    Serial.println("NodeMCU");
  #else
    Serial.println("Generic ESP8266");
  #endif
  Serial.print("CPU Freq: "); Serial.print(ESP.getCpuFreqMHz()); Serial.println(" MHz");
  Serial.println("==============================");

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, ledState);
  
  // Initialize pins for intercom control
  pinMode(cameraPin, OUTPUT);
  pinMode(doorPin, OUTPUT);
  
  // Set pins to inactive state (buttons not pressed)
  digitalWrite(cameraPin, optocouplerActiveHigh ? LOW : HIGH);
  digitalWrite(doorPin, optocouplerActiveHigh ? LOW : HIGH);
  
  Serial.println("🔌 GPIO PIN MAPPING:");
  Serial.print("Camera button: GPIO"); Serial.print(cameraPin);
  Serial.println(" (should be D1 on Wemos/D5 on NodeMCU)");
  Serial.print("Door button: GPIO"); Serial.print(doorPin); 
  Serial.println(" (should be D2 on Wemos/D4 on NodeMCU)");
  Serial.print("Call indicator: A0 (GPIO"); Serial.print(callIndicatorPin); Serial.println(")");
  Serial.print("Voltage threshold: "); Serial.println(settings.callThreshold);
  Serial.print("Camera time: "); Serial.print(settings.cameraActivationTime); Serial.println("ms");
  Serial.print("Door time: "); Serial.print(settings.doorActivationTime); Serial.println("ms");
  Serial.print("Debounce time: "); Serial.print(settings.callDebounceTime); Serial.println("ms");
  Serial.println("⚠️  Verify GPIO pins match your hardware wiring!");
  
  // Check initial state of call indicator
  int initialVoltage = analogRead(callIndicatorPin);
  Serial.print("Initial A0 reading: "); Serial.println(initialVoltage);
  
  // GPIO pin test - brief flash for verification
  Serial.println("🧪 Testing GPIO pins (brief flash)...");
  digitalWrite(cameraPin, !digitalRead(cameraPin)); delay(100);
  digitalWrite(cameraPin, !digitalRead(cameraPin));
  digitalWrite(doorPin, !digitalRead(doorPin)); delay(100); 
  digitalWrite(doorPin, !digitalRead(doorPin));
  Serial.println("✅ GPIO pin test completed");

  // WiFiManager setup - automatic connection or web portal
  Serial.println("=== WiFiManager SETUP ===");
  WiFiManager wifiManager;
  
  // Uncomment to reset WiFi settings (for testing)
  // wifiManager.resetSettings();
  
  // Set custom hostname for the device
  wifiManager.setHostname("intercom-telegram");
  
  // Try to connect to saved WiFi or start captive portal
  Serial.println("🔗 Starting WiFiManager...");
  if (!wifiManager.autoConnect("IntercomSetup", "12345678")) {
    Serial.println("❌ Failed to connect or setup portal timeout");
    Serial.println("Restarting device...");
    
    // Indicate WiFi error with LED (1 blink = WiFi failure)
    indicateError(1, 5);  // 1 blink, repeat 5 times
    
    delay(1000);
    ESP.restart();
  }
  
  Serial.println("✅ WiFi connected successfully!");
  Serial.print("IP address: "); Serial.println(WiFi.localIP());
  Serial.print("Gateway: "); Serial.println(WiFi.gatewayIP());
  Serial.print("Subnet: "); Serial.println(WiFi.subnetMask());
  Serial.print("DNS: "); Serial.println(WiFi.dnsIP());
  Serial.print("RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");

  // Setup mDNS responder
  if (MDNS.begin("intercom")) {
    Serial.println("🌐 mDNS responder started");
    Serial.println("Access device at: http://intercom.local/");
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("❌ Error setting up mDNS responder");
  }

  // TLS setup - more aggressive simplification for ESP8266
  Serial.println("🔒 Setting up TLS/SSL...");
  #ifdef ESP8266
    client.setInsecure(); // simplify TLS for ESP8266
    client.setTimeout(10000); // 10 seconds timeout
    Serial.println("✅ Using insecure TLS with 10s timeout");
  #else
    client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
    Serial.println("✅ Using certificate validation for ESP32");
  #endif

  // Web server and OTA setup
  Serial.println("=== WEB SERVER & OTA SETUP ===");
  
  // Main page
  server.on("/", []() {
    String page = "<html><head><meta charset='UTF-8'><title>Intercom Control</title></head><body>";
    page += "<h1>🏠 Telegram Intercom System</h1>";
    page += "<p>IP: " + WiFi.localIP().toString() + "</p>";
    page += "<p>Status: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected") + "</p>";
    page += "<p><a href='/settings'>⚙️ Settings</a></p>";
    page += "<p><a href='/update'>🔄 OTA Update</a></p>";
    page += "</body></html>";
    server.send(200, "text/html; charset=UTF-8", page);
  });

  // Settings page
  server.on("/settings", HTTP_GET, []() {
    String page = "<html><head><meta charset='UTF-8'><title>Intercom Settings</title></head><body>";
    page += "<h2>⚙️ Intercom Configuration</h2>";
    page += "<form method='POST'>";
    page += "Camera activation time (ms): <input type='number' name='camera' value='" + String(settings.cameraActivationTime) + "'><br><br>";
    page += "Call threshold (A0): <input type='number' name='threshold' value='" + String(settings.callThreshold) + "'><br><br>";
    page += "Call debounce time (ms): <input type='number' name='debounce' value='" + String(settings.callDebounceTime) + "'><br><br>";
    page += "Door activation time (ms): <input type='number' name='door' value='" + String(settings.doorActivationTime) + "'><br><br>";
    page += "<input type='submit' value='💾 Save Settings'>";
    page += "</form>";
    page += "<p><a href='/'>🔙 Back to Home</a></p>";
    page += "</body></html>";
    server.send(200, "text/html; charset=UTF-8", page);
  });

  // Handle settings POST request
  server.on("/settings", HTTP_POST, []() {
    bool changed = false;
    
    if (server.hasArg("camera")) {
      settings.cameraActivationTime = constrain(server.arg("camera").toInt(), 100, 10000);
      changed = true;
    }
    if (server.hasArg("threshold")) {
      settings.callThreshold = constrain(server.arg("threshold").toInt(), 0, 1023);
      changed = true;
    }
    if (server.hasArg("debounce")) {
      settings.callDebounceTime = constrain(server.arg("debounce").toInt(), 1000, 60000);
      changed = true;
    }
    if (server.hasArg("door")) {
      settings.doorActivationTime = constrain(server.arg("door").toInt(), 50, 5000);
      changed = true;
    }

    if (changed) {
      saveSettings();
      Serial.println("⚙️ Settings updated via web interface");
    }

    server.sendHeader("Location", "/settings");
    server.send(303);
  });

  // 404 handler
  server.onNotFound([]() {
    server.send(404, "text/plain", "Page not found!");
  });

  // Initialize ElegantOTA
  ElegantOTA.begin(&server);
  ElegantOTA.onStart(onOTAStart);
  ElegantOTA.onProgress(onOTAProgress);
  ElegantOTA.onEnd(onOTAEnd);

  // Start web server
  server.begin();
  Serial.println("🌐 HTTP server started at http://" + WiFi.localIP().toString());
  Serial.println("⚙️ Settings: http://intercom.local/settings");
  Serial.println("🔄 OTA Update: http://intercom.local/update");

  // Telegram setup
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("=== TELEGRAM SETUP ===");
    Serial.print("💾 Free heap: "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");
    
    // Send startup notification with reset reason
    String resetReason = ESP.getResetReason();
    String startupMsg = "🏠 Intercom bot started!\n";
    startupMsg += "IP: " + WiFi.localIP().toString() + "\n\n";
    startupMsg += "📊 BOOT INFO:\n";
    startupMsg += "🔄 Reset reason: " + resetReason + "\n";
    startupMsg += "💾 Free memory: " + String(ESP.getFreeHeap()) + " bytes\n";
    startupMsg += "⚡ CPU freq: " + String(ESP.getCpuFreqMHz()) + " MHz\n";
    startupMsg += "📊 Boot count: " + String(settings.rebootCount) + "\n";
    if (settings.lastUptime > 0) {
      startupMsg += "⏱️ Last uptime: " + String(settings.lastUptime) + " min\n";
    }
    startupMsg += "\nSystem ready to work!";
    
    bool sent = bot.sendMessageWithReplyKeyboard(CHAT_ID, startupMsg, "", replyKeyboard, true, false, false);
    Serial.print("📤 Startup notification: "); Serial.println(sent ? "✅ Sent" : "❌ Failed");
    
    Serial.println("🤖 Telegram bot ready!");
    Serial.println("============================");
  } else {
    Serial.println("Skipping Telegram setup - No WiFi connection");
  }
}

void loop() {
  // Handle web server and OTA
  server.handleClient();
  MDNS.update();
  ElegantOTA.loop();
  
  // Check memory every 30 seconds
  static unsigned long lastMemCheck = 0;
  if (millis() - lastMemCheck > 30000) {
    Serial.print("💾 Free: "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");
    lastMemCheck = millis();
  }
  
  // Check incoming calls
  checkIncomingCall();
  
  // Handle Telegram messages
  if (millis() > lastTimeBotRan + botRequestDelay) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    
    while (numNewMessages) {
      Serial.println("🔄 Processing Telegram messages...");
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = millis();
  }
  
  // Serial commands for debugging
  if (Serial.available()) {
    String command = Serial.readString();
    command.trim();
    
    if (command == "open") {
      Serial.println("🚪 Manual open via Serial");
      // Emulate Telegram message
      Serial.println("🎥 Activating intercom camera...");
      pressCameraButton();
      Serial.println("🚪 Opening door lock...");
      delay(500);
      pressDoorButton();
      Serial.println("✅ Intercom opened!");
    } else if (command == "test") {
      bot.sendMessage(CHAT_ID, "Test message from Serial", "");
    } else if (command == "status") {
      Serial.print("💾 Memory: "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");
      Serial.print("🌐 WiFi: "); Serial.println(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
      Serial.print("📊 A0: "); Serial.print(analogRead(callIndicatorPin));
      Serial.println(analogRead(callIndicatorPin) < settings.callThreshold ? " (CALL)" : " (NORMAL)");
      Serial.print("🌐 IP: "); Serial.println(WiFi.localIP());
    }
  }
  
  // Force memory cleanup every 5 seconds
  static unsigned long lastCleanup = 0;
  if (millis() - lastCleanup > 5000) {
    yield();
    lastCleanup = millis();
    
    // Emergency restart if critically low memory
    if (ESP.getFreeHeap() < 5000) {
      Serial.println("🚨 CRITICAL LOW MEMORY - RESTARTING...");
      
      // Save current uptime before restart
      settings.lastUptime = millis() / 60000;  // Convert to minutes
      saveSettings();
      
      // Try to send Telegram alert if WiFi is connected
      if (WiFi.status() == WL_CONNECTED) {
        String alertMsg = "🚨 CRITICAL ALERT!\n\n";
        alertMsg += "⚠️ Memory critically low: " + String(ESP.getFreeHeap()) + " bytes\n";
        alertMsg += "⏱️ Uptime: " + String(millis() / 60000) + " min\n";
        alertMsg += "🔄 System restarting automatically...\n";
        alertMsg += "📍 IP: " + WiFi.localIP().toString();
        
        bool sent = bot.sendMessage(CHAT_ID, alertMsg, "");
        Serial.print("📤 Critical alert sent: "); Serial.println(sent ? "✅ Success" : "❌ Failed");
        delay(1000);
      }
      
      // Always indicate error with LED (2 blinks = low memory)
      indicateError(2, 3);  // 2 blinks, repeat 3 times
      
      ESP.restart();
    }
  }
  
  // Watchdog protection - restart if system stuck for long time
  static unsigned long lastActivity = millis();
  
  // Update activity time regularly (every time we process something)
  if (millis() - lastTimeBotRan < botRequestDelay + 1000) {
    lastActivity = millis();  // System is active if Telegram processing is working
  }
  
  if (millis() - lastActivity > 300000) { // 5 minutes without activity
    Serial.println("🚨 SYSTEM STUCK - RESTARTING...");
    
    // Save current uptime before restart
    settings.lastUptime = millis() / 60000;  // Convert to minutes
    saveSettings();
    
    // Try to send Telegram alert if WiFi is connected
    if (WiFi.status() == WL_CONNECTED) {
      String stuckMsg = "🚨 CRITICAL ALERT!\n\n";
      stuckMsg += "⏰ System stuck detected!\n";
      stuckMsg += "🕐 No activity for 5+ minutes\n";
      stuckMsg += "⏱️ Uptime: " + String(millis() / 60000) + " min\n";
      stuckMsg += "🔄 System restarting automatically...\n";
      stuckMsg += "📍 IP: " + WiFi.localIP().toString();
      
      bool sent = bot.sendMessage(CHAT_ID, stuckMsg, "");
      Serial.print("📤 System stuck alert sent: "); Serial.println(sent ? "✅ Success" : "❌ Failed");
      delay(1000);
    }
    
    // Always indicate error with LED (3 blinks = system stuck)
    indicateError(3, 3);  // 3 blinks, repeat 3 times
    
    ESP.restart();
  }
}

 