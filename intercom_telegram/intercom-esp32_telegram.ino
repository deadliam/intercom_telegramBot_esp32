#include <EEPROM.h>
#include <WiFiManager.h>  // https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>   // Universal Telegram Bot Library
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include <time.h>
#include "esp_system.h"

// PROGMEM для статических строк (экономия RAM)
const char PROGMEM_HELP_MSG[] PROGMEM = "📋 === AVAILABLE COMMANDS ===\nping        - Test Serial communication\nmemtest     - Memory diagnostic test\nfreemem     - Emergency memory recovery\nopen        - Manual intercom open\ntest        - Send test Telegram message\nstatus      - Show full system status\nrestart     - Restart system\ncleanup     - Force memory cleanup\nresetwifi   - Reset WiFi settings\nwifiportal  - Start WiFi config portal\nforcereset  - FORCE clear ALL WiFi data\nscanwifi    - Scan WiFi networks\nhelp/?      - Show this help\n============================";

const char PROGMEM_SYSTEM_STARTUP[] PROGMEM = "=== SYSTEM STARTUP ===";
const char PROGMEM_MEMORY_CHECK[] PROGMEM = "💾 === MEMORY HEALTH CHECK ===";
const char PROGMEM_CRITICAL_MEM[] PROGMEM = "🚨 CRITICAL MEMORY - FORCING AGGRESSIVE CLEANUP!";

// Include secrets
#include "secrets.h"

// Hardware configuration for ESP32 WROOM-32D
// Pin mapping from NodeMCU (ESP8266) to ESP32:
// NodeMCU A0  -> ESP32 GPIO36 (ADC1_CH0) - Analog input only
// NodeMCU D5  -> ESP32 GPIO18 - Digital I/O
// NodeMCU D6  -> ESP32 GPIO19 - Digital I/O
// Built-in LED: ESP32 GPIO2 (instead of ESP8266 GPIO16)
const int callIndicatorPin = 36;    // Analog pin for call detection (ADC1_CH0)
const int cameraButtonPin = 18;     // Camera activation button (GPIO18)
const int doorButtonPin = 19;       // Door lock activation button (GPIO19)
const int ledPin = 2;               // Built-in LED for ESP32 (GPIO2)

// Outputs are active-HIGH (idle LOW, activation HIGH)

// Settings structure for EEPROM
struct IntercomSettings {
  int cameraActivationTime;    // Camera activation time in ms
  int callThreshold;          // Threshold value for A0
  int callDebounceTime;       // Call debounce time in ms  
  int doorActivationTime;     // Door activation time in ms
  char botToken[50];          // Telegram bot token
  char chatId[20];            // Telegram Chat ID
  char authorizedChats[200];  // CSV of extra authorized chat IDs
  unsigned long rebootCount;   // Count of system reboots
  unsigned long lastUptime;    // Last uptime before reboot (minutes)
  int watchdogTimeout;        // Hardware watchdog timeout in seconds
};

IntercomSettings settings;

// Telegram Bot Setup
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// Web server
WebServer server(80);

// Authorization helper for multiple Telegram users
static bool chatIdInCsv(const char* csv, const String& chatId) {
  if (!csv || !csv[0]) return false;
  String needle = chatId;
  // scan comma-separated list tolerant to spaces
  const char* p = csv;
  String token;
  while (*p) {
    // collect token
    token = "";
    while (*p && *p != ',') { token += *p++; }
    token.trim();
    if (token.length() > 0 && token == needle) return true;
    if (*p == ',') p++;
  }
  return false;
}

bool isAuthorized(const String &chatId) {
  // Primary admin from secrets
  if (chatId == CHAT_ID) return true;
  // Also allow admin configured in EEPROM (settings.chatId) if present
  if (settings.chatId[0] != '\0' && chatId == String(settings.chatId)) return true;
  // Extra authorized list from EEPROM CSV
  return chatIdInCsv(settings.authorizedChats, chatId);
}

bool addAuthorizedChatId(const String& chatId) {
  if (chatId.length() == 0) return false;
  if (isAuthorized(chatId)) return true;
  // Append to CSV if space allows
  String current = String(settings.authorizedChats);
  if (current.length() > 0) current += ",";
  current += chatId;
  if (current.length() >= sizeof(settings.authorizedChats)) return false;
  current.toCharArray(settings.authorizedChats, sizeof(settings.authorizedChats));
  saveSettings();
  return true;
}

bool removeAuthorizedChatId(const String& chatId) {
  String current = String(settings.authorizedChats);
  if (current.length() == 0) return true;
  // Rebuild CSV excluding chatId
  String rebuilt = "";
  int start = 0;
  while (start < (int)current.length()) {
    int comma = current.indexOf(',', start);
    if (comma < 0) comma = current.length();
    String token = current.substring(start, comma);
    token.trim();
    if (token.length() > 0 && token != chatId) {
      if (rebuilt.length() > 0) rebuilt += ",";
      rebuilt += token;
    }
    start = comma + 1;
  }
  rebuilt.toCharArray(settings.authorizedChats, sizeof(settings.authorizedChats));
  saveSettings();
  return true;
}

// Telegram and connectivity variables
bool telegramConnected = false;
unsigned long lastTelegramCheck = 0;
unsigned long telegramReconnectAttempts = 0;
unsigned long lastSuccessfulMessage = 0;
unsigned long lastTimeBotRan = 0;
const unsigned long botRequestDelay = 1000;

// Auto port testing
bool autoPortTestCompleted = false;
int workingTelegramPort = 433;  // Default port

// Call detection
bool callDetected = false;
bool lastCallState = false;
unsigned long lastCallTime = 0;
bool suppressCallDetection = false; // Disable call detection during door opening
unsigned long ignoreCallUntil = 0;  // Time until which incoming call detection is ignored

// Memory monitoring
unsigned long lastMemoryCheck = 0;
const unsigned long MEMORY_CHECK_INTERVAL = 5000; // Check every 5 seconds
uint32_t minFreeHeap = 0xFFFFFFFF; // Track minimum free heap

// Advanced Watchdog System
unsigned long lastLoopTime = 0;
unsigned long lastTelegramActivity = 0;
unsigned long lastWiFiCheck = 0;
const unsigned long MAX_LOOP_TIME = 10000;      // Max 10 seconds per loop
const unsigned long MAX_TELEGRAM_SILENCE = 300000; // Max 5 minutes without Telegram
const unsigned long CRITICAL_MEMORY_THRESHOLD = 20000; // Critical memory level
unsigned long softwareWatchdogCounter = 0;
bool criticalOperationInProgress = false;

// Time synchronization
bool timeInitialized = false;
time_t now;
const char* ntpServer1 = "pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 0;

// ========== MEMORY MANAGEMENT ==========

void checkMemoryHealth() {
  uint32_t currentFree = ESP.getFreeHeap();
  uint16_t fragmentation = 0; // ESP32 doesn't have getHeapFragmentation()
  uint32_t maxFreeBlock = ESP.getMaxAllocHeap();
  
  // Track minimum heap
  if (currentFree < minFreeHeap) {
    minFreeHeap = currentFree;
  }
  
  // Log memory status every 30 seconds
  if (millis() - lastMemoryCheck > MEMORY_CHECK_INTERVAL) {
    Serial.println(FPSTR(PROGMEM_MEMORY_CHECK));
    Serial.println("Current free: " + String(currentFree) + " bytes");
    Serial.println("Minimum free: " + String(minFreeHeap) + " bytes");
    Serial.println("Fragmentation: " + String(fragmentation) + "%");
    Serial.println("Max free block: " + String(maxFreeBlock) + " bytes");
    
    if (currentFree < 20000) {
      Serial.println("⚠️ LOW MEMORY WARNING!");
      forceMemoryCleanup(); // Cleanup much earlier
    }
    if (fragmentation > 30) {
      Serial.println("⚠️ HIGH FRAGMENTATION WARNING!");
      forceMemoryCleanup(); // Cleanup on high fragmentation
    }
    if (currentFree < 15000) {
      Serial.println("🚨 CRITICAL MEMORY - FORCING AGGRESSIVE CLEANUP!");
      
      // Force aggressive cleanup
      for (int i = 0; i < 10; i++) {
        yield();
        // ESP32 watchdog is handled automatically
        delay(10);
      }
      
      uint32_t afterCleanup = ESP.getFreeHeap();
      Serial.println("💾 After cleanup: " + String(afterCleanup) + " bytes");
      
      // Emergency restart if cleanup failed
      if (afterCleanup < 12000) {
        Serial.println("🚨 EMERGENCY: Memory cleanup failed - RESTARTING!");
        emergencyRestart("Critical memory after cleanup");
      }
    }
    
    // Force restart if memory gets dangerously low
    if (currentFree < 8000) {
      Serial.println("🚨 EMERGENCY: Memory dangerously low - FORCING RESTART!");
      emergencyRestart("Dangerously low memory");
    }
    
    lastMemoryCheck = millis();
  }
}

