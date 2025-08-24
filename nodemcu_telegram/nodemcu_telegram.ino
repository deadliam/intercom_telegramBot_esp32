/*
  Telegram bot для управления домофоном через ESP8266/Wemos
  
  ФУНКЦИОНАЛЬНОСТЬ:
  - Мониторинг входящих вызовов через аналоговый вход A0 (делитель напряжения)
  - Автоматическая отправка уведомлений в Telegram при входящих вызовах
  - Управление домофоном через команду "Open":
    1. Нажимает кнопку камеры (D1) на 2 секунды  
    2. Нажимает кнопку замка (D2) кратковременно
  
  ПОДКЛЮЧЕНИЕ:
  - D1 (GPIO5) - оптопара PC817 для кнопки камеры
  - D2 (GPIO4) - оптопара PC817 для кнопки замка  
  - A0 - делитель напряжения от светодиода индикации вызова (2x10кОм, делит 1.8В→0.9В)
  
  КОМАНДЫ:
  - /start - главное меню
  - /status - состояние системы и аналогового входа
  - "Open" - открытие домофона
*/

#ifdef ESP32
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
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

// Polling period (ms)
int botRequestDelay = 2000;
unsigned long lastTimeBotRan;

// UniversalTelegramBot variables

// GPIO for onboard LED (optional)
const int ledPin = 2;
bool ledState = LOW;

// Intercom configuration
const int cameraPin = 5;       // GPIO5 (D1 on Wemos) - кнопка включения камеры 
const int doorPin = 4;         // GPIO4 (D2 on Wemos) - кнопка открытия замка
const bool optocouplerActiveHigh = false; // PC817 оптопары обычно активны при LOW

// Call detection configuration
const int callIndicatorPin = A0;  // Аналоговый вход для мониторинга вызова
const int normalVoltageThreshold = 200;  // Пороговое значение (0.9V через делитель = ~279, ставим 200 для надежности)
const unsigned long callDebounceTime = 5000;  // 5 секунд между уведомлениями о вызовах

// Call detection variables
bool callDetected = false;
bool lastCallState = false;
unsigned long lastCallTime = 0;

// Reply keyboard with one "Open" button (persistent chat menu)
static const char replyKeyboard[] = "[[\"Open\"]]";

// Функция для нажатия кнопки камеры (2 секунды)
void pressCameraButton() {
  Serial.println("🎥 === CAMERA BUTTON PRESS START ===");
  Serial.print("Setting pin "); Serial.print(cameraPin); 
  Serial.print(" to "); Serial.println(optocouplerActiveHigh ? "HIGH" : "LOW");
  
  digitalWrite(cameraPin, optocouplerActiveHigh ? HIGH : LOW);
  delay(2000); // 2 секунды
  digitalWrite(cameraPin, optocouplerActiveHigh ? LOW : HIGH);
  
  Serial.print("Setting pin "); Serial.print(cameraPin); 
  Serial.print(" to "); Serial.println(optocouplerActiveHigh ? "LOW" : "HIGH");
  Serial.println("🎥 === CAMERA BUTTON RELEASE ===");
}

// Функция для кратковременного нажатия кнопки замка
void pressDoorButton() {
  Serial.println("🚪 === DOOR BUTTON PRESS START ===");
  Serial.print("Setting pin "); Serial.print(doorPin); 
  Serial.print(" to "); Serial.println(optocouplerActiveHigh ? "HIGH" : "LOW");
  
  digitalWrite(doorPin, optocouplerActiveHigh ? HIGH : LOW);
  delay(200); // Кратковременное нажатие 200ms
  digitalWrite(doorPin, optocouplerActiveHigh ? LOW : HIGH);
  
  Serial.print("Setting pin "); Serial.print(doorPin); 
  Serial.print(" to "); Serial.println(optocouplerActiveHigh ? "LOW" : "HIGH");
  Serial.println("🚪 === DOOR BUTTON RELEASE ===");
}

// Полная последовательность открытия домофона
void openIntercom() {
  Serial.println("=== Starting intercom opening sequence ===");
  
  // Сначала включаем камеру
  pressCameraButton();
  
  // Небольшая пауза между операциями
  delay(500);
  
  // Затем открываем замок
  pressDoorButton();
  
  Serial.println("=== Intercom opening sequence completed ===");
}