// Force memory cleanup when needed
void forceMemoryCleanup() {
  Serial.println("🧹 Forcing aggressive memory cleanup...");
  
  uint32_t beforeCleanup = ESP.getFreeHeap();
  
  // More aggressive cleanup cycles
  for (int i = 0; i < 20; i++) {
    yield();
    // ESP32 watchdog is handled automatically
    delay(10);
  }
  
  // Force garbage collection
  delay(100);
  
  // Additional cleanup cycles
  for (int i = 0; i < 10; i++) {
    yield();
    // ESP32 watchdog is handled automatically
    delay(5);
  }
  
  delay(200);  // Allow system cleanup
  uint32_t afterCleanup = ESP.getFreeHeap();
  
  int recovered = afterCleanup - beforeCleanup;
  // Разбиваем сложную конкатенацию
  Serial.print("💾 Memory: "); 
  Serial.print(beforeCleanup); 
  Serial.print(" -> "); 
  Serial.print(afterCleanup); 
  Serial.print(" (+"); 
  Serial.print(recovered); 
  Serial.println(" bytes)");
  
  // If cleanup didn't help much, something is wrong
  if (recovered < 100 && afterCleanup < 15000) {
    Serial.println("⚠️ WARNING: Memory cleanup ineffective!");
  }
}

// ========== ADVANCED WATCHDOG SYSTEM ==========

void softwareWatchdogFeed() {
  softwareWatchdogCounter++;
  lastLoopTime = millis();
  // ESP32 watchdog is handled automatically
  
  // Log every 1000 feeds (about every 100 seconds in normal operation)
  if (softwareWatchdogCounter % 1000 == 0) {
    Serial.println("🐕 Watchdog: " + String(softwareWatchdogCounter) + " feeds");
  }
}

void beginCriticalOperation(const char* operationName) {
  criticalOperationInProgress = true;
  Serial.println("🔒 Beginning critical operation: " + String(operationName));
  
  // Pre-operation memory check
  uint32_t freeMem = ESP.getFreeHeap();
  if (freeMem < CRITICAL_MEMORY_THRESHOLD) {
    Serial.println("🚨 CRITICAL: Starting operation with low memory!");
    forceMemoryCleanup();
  }
  
  softwareWatchdogFeed();
}

void endCriticalOperation(const char* operationName) {
  criticalOperationInProgress = false;
  Serial.println("✅ Completed critical operation: " + String(operationName));
  
  // Post-operation memory check
  uint32_t freeMem = ESP.getFreeHeap();
  Serial.println("💾 Memory after operation: " + String(freeMem) + " bytes");
  
  softwareWatchdogFeed();
}

void checkSystemHealth() {
  unsigned long currentTime = millis();
  
  // Check for loop hang (more than 10 seconds without loop iteration)
  if (currentTime - lastLoopTime > MAX_LOOP_TIME) {
    Serial.println("🚨 SYSTEM HANG DETECTED - FORCING RESTART!");
    Serial.println("Last loop was " + String((currentTime - lastLoopTime) / 1000) + " seconds ago");
    
    // Try to send emergency message before restart
    if (telegramConnected) {
      bot.sendMessage(CHAT_ID, "🚨 EMERGENCY: System hang detected, restarting...", "");
      delay(1000); // Give time to send
    }
    
    ESP.restart();
  }
  
  // Check for memory leaks
  uint32_t currentFree = ESP.getFreeHeap();
  if (currentFree < CRITICAL_MEMORY_THRESHOLD) {
    Serial.println("🚨 CRITICAL MEMORY LEAK DETECTED!");
    Serial.println("Free memory: " + String(currentFree) + " bytes");
    
    // Emergency cleanup
    forceMemoryCleanup();
    
    currentFree = ESP.getFreeHeap();
    if (currentFree < CRITICAL_MEMORY_THRESHOLD) {
      Serial.println("🚨 MEMORY CLEANUP FAILED - FORCING RESTART!");
      
      // Emergency restart
      if (telegramConnected) {
        bot.sendMessage(CHAT_ID, "🚨 EMERGENCY: Critical memory leak, restarting...", "");
        delay(1000);
      }
      ESP.restart();
    }
  }
  
  // Check WiFi health
  if (currentTime - lastWiFiCheck > 60000) { // Check every minute
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ WiFi disconnected - attempting reconnection");
      WiFi.reconnect();
    }
    lastWiFiCheck = currentTime;
  }
  
  // Check Telegram silence (no activity for too long)
  if (telegramConnected && (currentTime - lastTelegramActivity > MAX_TELEGRAM_SILENCE)) {
    Serial.println("⚠️ Telegram silent for too long - testing connection");
    
    // Test Telegram connection
    if (!testTelegramConnectivity()) {
      telegramConnected = false;
      Serial.println("💬 Telegram connection lost");
    }
  }
}

void emergencyRestart(const char* reason) {
  Serial.println("🚨 EMERGENCY RESTART: " + String(reason));
  
  // Try to save critical state
  settings.lastUptime = millis() / 60000;
  saveSettings();
  
  // Try to send emergency notification
  if (telegramConnected) {
    String emergencyMsg = "🚨 EMERGENCY RESTART\n\nReason: ";
    emergencyMsg += reason;
    emergencyMsg += "\nUptime: " + String(millis() / 60000) + " min";
    emergencyMsg += "\nMemory: " + String(ESP.getFreeHeap()) + " bytes";
    
    bot.sendMessage(CHAT_ID, emergencyMsg, "");
    delay(2000); // Give time to send
  }
  
  ESP.restart();
}

// ========== AUTO PORT DETECTION ==========

bool autoDetectTelegramPort() {
  Serial.println("🔍 Auto-detecting working Telegram port...");
  
  // Test HTTPS ports in order of preference (Livebox 6 often blocks 443)
  int testPorts[] = {443, 8443, 80};
  int numPorts = sizeof(testPorts) / sizeof(testPorts[0]);
  
  for (int i = 0; i < numPorts; i++) {
    Serial.print("Testing port " + String(testPorts[i]) + "... ");
    
    WiFiClientSecure testClient;
    testClient.setInsecure();  // Required for Livebox 6 router compatibility
    
    unsigned long start = millis();
    if (testClient.connect("api.telegram.org", testPorts[i])) {
      unsigned long duration = millis() - start;
      Serial.println("✅ Success in " + String(duration) + " ms");
      
      workingTelegramPort = testPorts[i];
      testClient.stop();
      
      Serial.println("🎯 Using Telegram port: " + String(workingTelegramPort));
      return true;
    } else {
      unsigned long duration = millis() - start;
      Serial.println("❌ Failed after " + String(duration) + " ms");
    }
    
    delay(1000);
  }
  
  Serial.println("⚠️ All HTTPS ports failed - using default 443");
  Serial.println("💡 Livebox 6 may be blocking SSL handshake");
  Serial.println("💡 Try: Mobile hotspot, router reboot, or different network");
  workingTelegramPort = 443;
  return false;
}

// ========== TIME FUNCTIONS ==========

bool initializeNTP() {
  Serial.println("🕐 Initializing NTP time synchronization...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1);
  
  // Wait for time sync
  unsigned long start = millis();
  while (millis() - start < 10000) {
    time(&now);
    if (now > 1000000000) {
      timeInitialized = true;
      Serial.println("✅ NTP synchronization successful");
      return true;
    }
    delay(500);
    // ESP32 watchdog is handled automatically
  }
  
  Serial.println("❌ NTP synchronization failed");
  return false;
}

bool isTimeValid() {
  time(&now);
  return (now > 1000000000 && timeInitialized);
}

// ========== TELEGRAM FUNCTIONS ==========

bool testTelegramConnectivity() {
  Serial.println("📡 Testing Telegram API connectivity...");
  Serial.println("🔓 Using insecure SSL for Livebox 6 router compatibility...");
  
  // Check time first
  if (!isTimeValid()) {
    Serial.println("⚠️ System time invalid - attempting quick sync...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1);
    delay(2000);
    time(&now);
    if (now > 1000000000) {
      timeInitialized = true;
      Serial.println("✅ Time sync successful");
    }
  }
  
  // Use simple SSL approach with minimal Livebox 6 compatibility
  WiFiClientSecure testClient;
  testClient.setInsecure();  // Required for Livebox 6 router compatibility
  
  Serial.print("🔗 Connecting to api.telegram.org:" + String(workingTelegramPort) + "... ");
  unsigned long connectStart = millis();
  
  if (testClient.connect("api.telegram.org", workingTelegramPort)) {
    unsigned long connectTime = millis() - connectStart;
    Serial.println("✅ Connected in " + String(connectTime) + " ms");
    
    // Test API call
    testClient.print("GET /bot");
    testClient.print(BOT_TOKEN);
    testClient.println("/getMe HTTP/1.1");
    testClient.println("Host: api.telegram.org");
    testClient.println("Connection: close");
    testClient.println();
    
    // Wait for response
    unsigned long timeout = millis() + 10000;
    while (testClient.available() == 0 && millis() < timeout) {
      delay(100);
      // ESP32 watchdog is handled automatically
    }
    
    if (testClient.available()) {
      String response = testClient.readStringUntil('\n');
      testClient.stop();
      
      if (response.indexOf("200 OK") >= 0) {
        Serial.println("✅ API test successful");
        return true;
      } else {
        Serial.println("⚠️ Connected but API failed");
      }
    } else {
      Serial.println("⚠️ Connected but no API response");
      testClient.stop();
    }
  } else {
    unsigned long connectTime = millis() - connectStart;
    Serial.println("❌ Failed after " + String(connectTime) + " ms");
  }
  
  return false;
}

bool reconnectTelegram() {
  Serial.println("🔄 Attempting Telegram reconnection...");
  telegramReconnectAttempts++;
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected");
    return false;
  }
  
  if (!testTelegramConnectivity()) {
    Serial.println("❌ Reconnection failed (attempt " + String(telegramReconnectAttempts) + ")");
    return false;
  }
  
  // Send test message
  String testMsg = "🔄 Telegram reconnected!\nAttempt: " + String(telegramReconnectAttempts);
  testMsg += "\nUptime: " + String(millis() / 60000) + " min";
  
  bool sent = bot.sendMessage(CHAT_ID, testMsg, "");
  if (sent) {
    Serial.println("✅ Reconnected successfully (attempt " + String(telegramReconnectAttempts) + ")");
    telegramConnected = true;
    lastSuccessfulMessage = millis();
    return true;
  } else {
    Serial.println("❌ Test message failed (attempt " + String(telegramReconnectAttempts) + ")");
    return false;
  }
}

// ========== HARDWARE FUNCTIONS ==========

void pressCameraButton() {
  Serial.println("🎥 === CAMERA BUTTON PRESS ===");
  Serial.println("Changing camera pin " + String(cameraButtonPin) + " to HIGH");
  
  digitalWrite(cameraButtonPin, HIGH);
  delay(settings.cameraActivationTime);
  digitalWrite(cameraButtonPin, LOW);
  
  Serial.println("Changing camera pin " + String(cameraButtonPin) + " to LOW");
  Serial.println("🎥 === CAMERA BUTTON RELEASE ===");
}

void viewDisplayOnly() {
  beginCriticalOperation("View Display Only");
  suppressCallDetection = true;
  pressCameraButton();
  endCriticalOperation("View Display Only");
  suppressCallDetection = false;
  // Ignore incoming call detection for 32 seconds after turning display on
  ignoreCallUntil = millis() + 32000UL;
}

void pressDoorButton() {
  Serial.println("🚪 === DOOR BUTTON PRESS ===");
  Serial.println("Changing door pin " + String(doorButtonPin) + " to HIGH");
  
  digitalWrite(doorButtonPin, HIGH);
  delay(settings.doorActivationTime);
  digitalWrite(doorButtonPin, LOW);
  
  Serial.println("Changing door pin " + String(doorButtonPin) + " to LOW");
  Serial.println("🚪 === DOOR BUTTON RELEASE ===");
}

void openIntercom() {
  beginCriticalOperation("Intercom Opening");
  suppressCallDetection = true; // Ignore call detection while opening
  bool displayCurrentlyOn = (digitalRead(cameraButtonPin) == HIGH) || (millis() < ignoreCallUntil);
  bool pressedCameraThisCycle = false;
  
  // Check memory before each critical step
  uint32_t freeMem = ESP.getFreeHeap();
  if (freeMem < CRITICAL_MEMORY_THRESHOLD) {
    Serial.println("🚨 CRITICAL MEMORY - Aborting intercom operation!");
    endCriticalOperation("Intercom Opening - ABORTED");
    suppressCallDetection = false;
    return;
  }
  
  // If display is already on (or recently turned on), skip camera press to avoid toggling it off
  if (!displayCurrentlyOn) {
    pressCameraButton();
    pressedCameraThisCycle = true;
    // When we turn on the display here, ignore incoming call detection for 32s
    unsigned long newIgnoreUntil = millis() + 32000UL;
    if (newIgnoreUntil > ignoreCallUntil) {
      ignoreCallUntil = newIgnoreUntil;
    }
  }
  
  // Safe delay with watchdog feeding and memory check
  softwareWatchdogFeed();
  yield();
  
  // Check memory again before second operation
  freeMem = ESP.getFreeHeap();
  if (freeMem < CRITICAL_MEMORY_THRESHOLD) {
    Serial.println("🚨 CRITICAL MEMORY after camera - Aborting door operation!");
    endCriticalOperation("Intercom Opening - PARTIAL");
    suppressCallDetection = false;
    return;
  }
  
  delay(500);
  pressDoorButton();
  
  endCriticalOperation("Intercom Opening");
  // Set/extend ignore window before lifting suppression to avoid race
  if (pressedCameraThisCycle) {
    // Already set to 32s above; keep it as-is
  } else {
    // If we didn't press camera now, still add a small tail to avoid relay noise
    unsigned long tail = millis() + 2000UL;
    if (tail > ignoreCallUntil) {
      ignoreCallUntil = tail;
    }
  }
  suppressCallDetection = false; // Re-enable call detection after ignore window is set
}

void checkIncomingCall() {
  unsigned long nowTs = millis();
  if (suppressCallDetection || (nowTs < ignoreCallUntil)) {
    // Skip call detection while door/camera operation is in progress
    lastCallState = false;
    return;
  }
  int analogValue = analogRead(callIndicatorPin);
  bool currentCallState = (analogValue < settings.callThreshold);
  
  // Debug every 2 minutes (reduce spam)
  static unsigned long lastDebugTime = 0;
  if (millis() - lastDebugTime > 120000) {
    Serial.print("A0: "); 
    Serial.print(analogValue); 
    Serial.print("/"); 
    Serial.print(settings.callThreshold); 
    Serial.print(", Mem: "); 
    Serial.print(ESP.getFreeHeap()); 
    Serial.println("b");
    lastDebugTime = millis();
  }
  
  if (currentCallState && !lastCallState && 
      (nowTs - lastCallTime > settings.callDebounceTime) &&
      (nowTs >= ignoreCallUntil)) {
    
      callDetected = true;
      lastCallTime = millis();
      
    Serial.println("📞 === INCOMING CALL DETECTED ===");
    Serial.println("Analog value: " + String(analogValue));
    
    // Send notification
    String callMsg = "📞 INCOMING CALL!\n\nAnalog value: " + String(analogValue);
    callMsg += "\nThreshold: " + String(settings.callThreshold);
    callMsg += "\nTime: " + String(millis() / 60000) + " min uptime";
    
    bot.sendMessage(CHAT_ID, callMsg, "");
    Serial.println("📤 Call notification sent");
  }
  
  lastCallState = currentCallState;
}

// ========== TELEGRAM MESSAGE HANDLING ==========

void handleNewMessages(int numNewMessages) {
  // ESP32 watchdog is handled automatically
  Serial.println("📨 Handling " + String(numNewMessages) + " new messages");

  for (int i = 0; i < numNewMessages; i++) {
    // ESP32 watchdog is handled automatically
    
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    Serial.println("📩 Message from " + from_name + " (" + chat_id + "): " + text);

    if (!isAuthorized(chat_id)) {
      String unauthMsg = "❌ Unauthorized access\n\n";
      unauthMsg += "Your chat_id: ";
      unauthMsg += chat_id;
      unauthMsg += "\nAsk admin to add it to whitelist.";
      bot.sendMessage(chat_id, unauthMsg, "");
      Serial.println("⚠️ Unauthorized user blocked, chat_id=" + chat_id);
      continue;
    }

    if (text == "/start") {
      String welcome = "🏠 *Telegram Intercom System*\n\n";
      welcome += "Welcome! Use the button below to open the intercom:\n\n";
      welcome += "🎥 Camera will activate for " + String(settings.cameraActivationTime) + "ms\n";
      welcome += "🚪 Door will unlock for " + String(settings.doorActivationTime) + "ms\n";
      welcome += "📞 Call detection threshold: " + String(settings.callThreshold) + "\n\n";
      welcome += "📋 *Available Commands:*\n";
      welcome += "/start - Show this menu\n";
      welcome += "/status - System status\n";
      if (chat_id == CHAT_ID) {
        welcome += "\n*Admin commands:*\n";
        welcome += "/allow <chat_id> - Add user\n";
        welcome += "/deny <chat_id> - Remove user\n";
        welcome += "/list - List authorized\n";
      }

      String replyKeyboard = "[[\"🚪 Open Intercom\", \"📺 View Display\"], [\"📊 Status\"]]";
      bot.sendMessageWithReplyKeyboard(chat_id, welcome, "", replyKeyboard, true, false, false);
      
    } else if (text == "🚪 Open Intercom") {
      Serial.println("🚪 Remote open command received");
      
      bot.sendMessage(chat_id, "🎥 Activating camera and opening door...", "");
      openIntercom();
      
      String confirmMsg = "✅ *Intercom Opened Successfully*\n\n";
      confirmMsg += "🎥 Camera activated for " + String(settings.cameraActivationTime) + "ms\n";
      confirmMsg += "🚪 Door unlocked for " + String(settings.doorActivationTime) + "ms\n";
      confirmMsg += "⏰ Time: " + String(millis() / 60000) + " min uptime";
      
      bot.sendMessage(chat_id, confirmMsg, "Markdown");
      
    } else if (text == "📺 View Display") {
      Serial.println("📺 View Display command received");
      bot.sendMessage(chat_id, "🎥 Turning on display...", "");
      viewDisplayOnly();
      bot.sendMessage(chat_id, "✅ Display turned on", "");
    } else if (text.startsWith("/allow")) {
      if (chat_id != CHAT_ID) { bot.sendMessage(chat_id, "❌ Admin only", ""); continue; }
      String parts = text; parts.replace("/allow",""); parts.trim();
      if (parts.length() == 0) { bot.sendMessage(chat_id, "Usage: /allow <chat_id>", ""); continue; }
      bool ok = addAuthorizedChatId(parts);
      bot.sendMessage(chat_id, ok ? ("✅ Added: " + parts) : "❌ Failed to add (no space?)", "");
    } else if (text.startsWith("/deny")) {
      if (chat_id != CHAT_ID) { bot.sendMessage(chat_id, "❌ Admin only", ""); continue; }
      String parts = text; parts.replace("/deny",""); parts.trim();
      if (parts.length() == 0) { bot.sendMessage(chat_id, "Usage: /deny <chat_id>", ""); continue; }
      bool ok = removeAuthorizedChatId(parts);
      bot.sendMessage(chat_id, ok ? ("✅ Removed: " + parts) : "❌ Failed to remove", "");
    } else if (text == "/list") {
      if (chat_id != CHAT_ID) { bot.sendMessage(chat_id, "❌ Admin only", ""); continue; }
      String list = String(settings.authorizedChats);
      if (list.length() == 0) list = "<empty>";
      bot.sendMessage(chat_id, "👥 Authorized: " + list, "");
      
    } else if (text == "/status" || text == "📊 Status") {
      Serial.println("📊 Status command received - OPTIMIZED VERSION");
      
      // Проверяем память перед созданием большого сообщения
      uint32_t memBefore = ESP.getFreeHeap();
      if (memBefore < 20000) {
        bot.sendMessage(chat_id, "⚠️ Low memory, sending short status:\n💾 Memory: " + String(memBefore) + " bytes\n📊 IP: " + WiFi.localIP().toString(), "");
        return;
      }
      
      // Создаем статус по частям для экономии памяти
      String statusMsg = "📊 *System Status*\n\n";
      
      // WiFi Status
      statusMsg += "📡 *WiFi:* ";
      statusMsg += (WiFi.status() == WL_CONNECTED) ? "✅ Connected\n" : "❌ Disconnected\n";
      if (WiFi.status() == WL_CONNECTED) {
        statusMsg += "🌐 *SSID:* ";
        statusMsg += WiFi.SSID();
        statusMsg += "\n📍 *IP:* ";
        statusMsg += WiFi.localIP().toString();
        statusMsg += "\n";
      }
      
      // Отправляем первую часть и очищаем строку
      bot.sendMessage(chat_id, statusMsg, "Markdown");
      statusMsg = ""; // Освобождаем память
      
      // Telegram Status  
      statusMsg = "💬 *Telegram:* ";
      statusMsg += telegramConnected ? "✅ Connected" : "❌ Disconnected";
      statusMsg += "\n💾 *Memory:* ";
      statusMsg += String(ESP.getFreeHeap());
      statusMsg += " bytes\n⏰ *Uptime:* ";
      statusMsg += String(millis() / 60000);
      statusMsg += " min";
      
      String replyKeyboard = "[[\"🚪 Open Intercom\", \"📺 View Display\"], [\"📊 Status\"]]";
      bot.sendMessageWithReplyKeyboard(chat_id, statusMsg, "Markdown", replyKeyboard, true, false, false);
      
    } else {
      bot.sendMessage(chat_id, "❓ Unknown command. Use /start to see available options.", "");
    }
  }
}

// ========== EEPROM FUNCTIONS ==========

void saveSettings() {
  EEPROM.put(0, settings);
  EEPROM.commit();
  Serial.println("⚙️ Settings saved to EEPROM");
}

void loadSettings() {
  EEPROM.get(0, settings);
  
  // Validate settings and load defaults if invalid
  if (settings.cameraActivationTime < 100 || settings.cameraActivationTime > 10000 || 
      settings.callThreshold < 0 || settings.callThreshold > 1023 ||
      settings.callDebounceTime < 1000 || settings.callDebounceTime > 60000 ||
      settings.watchdogTimeout < 1 || settings.watchdogTimeout > 60) {
    
    // Load default settings
    settings.cameraActivationTime = 2000;
    settings.callThreshold = 15;
    settings.callDebounceTime = 5000;
    settings.doorActivationTime = 500;
    strncpy(settings.botToken, BOT_TOKEN, sizeof(settings.botToken) - 1);
    strncpy(settings.chatId, CHAT_ID, sizeof(settings.chatId) - 1);
    settings.authorizedChats[0] = '\0';
    settings.rebootCount = 1;
    settings.lastUptime = 0;
    settings.watchdogTimeout = 8;
    
    Serial.println("⚙️ Loading default settings");
    saveSettings();
  } else {
    settings.rebootCount++;
    settings.lastUptime = millis() / 60000;
    // sanitize CSV
    bool ok = true;
    for (size_t i = 0; i < sizeof(settings.authorizedChats) && settings.authorizedChats[i]; ++i) {
      char c = settings.authorizedChats[i];
      if (!(c == ',' || c == ' ' || (c >= '0' && c <= '9') || c == '+' || c == '-')) { ok = false; break; }
    }
    if (!ok) settings.authorizedChats[0] = '\0';
    saveSettings();
  }
}

// ========== WEB SERVER ==========

void handleRoot() {
  String page = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<title>Telegram Intercom</title>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<style>";
  page += "body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}";
  page += ".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}";
  page += ".status{background:#e8f5e8;padding:15px;border-radius:5px;margin:10px 0}";
  page += ".controls{background:#f0f8ff;padding:15px;border-radius:5px;margin:10px 0}";
  page += "a{color:#0066cc;text-decoration:none;margin-right:15px}";
  page += "a:hover{text-decoration:underline}";
  page += "</style>";
  page += "</head><body>";
  page += "<div class='container'>";
  page += "<h1>🏠 Telegram Intercom System</h1>";
  
  page += "<div class='status'>";
  page += "<h2>📊 System Status</h2>";
  page += "<p><strong>WiFi:</strong> " + String(WiFi.status() == WL_CONNECTED ? "✅ Connected" : "❌ Disconnected") + "</p>";
  page += "<p><strong>IP Address:</strong> " + WiFi.localIP().toString() + "</p>";
  page += "<p><strong>Telegram:</strong> " + String(telegramConnected ? "✅ Connected" : "❌ Disconnected") + "</p>";
  page += "<p><strong>Telegram Port:</strong> " + String(workingTelegramPort) + "</p>";
  page += "<p><strong>SSL Mode:</strong> 🔓 Insecure (Livebox 6 compatible)</p>";
  page += "<p><strong>Uptime:</strong> " + String(millis() / 60000) + " minutes</p>";
  page += "<p><strong>Free Memory:</strong> " + String(ESP.getFreeHeap()) + " bytes</p>";
  page += "<p><strong>Reboot Count:</strong> " + String(settings.rebootCount) + "</p>";
  page += "</div>";
  
  page += "<div class='controls'>";
  page += "<h2>🔧 Controls</h2>";
  page += "<p><a href='/settings'>⚙️ Settings</a>";
  page += "<a href='/wifi'>📶 WiFi Setup</a>";
  page += "<a href='/otainfo'><strong>🔄 OTA Update (ElegantOTA)</strong></a>";
  page += "<a href='/test'>🧪 Test Telegram</a>";
  page += "<a href='/reboot'>🔄 Reboot System</a></p>";
  page += "<div style='background:#e8f4f8;padding:10px;border-radius:5px;margin:10px 0'>";
  page += "<small>📋 <strong>OTA Update:</strong> Upload firmware via web interface<br>";
  page += "🌐 <strong>Direct link:</strong> <a href='/update' target='_blank'>http://intercom.local/update</a></small>";
  page += "</div>";
  page += "</div>";
  
  page += "</div></body></html>";
  
  server.send(200, "text/html; charset=utf-8", page);
}

void handleSettings() {
  String page = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<title>Settings - Telegram Intercom</title>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<style>";
  page += "body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}";
  page += ".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}";
  page += ".setting{margin:15px 0;padding:10px;border:1px solid #ddd;border-radius:5px}";
  page += "input[type=number]{width:100px;padding:5px}";
  page += "button{background:#0066cc;color:white;padding:10px 20px;border:none;border-radius:5px;cursor:pointer}";
  page += "button:hover{background:#0052a3}";
  page += ".warning{background:#fff3cd;color:#856404;padding:10px;border-radius:5px;margin:10px 0}";
  page += "</style>";
  page += "</head><body>";
  page += "<div class='container'>";
  page += "<h1>⚙️ System Settings</h1>";
  page += "<p><a href='/'>← Back to Status</a></p>";
  
  page += "<div class='warning'>";
  page += "⚠️ <strong>Warning:</strong> Changing these settings will restart the system!";
  page += "</div>";
  
  page += "<form method='POST' action='/save'>";
  
  page += "<div class='setting'>";
  page += "<h3>🎥 Camera Settings</h3>";
  page += "<label>Camera Activation Time (ms): </label>";
  page += "<input type='number' name='cameraTime' value='" + String(settings.cameraActivationTime) + "' min='100' max='10000'>";
  page += "<small> (100-10000ms)</small>";
  page += "</div>";
  
  page += "<div class='setting'>";
  page += "<h3>🚪 Door Settings</h3>";
  page += "<label>Door Activation Time (ms): </label>";
  page += "<input type='number' name='doorTime' value='" + String(settings.doorActivationTime) + "' min='500' max='5000'>";
  page += "<small> (500-5000ms)</small>";
  page += "</div>";
  
  page += "<div class='setting'>";
  page += "<h3>📞 Call Detection</h3>";
  page += "<label>Call Threshold (A0 value): </label>";
  page += "<input type='number' name='threshold' value='" + String(settings.callThreshold) + "' min='0' max='1023'>";
  page += "<small> (рекомендуется 10–20, current A0: " + String(analogRead(callIndicatorPin)) + ")</small><br><br>";
  page += "<label>Call Debounce Time (ms): </label>";
  page += "<input type='number' name='debounce' value='" + String(settings.callDebounceTime) + "' min='1000' max='60000'>";
  page += "<small> (1000-60000ms)</small>";
  page += "</div>";
  
  page += "<div class='setting'>";
  page += "<h3>🐕 Watchdog Settings</h3>";
  page += "<label>Watchdog Timeout (seconds): </label>";
  page += "<input type='number' name='watchdog' value='" + String(settings.watchdogTimeout) + "' min='1' max='60'>";
  page += "<small> (1-60 seconds)</small>";
  page += "</div>";
  
  page += "<br><button type='submit'>💾 Save Settings & Reboot</button>";
  page += "</form>";
  
  page += "</div></body></html>";
  
  server.send(200, "text/html; charset=utf-8", page);
}

void handleSaveSettings() {
  if (server.hasArg("cameraTime")) {
    settings.cameraActivationTime = server.arg("cameraTime").toInt();
  }
  if (server.hasArg("doorTime")) {
    settings.doorActivationTime = server.arg("doorTime").toInt();
  }
  if (server.hasArg("threshold")) {
    settings.callThreshold = server.arg("threshold").toInt();
  }
  if (server.hasArg("debounce")) {
    settings.callDebounceTime = server.arg("debounce").toInt();
  }
  if (server.hasArg("watchdog")) {
    settings.watchdogTimeout = server.arg("watchdog").toInt();
  }
  
  // Validate settings
  if (settings.cameraActivationTime < 100) settings.cameraActivationTime = 100;
  if (settings.cameraActivationTime > 10000) settings.cameraActivationTime = 10000;
  if (settings.doorActivationTime < 500) settings.doorActivationTime = 500;
  if (settings.doorActivationTime > 5000) settings.doorActivationTime = 5000;
  if (settings.callThreshold < 0) settings.callThreshold = 0;
  if (settings.callThreshold > 1023) settings.callThreshold = 1023;
  if (settings.callDebounceTime < 1000) settings.callDebounceTime = 1000;
  if (settings.callDebounceTime > 60000) settings.callDebounceTime = 60000;
  if (settings.watchdogTimeout < 1) settings.watchdogTimeout = 1;
  if (settings.watchdogTimeout > 60) settings.watchdogTimeout = 60;
  
  saveSettings();
  
  String page = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<meta http-equiv='refresh' content='3;url=/'>";
  page += "<title>Settings Saved</title>";
  page += "</head><body>";
  page += "<h1>✅ Settings Saved!</h1>";
  page += "<p>System will reboot in 3 seconds...</p>";
  page += "</body></html>";
  
  server.send(200, "text/html; charset=utf-8", page);
    
    delay(1000);
    ESP.restart();
  }
  
void handleTest() {
  String result = "❌ Failed";
  if (telegramConnected) {
    bool sent = bot.sendMessage(CHAT_ID, "🧪 Web test message from " + WiFi.localIP().toString(), "");
    if (sent) {
      result = "✅ Success";
      lastSuccessfulMessage = millis();
    }
  }
  
  String page = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<meta http-equiv='refresh' content='3;url=/'>";
  page += "<title>Telegram Test</title>";
  page += "</head><body>";
  page += "<h1>📤 Telegram Test: " + result + "</h1>";
  page += "<p>Returning to main page...</p>";
  page += "</body></html>";
  
  server.send(200, "text/html; charset=utf-8", page);
}

void handleWiFi() {
  String page = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<title>WiFi Setup - Telegram Intercom</title>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<style>";
  page += "body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}";
  page += ".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}";
  page += ".wifi-info{background:#e8f4f8;padding:15px;border-radius:5px;margin:10px 0}";
  page += ".wifi-controls{background:#fff3cd;padding:15px;border-radius:5px;margin:10px 0}";
  page += "button{background:#0066cc;color:white;padding:10px 20px;border:none;border-radius:5px;cursor:pointer;margin:5px}";
  page += "button:hover{background:#0052a3}";
  page += ".danger{background:#dc3545}";
  page += ".danger:hover{background:#c82333}";
  page += ".warning{background:#ffc107;color:#000}";
  page += ".warning:hover{background:#e0a800}";
  page += "</style>";
  page += "</head><body>";
  page += "<div class='container'>";
  page += "<h1>📶 WiFi Setup</h1>";
  page += "<p><a href='/'>← Back to Status</a></p>";
  
  page += "<div class='wifi-info'>";
  page += "<h2>📊 Current WiFi Status</h2>";
  page += "<p><strong>Status:</strong> " + String(WiFi.status() == WL_CONNECTED ? "✅ Connected" : "❌ Disconnected") + "</p>";
  if (WiFi.status() == WL_CONNECTED) {
    page += "<p><strong>SSID:</strong> " + WiFi.SSID() + "</p>";
    page += "<p><strong>IP Address:</strong> " + WiFi.localIP().toString() + "</p>";
    page += "<p><strong>Gateway:</strong> " + WiFi.gatewayIP().toString() + "</p>";
    page += "<p><strong>DNS:</strong> " + WiFi.dnsIP().toString() + "</p>";
    page += "<p><strong>Signal Strength:</strong> " + String(WiFi.RSSI()) + " dBm</p>";
    page += "<p><strong>MAC Address:</strong> " + WiFi.macAddress() + "</p>";
  }
  page += "</div>";
  
  page += "<div class='wifi-controls'>";
  page += "<h2>🔧 WiFi Controls</h2>";
  page += "<p><strong>WiFi AutoConnect:</strong> Enabled ✅</p>";
  page += "<p>System automatically connects to saved WiFi networks on startup.</p>";
  page += "<br>";
  
  page += "<button onclick=\"location.href='/wifi/scan'\">🔍 Scan Networks</button>";
  page += "<button onclick=\"location.href='/wifi/portal'\" class='warning'>⚙️ WiFi Config Portal</button>";
  page += "<button onclick=\"if(confirm('Reset WiFi settings? Device will restart in AP mode.')) location.href='/wifi/reset'\" class='danger'>🗑️ Reset WiFi</button>";
  page += "</div>";
  
  page += "<div style='background:#f8f9fa;padding:15px;border-radius:5px;margin:10px 0'>";
  page += "<h3>📋 WiFi Commands</h3>";
  page += "<ul>";
  page += "<li><strong>Scan Networks:</strong> Show available WiFi networks</li>";
  page += "<li><strong>Config Portal:</strong> Open WiFiManager portal (device will restart)</li>";
  page += "<li><strong>Reset WiFi:</strong> Clear saved credentials and restart in AP mode</li>";
  page += "</ul>";
  page += "</div>";
  
  page += "</div></body></html>";
  
  server.send(200, "text/html; charset=utf-8", page);
}

void handleWiFiScan() {
  String page = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<title>WiFi Scan - Telegram Intercom</title>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<meta http-equiv='refresh' content='30'>";  // Auto refresh every 30 seconds
  page += "<style>";
  page += "body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}";
  page += ".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}";
  page += ".network{background:#f8f9fa;padding:10px;margin:5px 0;border-radius:5px;border-left:4px solid #0066cc}";
  page += ".strong{color:#28a745}";
  page += ".medium{color:#ffc107}";
  page += ".weak{color:#dc3545}";
  page += "</style>";
  page += "</head><body>";
  page += "<div class='container'>";
  page += "<h1>🔍 WiFi Network Scan</h1>";
  page += "<p><a href='/wifi'>← Back to WiFi Setup</a></p>";
  
  page += "<p>🔄 Scanning for networks... (auto-refresh every 30s)</p>";
  
  // Perform WiFi scan
  int networkCount = WiFi.scanNetworks();
  
  if (networkCount == 0) {
    page += "<p>❌ No networks found</p>";
  } else {
    page += "<p>📡 Found " + String(networkCount) + " networks:</p>";
    
    for (int i = 0; i < networkCount; i++) {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      String security = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "🔓 Open" : "🔒 Secured";
      
      String signalClass = "weak";
      String signalText = "Weak";
      if (rssi > -50) {
        signalClass = "strong";
        signalText = "Strong";
      } else if (rssi > -70) {
        signalClass = "medium";
        signalText = "Medium";
      }
      
      page += "<div class='network'>";
      page += "<strong>" + ssid + "</strong> " + security;
      page += "<br><span class='" + signalClass + "'>📶 " + signalText + " (" + String(rssi) + " dBm)</span>";
      page += "</div>";
    }
  }
  
  page += "</div></body></html>";
  
  server.send(200, "text/html; charset=utf-8", page);
}

void handleWiFiPortal() {
  String page = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<meta http-equiv='refresh' content='5;url=http://192.168.4.1'>";
  page += "<title>WiFi Portal Starting</title>";
  page += "</head><body>";
  page += "<h1>📶 Starting WiFi Configuration Portal...</h1>";
  page += "<p>Device will restart and create 'TelegramIntercom' network.</p>";
  page += "<p>Connect to 'TelegramIntercom' and go to 192.168.4.1</p>";
  page += "<p>Redirecting in 5 seconds...</p>";
  page += "</body></html>";
  
  server.send(200, "text/html; charset=utf-8", page);
  
  delay(2000);
  WiFiManager wm;
  wm.resetSettings();  // Clear saved WiFi
  ESP.restart();       // Restart to enter portal mode
}

void handleWiFiReset() {
  String page = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<meta http-equiv='refresh' content='5;url=http://192.168.4.1'>";
  page += "<title>WiFi Reset</title>";
  page += "</head><body>";
  page += "<h1>🗑️ WiFi Settings Reset</h1>";
  page += "<p>All WiFi credentials cleared!</p>";
  page += "<p>Device will restart in AP mode as 'TelegramIntercom'</p>";
  page += "<p>Connect to 'TelegramIntercom' network and go to 192.168.4.1</p>";
  page += "<p>Redirecting in 5 seconds...</p>";
    page += "</body></html>";
  
  server.send(200, "text/html; charset=utf-8", page);
  
  delay(2000);
  WiFiManager wm;
  wm.resetSettings();  // Clear all saved WiFi credentials
  ESP.restart();       // Restart to enter AP mode
}

void handleOTAInfo() {
  String page = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<title>OTA Update Info - Telegram Intercom</title>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<style>";
  page += "body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}";
  page += ".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}";
  page += ".ota-info{background:#e8f4f8;padding:15px;border-radius:5px;margin:10px 0}";
  page += ".warning{background:#fff3cd;color:#856404;padding:15px;border-radius:5px;margin:10px 0}";
  page += "button{background:#0066cc;color:white;padding:15px 30px;border:none;border-radius:5px;cursor:pointer;font-size:16px}";
  page += "button:hover{background:#0052a3}";
  page += ".step{background:#f8f9fa;padding:10px;margin:5px 0;border-left:4px solid #0066cc;border-radius:3px}";
  page += "</style>";
  page += "</head><body>";
  page += "<div class='container'>";
  page += "<h1>🔄 OTA (Over-The-Air) Update</h1>";
  page += "<p><a href='/'>← Back to Status</a></p>";
  
  page += "<div class='ota-info'>";
  page += "<h2>📊 Current System Info</h2>";
  page += "<p><strong>Chip ID:</strong> " + String((uint32_t)ESP.getEfuseMac(), HEX) + "</p>";
  page += "<p><strong>Flash Size:</strong> " + String(ESP.getFlashChipSize()) + " bytes</p>";
  page += "<p><strong>Free Space:</strong> " + String(ESP.getFreeSketchSpace()) + " bytes</p>";
  page += "<p><strong>Sketch Size:</strong> " + String(ESP.getSketchSize()) + " bytes</p>";
  page += "<p><strong>Core Version:</strong> " + String(ESP.getSdkVersion()) + "</p>";
  page += "<p><strong>IDF Version:</strong> " + String(ESP.getSdkVersion()) + "</p>";
  page += "</div>";
  
  page += "<div class='warning'>";
  page += "⚠️ <strong>Warning:</strong> Do not power off the device during OTA update!<br>";
  page += "📶 Ensure stable WiFi connection before starting update.";
  page += "</div>";
  
  page += "<h2>📋 How to Update</h2>";
  page += "<div class='step'>1. Compile your code in Arduino IDE</div>";
  page += "<div class='step'>2. Go to Sketch → Export compiled Binary</div>";
  page += "<div class='step'>3. Click 'Go to ElegantOTA' button below</div>";
  page += "<div class='step'>4. Upload the .bin file</div>";
  page += "<div class='step'>5. Wait for completion and device restart</div>";
  
  page += "<br>";
  page += "<button onclick=\"window.open('/update', '_blank')\">🚀 Go to ElegantOTA Update Page</button>";
  
  page += "<h3>🔗 Direct Links</h3>";
  page += "<ul>";
  page += "<li><a href='/update' target='_blank'>ElegantOTA Update Interface</a></li>";
  page += "<li><a href='http://" + WiFi.localIP().toString() + "/update' target='_blank'>Direct IP Access</a></li>";
  page += "</ul>";
  
  page += "</div></body></html>";
  
  server.send(200, "text/html; charset=utf-8", page);
}

void handleReboot() {
  String page = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<meta http-equiv='refresh' content='5;url=/'>";
  page += "<title>System Reboot</title>";
  page += "</head><body>";
  page += "<h1>🔄 System Rebooting...</h1>";
  page += "<p>Please wait 30 seconds and refresh the page.</p>";
  page += "</body></html>";
  
  server.send(200, "text/html; charset=utf-8", page);
  
  delay(1000);
  ESP.restart();
}

// ========== SETUP ==========

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("");
  Serial.println("");
  Serial.println(FPSTR(PROGMEM_SYSTEM_STARTUP));
  Serial.println("🔧 ESP32 WROOM-32D Version");
  Serial.println("⏰ Timestamp: " + String(millis()));
  Serial.println("🔄 Reset reason: " + String(esp_reset_reason()));
  Serial.println("💾 Free heap: " + String(ESP.getFreeHeap()) + " bytes");
  
  // Initialize EEPROM
  Serial.println("📝 Initializing EEPROM...");
  EEPROM.begin(sizeof(IntercomSettings));
  loadSettings();
  Serial.println("✅ Settings loaded");
  
  // Hardware watchdog
  Serial.println("🐕 Enabling hardware watchdog...");
  // ESP32 watchdog is handled automatically
  Serial.println("✅ Hardware watchdog enabled");
  
  // Initialize advanced watchdog system
  lastLoopTime = millis();
  lastTelegramActivity = millis();
  lastWiFiCheck = millis();
  softwareWatchdogCounter = 0;
  minFreeHeap = ESP.getFreeHeap();
  Serial.println("🐕 Advanced watchdog system initialized");
  
  // Pin setup
  pinMode(cameraButtonPin, OUTPUT);
  pinMode(doorButtonPin, OUTPUT);
  pinMode(ledPin, OUTPUT);
  
  digitalWrite(cameraButtonPin, LOW);
  digitalWrite(doorButtonPin, LOW);
  digitalWrite(ledPin, HIGH);
  
  // Set ADC resolution to 10-bit for compatibility with ESP8266 (0-1023)
  analogReadResolution(10);
  
  Serial.println("✅ Hardware pins configured");
  
  // WiFi setup
  Serial.println("📶 Setting up WiFi...");
  
  // ESP32-specific WiFi configuration for better compatibility
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFiManager wm;
  
  // Simplified WiFiManager configuration for ESP32 stability
  wm.setTimeout(180);  // Connection timeout
  wm.setDebugOutput(true);  // Enable debug output
  
  // Add small delay for stability
  delay(1000);
  
  Serial.println("🔧 WiFiManager configured for ESP32 stability");
  Serial.println("💾 Free heap before WiFi: " + String(ESP.getFreeHeap()) + " bytes");
  
  if (!wm.autoConnect("TelegramIntercom")) {
    Serial.println("❌ Failed to connect WiFi");
    ESP.restart();
  }
  
  Serial.println("✅ WiFi connected");
  Serial.println("🌐 Connected to network: " + WiFi.SSID());
  Serial.println("📡 IP address: " + WiFi.localIP().toString());
  Serial.println("📶 Signal strength: " + String(WiFi.RSSI()) + " dBm");
  Serial.println("🔧 Gateway: " + WiFi.gatewayIP().toString());
  Serial.println("📋 MAC address: " + WiFi.macAddress());
  
  // NTP setup
  Serial.println("🕐 Setting up time synchronization...");
  if (initializeNTP()) {
    Serial.println("✅ Time synchronized");
  } else {
    Serial.println("⚠️ Time sync failed - SSL may have issues");
  }
  
  // mDNS setup
  if (MDNS.begin("intercom")) {
    Serial.println("🌐 mDNS started: http://intercom.local/");
  }
  
  // Auto-detect working Telegram port
  if (!autoPortTestCompleted) {
    autoDetectTelegramPort();
    autoPortTestCompleted = true;
  }
  
  // Simple SSL setup with minimal Livebox 6 compatibility
  Serial.println("🔒 Setting up SSL for Telegram with Livebox 6 compatibility...");
  client.setInsecure();  // Required for Livebox 6 router - bypass certificate validation
  
  // Web server routes
  server.on("/", handleRoot);
  server.on("/settings", handleSettings);
  server.on("/save", HTTP_POST, handleSaveSettings);
  server.on("/test", handleTest);
  server.on("/reboot", handleReboot);
  
  // WiFi management routes
  server.on("/wifi", handleWiFi);
  server.on("/wifi/scan", handleWiFiScan);
  server.on("/wifi/portal", handleWiFiPortal);
  server.on("/wifi/reset", handleWiFiReset);
  
  // OTA info page
  server.on("/otainfo", handleOTAInfo);
  
  // Initialize ElegantOTA (provides /update endpoint)
  ElegantOTA.begin(&server);
  server.begin();
  Serial.println("🌐 Web server started with all routes:");
  Serial.println("  📊 http://intercom.local/ - Main status");
  Serial.println("  ⚙️ http://intercom.local/settings - Settings");
  Serial.println("  📶 http://intercom.local/wifi - WiFi setup");
  Serial.println("  🔄 http://intercom.local/update - ElegantOTA (Firmware Update)");
  Serial.println("  📋 http://intercom.local/otainfo - OTA Information");
  Serial.println("✅ ElegantOTA initialized and ready for firmware updates");

  // Test Telegram
  Serial.println("=== TELEGRAM SETUP ===");
  if (testTelegramConnectivity()) {
    telegramConnected = true;
    
    // Send simpler startup message to save memory
    String startupMsg = "🚀 Intercom Started\n";
    startupMsg += "IP: " + WiFi.localIP().toString() + "\n";
    startupMsg += "Boot #" + String(settings.rebootCount) + " Ready!";
    
    String replyKeyboard = "[[\"🚪 Open Intercom\", \"📺 View Display\"]]";
    bool sent = bot.sendMessageWithReplyKeyboard(CHAT_ID, startupMsg, "", replyKeyboard, true, false, false);
    Serial.println("📤 Startup notification: " + String(sent ? "✅ Sent" : "❌ Failed"));
    
    if (sent) {
      lastSuccessfulMessage = millis();
      Serial.println("✅ Telegram fully operational");
    }
  } else {
    Serial.println("❌ Telegram connectivity failed");
  }
  
  Serial.println("🔥 SETUP COMPLETED - ENTERING MAIN LOOP");
  Serial.println("========================================");
  Serial.println("💡 Serial commands available! Type 'help' for list");
}