// Функция проверки входящего вызова
void checkIncomingCall() {
  int analogValue = analogRead(callIndicatorPin);
  bool currentCallState = (analogValue < normalVoltageThreshold);
  
  // Отладочная информация каждые 10 секунд
  static unsigned long lastDebugTime = 0;
  if (millis() - lastDebugTime > 10000) {
    Serial.print("Call indicator A0 value: ");
    Serial.print(analogValue);
    Serial.print(" (threshold: ");
    Serial.print(normalVoltageThreshold);
    Serial.println(")");
    lastDebugTime = millis();
  }
  
  // Обнаружение нового вызова
  if (currentCallState && !lastCallState) {
    // Новый вызов обнаружен
    if (millis() - lastCallTime > callDebounceTime) {
      callDetected = true;
      lastCallTime = millis();
      
      // Уведомление о вызове через Telegram
      Serial.println("🔔 === INCOMING CALL DETECTED ===");
      Serial.println("📞 Someone is calling the intercom!");
      
      String callMessage = "🔔 ВХОДЯЩИЙ ВЫЗОВ!\n\nКто-то звонит в домофон.\nНажмите 'Open' чтобы открыть.";
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
      String welcome = "🏠 Добро пожаловать, " + from_name + "!\n\nЭтот бот управляет домофоном:\n• Мониторинг входящих вызовов 🔔\n• Открытие домофона 🚪\n\nНажмите 'Open' для открытия или /status для состояния системы.";
      bot.sendMessageWithReplyKeyboard(chat_id, welcome, "", replyKeyboard, true, false, false);
    } else if (text == "Open") {
      Serial.println("🚪 Processing 'Open' command - INTERCOM SEQUENCE START");
      bot.sendMessage(chat_id, "🎥 Включаю камеру домофона...", "");
      
      // Включаем камеру (2 секунды)
      pressCameraButton();
      
      bot.sendMessage(chat_id, "🚪 Открываю замок...", "");
      
      // Пауза и открытие замка
      delay(500);
      pressDoorButton();
      
      bot.sendMessage(chat_id, "✅ Домофон открыт!", "");
      Serial.println("🚪 INTERCOM SEQUENCE COMPLETED");
    } else if (text == "/status") {
      Serial.println("📊 Processing /status command");
      // Команда для проверки состояния системы
      int analogValue = analogRead(callIndicatorPin);
      String statusMessage = "📊 СОСТОЯНИЕ СИСТЕМЫ\n\n";
      statusMessage += "🔌 Аналоговый вход A0: " + String(analogValue) + "\n";
      statusMessage += "⚡ Пороговое значение: " + String(normalVoltageThreshold) + "\n";
      statusMessage += "🚨 Состояние вызова: " + String(analogValue < normalVoltageThreshold ? "ВЫЗОВ" : "НОРМА") + "\n";
      statusMessage += "🌐 IP адрес: " + WiFi.localIP().toString();
      
      bot.sendMessage(chat_id, statusMessage, "");
    } else {
      Serial.print("❓ Unknown command received: '"); Serial.print(text); Serial.println("'");
      bot.sendMessageWithReplyKeyboard(chat_id, "🏠 Для открытия домофона нажмите кнопку 'Open'\n\nДоступные команды:\n/start - главное меню\n/status - состояние системы", "", replyKeyboard, true, false, false);
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  Serial.println("=== CONFIGURATION DEBUG ===");
  Serial.print("Secrets loaded from: "); Serial.println(SECRETS_SOURCE);
  Serial.print("WiFi SSID: "); Serial.println(WIFI_SSID);
  Serial.print("WiFi Password: "); Serial.println(WIFI_PASSWORD);
  Serial.print("Bot Token: "); Serial.println(BOT_TOKEN);
  Serial.print("Chat ID: "); Serial.println(CHAT_ID);
  
  // Проверяем, загрузились ли реальные данные
  if (String(WIFI_SSID) == "YOUR_WIFI_SSID") {
    Serial.println("🚨 WARNING: Using template values! Real secrets.h not loaded!");
  } else {
    Serial.println("✅ Real secrets loaded successfully");
  }
  
  // Важная информация для отладки
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
  
  // Инициализация пинов для управления домофоном
  pinMode(cameraPin, OUTPUT);
  pinMode(doorPin, OUTPUT);
  
  // Устанавливаем пины в неактивное состояние (кнопки не нажаты)
  digitalWrite(cameraPin, optocouplerActiveHigh ? LOW : HIGH);
  digitalWrite(doorPin, optocouplerActiveHigh ? LOW : HIGH);
  
  Serial.println("🔌 GPIO PIN MAPPING:");
  Serial.print("Camera button: GPIO"); Serial.print(cameraPin);
  Serial.println(" (should be D1 on Wemos/D5 on NodeMCU)");
  Serial.print("Door button: GPIO"); Serial.print(doorPin); 
  Serial.println(" (should be D2 on Wemos/D4 on NodeMCU)");
  Serial.print("Call indicator: A0 (GPIO"); Serial.print(callIndicatorPin); Serial.println(")");
  Serial.print("Voltage threshold: "); Serial.println(normalVoltageThreshold);
  Serial.println("⚠️  Verify GPIO pins match your hardware wiring!");
  
  // Проверяем начальное состояние индикатора вызова
  int initialVoltage = analogRead(callIndicatorPin);
  Serial.print("Initial A0 reading: "); Serial.println(initialVoltage);
  
  // Тест GPIO пинов - кратковременное мигание для проверки
  Serial.println("🧪 Testing GPIO pins (brief flash)...");
  digitalWrite(cameraPin, !digitalRead(cameraPin)); delay(100);
  digitalWrite(cameraPin, !digitalRead(cameraPin));
  digitalWrite(doorPin, !digitalRead(doorPin)); delay(100); 
  digitalWrite(doorPin, !digitalRead(doorPin));
  Serial.println("✅ GPIO pin test completed");

  // Connect to Wi-Fi
  Serial.println("=== WiFi CONNECTION DEBUG ===");
  Serial.print("MAC Address: "); Serial.println(WiFi.macAddress());
  
  #ifdef ESP8266
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
  #endif
  WiFi.mode(WIFI_STA);
  
  Serial.print("Attempting to connect to SSID: "); Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  const int maxAttempts = 30; // 30 секунд timeout
  
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(1000);
    attempts++;
    Serial.print("Connecting to WiFi.. Attempt ");
    Serial.print(attempts);
    Serial.print("/");
    Serial.print(maxAttempts);
    Serial.print(" - Status: ");
    
    switch (WiFi.status()) {
      case WL_IDLE_STATUS:
        Serial.println("IDLE");
        break;
      case WL_NO_SSID_AVAIL:
        Serial.println("NO_SSID_AVAILABLE");
        break;
      case WL_SCAN_COMPLETED:
        Serial.println("SCAN_COMPLETED");
        break;
      case WL_CONNECT_FAILED:
        Serial.println("CONNECT_FAILED");
        break;
      case WL_CONNECTION_LOST:
        Serial.println("CONNECTION_LOST");
        break;
      case WL_DISCONNECTED:
        Serial.println("DISCONNECTED");
        break;
      default:
        Serial.println("UNKNOWN");
        break;
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✅ WiFi connected successfully!");
    Serial.print("IP address: "); Serial.println(WiFi.localIP());
    Serial.print("Gateway: "); Serial.println(WiFi.gatewayIP());
    Serial.print("Subnet: "); Serial.println(WiFi.subnetMask());
    Serial.print("DNS: "); Serial.println(WiFi.dnsIP());
    Serial.print("RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
  } else {
    Serial.println("❌ WiFi connection FAILED!");
    Serial.println("Scanning for available networks...");
    
    // Сканируем доступные сети
    int networks = WiFi.scanNetworks();
    if (networks == 0) {
      Serial.println("No networks found");
    } else {
      Serial.print(networks);
      Serial.println(" networks found:");
      for (int i = 0; i < networks; ++i) {
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.print(WiFi.SSID(i));
        Serial.print(" (");
        Serial.print(WiFi.RSSI(i));
        Serial.print(" dBm) ");
        Serial.print(WiFi.encryptionType(i) == 7 ? "Open" : "Secured");
        
        // Проверяем, совпадает ли с нашим SSID
        if (WiFi.SSID(i) == String(ssid)) {
          Serial.print(" ← TARGET NETWORK FOUND!");
        }
        Serial.println();
      }
    }
    
    Serial.println("TROUBLESHOOTING:");
    Serial.println("1. Check SSID spelling and case sensitivity");
    Serial.println("2. Ensure network is 2.4GHz (ESP8266 doesn't support 5GHz)");
    Serial.println("3. Check if network is hidden");
    Serial.println("4. Verify signal strength");
    Serial.println("Device will continue without WiFi...");
  }

  // TLS setup - более агрессивное упрощение для ESP8266
  Serial.println("🔒 Setting up TLS/SSL...");
  #ifdef ESP8266
    client.setInsecure(); // simplify TLS for ESP8266
    client.setTimeout(10000); // 10 секунд таймаут
    Serial.println("✅ Using insecure TLS with 10s timeout");
  #else
    client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
    Serial.println("✅ Using certificate validation for ESP32");
  #endif

  // Настройка Telegram
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("=== TELEGRAM SETUP ===");
    Serial.print("💾 Free heap: "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");
    
    // Отправляем стартовое уведомление
    String startupMsg = "🏠 Домофон бот запущен!\nIP: " + WiFi.localIP().toString() + "\n\nСистема готова к работе!";
    bool sent = bot.sendMessageWithReplyKeyboard(CHAT_ID, startupMsg, "", replyKeyboard, true, false, false);
    Serial.print("📤 Startup notification: "); Serial.println(sent ? "✅ Sent" : "❌ Failed");
    
    Serial.println("🤖 Telegram bot ready!");
    Serial.println("============================");
  } else {
    Serial.println("Skipping Telegram setup - No WiFi connection");
  }
}

void loop() {
  // Проверяем память каждые 30 секунд
  static unsigned long lastMemCheck = 0;
  if (millis() - lastMemCheck > 30000) {
    Serial.print("💾 Free: "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");
    lastMemCheck = millis();
  }
  
  // Проверяем входящие вызовы
  checkIncomingCall();
  
  // Обработка Telegram сообщений
  if (millis() > lastTimeBotRan + botRequestDelay) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    
    while (numNewMessages) {
      Serial.println("🔄 Processing Telegram messages...");
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = millis();
  }
  
  // Serial commands для отладки
  if (Serial.available()) {
    String command = Serial.readString();
    command.trim();
    
    if (command == "open") {
      Serial.println("🚪 Manual open via Serial");
      // Эмулируем Telegram сообщение
      Serial.println("🎥 Включаю камеру домофона...");
      pressCameraButton();
      Serial.println("🚪 Открываю замок...");
      delay(500);
      pressDoorButton();
      Serial.println("✅ Домофон открыт!");
    } else if (command == "test") {
      bot.sendMessage(CHAT_ID, "Test message from Serial", "");
    } else if (command == "status") {
      Serial.print("💾 Memory: "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");
      Serial.print("🌐 WiFi: "); Serial.println(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
      Serial.print("📊 A0: "); Serial.print(analogRead(callIndicatorPin));
      Serial.println(analogRead(callIndicatorPin) < normalVoltageThreshold ? " (CALL)" : " (NORMAL)");
      Serial.print("🌐 IP: "); Serial.println(WiFi.localIP());
    }
  }
  
  // Принудительная очистка памяти каждые 5 секунд
  static unsigned long lastCleanup = 0;
  if (millis() - lastCleanup > 5000) {
    yield();
    lastCleanup = millis();
    
    // Аварийная перезагрузка если памяти критически мало
    if (ESP.getFreeHeap() < 5000) {
      Serial.println("🚨 CRITICAL LOW MEMORY - RESTARTING...");
      delay(1000);
      ESP.restart();
    }
  }
  
  // Watchdog protection - перезапуск если система зависла надолго
  static unsigned long lastActivity = millis();
  if (millis() - lastActivity > 300000) { // 5 минут без активности
    Serial.println("🚨 SYSTEM STUCK - RESTARTING...");
    ESP.restart();
  }
}

 