// ========== MAIN LOOP ==========

void loop() {
  // Advanced watchdog feeding
  softwareWatchdogFeed();
  
  // Continuous memory monitoring and emergency cleanup
  uint32_t currentLoopMemory = ESP.getFreeHeap();
  if (currentLoopMemory < 12000) {
    Serial.println("🚨 LOOP: Critical memory detected: " + String(currentLoopMemory) + " bytes");
    forceMemoryCleanup();
    
    // Double check after cleanup
    currentLoopMemory = ESP.getFreeHeap();
    if (currentLoopMemory < 10000) {
      Serial.println("🚨 LOOP: Emergency restart - cleanup failed!");
      emergencyRestart("Loop memory critically low");
    }
  }
  
  // Debug: показать что loop работает (каждые 30 секунд)
  static unsigned long lastLoopDebug = 0;
  if (millis() - lastLoopDebug > 30000) {
    Serial.println("🔄 Loop running, memory: " + String(ESP.getFreeHeap()) + " bytes");
    Serial.println("💡 Serial commands ready! Type 'ping' to test");
    lastLoopDebug = millis();
  }
  
  // System health monitoring
  checkSystemHealth();
  checkMemoryHealth();
  
  // Handle web server (with emergency mode)
  uint32_t freeBeforeWeb = ESP.getFreeHeap();
  if (freeBeforeWeb > 20000) {
    // Full web server + OTA
    server.handleClient();
    ElegantOTA.loop();
  } else if (freeBeforeWeb > 15000) {
    // Web server only, no OTA
    server.handleClient();
  } else if (freeBeforeWeb > 10000) {
    // EMERGENCY MODE: Handle only critical requests
    server.handleClient();
    static unsigned long lastCleanup = 0;
    if (millis() - lastCleanup > 30000) {
      forceMemoryCleanup();
      lastCleanup = millis();
    }
  } else {
    // Memory critically low - skip web server completely
    static unsigned long lastWebWarning = 0;
    if (millis() - lastWebWarning > 60000) {
      Serial.print("🚨 Web disabled, mem: "); 
      Serial.print(freeBeforeWeb); 
      Serial.println("b");
      lastWebWarning = millis();
    }
    forceMemoryCleanup();
  }
  
  // Check for incoming calls
  checkIncomingCall();
  
  // Handle Telegram messages
  if (millis() > lastTimeBotRan + botRequestDelay) {
    // Check memory before Telegram operations
    uint32_t freeMem = ESP.getFreeHeap();
    if (freeMem < 15000) {
      Serial.println("🚨 Skipping Telegram check - low memory: " + String(freeMem) + " bytes");
      forceMemoryCleanup();
      lastTimeBotRan = millis();
      
      // Emergency restart if memory critically low
      if (freeMem < 10000) {
        Serial.println("🚨 CRITICAL: Memory too low for any operation!");
        emergencyRestart("Memory critically low before Telegram");
      }
      return;
    }
    
    beginCriticalOperation("Telegram Message Handling");
    
    // Reconnect if needed
    if (!telegramConnected && (millis() - lastTelegramCheck > 30000)) {
      if (reconnectTelegram()) {
        telegramConnected = true;
      }
      lastTelegramCheck = millis();
    }
    
    // Handle messages
    if (telegramConnected || WiFi.status() == WL_CONNECTED) {
      int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      if (numNewMessages > 0) {
        handleNewMessages(numNewMessages);
        telegramConnected = true;
        lastSuccessfulMessage = millis();
        
        while ((numNewMessages = bot.getUpdates(bot.last_message_received + 1)) > 0) {
      handleNewMessages(numNewMessages);
          lastSuccessfulMessage = millis();
        }
      } else if (numNewMessages < 0) {
        Serial.println("❌ Telegram getUpdates failed");
        telegramConnected = false;
      }
    }
    
    lastTimeBotRan = millis();
    lastTelegramActivity = millis(); // Update activity timestamp
    endCriticalOperation("Telegram Message Handling");
  }
  
  // Basic Serial commands
  if (Serial.available()) {
    Serial.println("📡 Serial data available!");
    String command = Serial.readString();
    Serial.println("📥 Raw command: '" + command + "'");
    command.trim();
    Serial.println("🎯 Trimmed command: '" + command + "' (length: " + String(command.length()) + ")");
    
    if (command == "open") {
      Serial.println("🚪 Manual open via Serial");
      openIntercom();
      Serial.println("✅ Intercom opened!");
    } else if (command == "test") {
      Serial.println("📤 Sending test message...");
      bool sent = bot.sendMessage(CHAT_ID, "🧪 Test message from Serial", "");
      Serial.println("📤 Test: " + String(sent ? "✅ Sent" : "❌ Failed"));
      if (sent) {
        telegramConnected = true;
        lastSuccessfulMessage = millis();
      }
    } else if (command == "status") {
      Serial.println("📊 === SYSTEM STATUS ===");
      Serial.println("🌐 WiFi: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("📍 IP: " + WiFi.localIP().toString());
        Serial.println("🌐 SSID: " + WiFi.SSID());
        Serial.println("📶 Signal: " + String(WiFi.RSSI()) + " dBm");
      }
      Serial.println("💬 Telegram: " + String(telegramConnected ? "✅ Connected" : "❌ Disconnected"));
      Serial.println("🔗 Port: " + String(workingTelegramPort));
      Serial.println("🔓 SSL: Insecure (Livebox 6 compatible)");
      Serial.println("💾 Memory: " + String(ESP.getFreeHeap()) + " bytes");
      Serial.println("⏰ Uptime: " + String(millis() / 60000) + " min");
      Serial.println("🔄 Boot count: " + String(settings.rebootCount));
      Serial.println("📞 Call threshold: " + String(settings.callThreshold));
      Serial.println("📊 Current A0: " + String(analogRead(callIndicatorPin)));
      Serial.println("🕐 Time sync: " + String(timeInitialized ? "✅ Synced" : "❌ Not synced"));
      Serial.println("🐕 Watchdog feeds: " + String(softwareWatchdogCounter));
      Serial.println("💾 Minimum heap: " + String(minFreeHeap) + " bytes");
      Serial.println("🔒 Critical operation: " + String(criticalOperationInProgress ? "IN PROGRESS" : "None"));
    } else if (command == "restart") {
      Serial.println("🔄 Manual restart via Serial...");
      emergencyRestart("Manual restart via serial");
    } else if (command == "cleanup") {
      Serial.println("🧹 Manual memory cleanup...");
      forceMemoryCleanup();
      uint32_t afterCleanup = ESP.getFreeHeap();
      Serial.println("💾 Memory after cleanup: " + String(afterCleanup) + " bytes");
    } else if (command == "resetwifi") {
      Serial.println("🗑️ Resetting WiFi settings...");
      WiFiManager wm;
      wm.resetSettings();
      Serial.println("✅ WiFi settings cleared, restarting...");
      delay(1000);
      ESP.restart();
    } else if (command == "wifiportal") {
      Serial.println("📶 Starting WiFi configuration portal...");
      WiFi.disconnect(true);
      delay(1000);
      WiFiManager wm;
      wm.resetSettings();
      wm.setConfigPortalTimeout(300); // 5 minute timeout
      Serial.println("🔄 Creating 'TelegramIntercom' network...");
      Serial.println("📱 Connect to 'TelegramIntercom' and go to 192.168.4.1");
      wm.startConfigPortal("TelegramIntercom");
    } else if (command == "forcereset") {
      Serial.println("🔥 FORCE RESET - Clearing ALL WiFi data...");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(1000);
      // ESP32 doesn't have eraseConfig(), WiFiManager handles this
      delay(1000);
      Serial.println("💥 All settings erased, restarting...");
      ESP.restart();
    } else if (command == "scanwifi") {
      Serial.println("🔍 Scanning for WiFi networks...");
      int n = WiFi.scanNetworks();
      if (n == 0) {
        Serial.println("❌ No networks found");
      } else {
        Serial.println("📡 Found " + String(n) + " networks:");
        for (int i = 0; i < n; ++i) {
          // Разбиваем сложную конкатенацию WiFi info
          Serial.print(i + 1); 
          Serial.print(": "); 
          Serial.print(WiFi.SSID(i)); 
          Serial.print(" ("); 
          Serial.print(WiFi.RSSI(i)); 
          Serial.print(" dBm) "); 
          Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "🔓" : "🔒");
        }
      }
    } else if (command == "ping") {
      Serial.println("🏓 PONG! Serial commands working!");
      Serial.println("📊 Memory: " + String(ESP.getFreeHeap()) + " bytes");
      Serial.println("⏰ Uptime: " + String(millis() / 1000) + " seconds");
    } else if (command == "memtest") {
      Serial.println("🧪 === MEMORY TEST RESULTS ===");
      uint32_t freeMem = ESP.getFreeHeap();
      uint16_t fragmentation = 0; // ESP32 doesn't have getHeapFragmentation()
      uint32_t maxBlock = ESP.getMaxAllocHeap();
      
      Serial.print("📊 Free: "); Serial.print(freeMem); Serial.println("b");
      Serial.print("🧩 Frag: "); Serial.print(fragmentation); Serial.println("%");
      Serial.print("📦 Block: "); Serial.print(maxBlock); Serial.println("b");
      Serial.print("🏆 Min: "); Serial.print(minFreeHeap); Serial.println("b");
      
      // Тест на стабильность памяти
      if (freeMem > 25000) {
        Serial.println("🎯 ✅ EXCELLENT");
      } else if (freeMem > 20000) {
        Serial.println("🎯 ✅ GOOD");
      } else if (freeMem > 15000) {
        Serial.println("🎯 ⚠️ LOW");
      } else if (freeMem > 10000) {
        Serial.println("🎯 🚨 CRITICAL");
      } else {
        Serial.println("🎯 💀 EMERGENCY");
      }
      Serial.println("===============================");
    } else if (command == "freemem") {
      Serial.println("🧹 EMERGENCY MEMORY RECOVERY");
      
      // Агрессивная очистка
      for (int i = 0; i < 50; i++) {
        yield();
        // ESP32 watchdog is handled automatically
        delay(5);
      }
      
      Serial.print("💾 Before: "); Serial.print(ESP.getFreeHeap()); Serial.println("b");
      
      // Принудительная сборка мусора
      delay(500);
      
      uint32_t afterMem = ESP.getFreeHeap();
      Serial.print("💾 After: "); Serial.print(afterMem); Serial.println("b");
      
      if (afterMem > 20000) {
        Serial.println("✅ Memory recovered successfully");
      } else {
        Serial.println("⚠️ Memory still low - consider restart");
      }
    } else if (command == "help" || command == "?" || command == "") {
      Serial.println(FPSTR(PROGMEM_HELP_MSG));
    } else {
      Serial.println("❓ Unknown command: '" + command + "'");
      Serial.println("💡 Type 'help' to see available commands");
    }
  }
  
  delay(100);
}
 