/*
  Minimal Telegram bot for ESP8266/ESP32 using UniversalTelegramBot
  - /start shows an inline keyboard with one button: "Open"
  - Pressing "Open" pulses a relay and logs to Serial
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
  #if __has_include("secrets.h")
    #include "secrets.h"
  #else
    #include "secrets.example.h"
  #endif
#else
  #include "secrets.h"
#endif

// Replace with your network credentials
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Initialize Telegram BOT
#define BOTtoken BOT_TOKEN  // your Bot Token (from BotFather)

// Use @myidbot to find out the chat ID of an individual or a group
// Also note that you need to click "start" on a bot before it can message you
#define CHAT_ID "52489332"

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// Polling period (ms)
int botRequestDelay = 1000;
unsigned long lastTimeBotRan;

// GPIO for onboard LED (optional)
const int ledPin = 2;
bool ledState = LOW;

// Relay configuration
const int relayPin = 5;         // GPIO5 (D1 on NodeMCU). Change to your wiring
const bool relayActiveHigh = false; // set true if your relay is active HIGH

// Reply keyboard with one "Open" button (persistent chat menu)
// Library expects just the keyboard array; it wraps it internally
static const char replyKeyboard[] = "[[\"Open\"]]";

void relayOn() {
  Serial.print("Relay ON (pin "); Serial.print(relayPin);
  Serial.print(", active "); Serial.println(relayActiveHigh ? "HIGH" : "LOW");
  digitalWrite(relayPin, relayActiveHigh ? HIGH : LOW);
}

void relayOff() {
  Serial.print("Relay OFF (pin "); Serial.print(relayPin);
  Serial.print(", active "); Serial.println(relayActiveHigh ? "HIGH" : "LOW");
  digitalWrite(relayPin, relayActiveHigh ? LOW : HIGH);
}

// Handle new messages: only /start and one inline button "Open"
void handleNewMessages(int numNewMessages) {
  Serial.print("handleNewMessages: "); Serial.println(numNewMessages);

  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String type = bot.messages[i].type;
    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

  if (chat_id != CHAT_ID) {
      bot.sendMessage(chat_id, "Unauthorized user", "");
      continue;
    }

    // Normal text messages only; reply keyboard sends text "Open"
    if (text == "/start") {
      String welcome = "Welcome, " + from_name + ". Tap the button:";
      bool ok = bot.sendMessageWithReplyKeyboard(chat_id, welcome, "", replyKeyboard, true, false, false);
      Serial.print("/start -> reply keyboard: "); Serial.println(ok ? "OK" : "FAIL");
    } else if (text == "Open") {
      Serial.println("Open button pressed");
      bot.sendMessage(chat_id, "Opening...", "");
      relayOn();
      delay(500);
      relayOff();
      bot.sendMessage(chat_id, "Done.", "");
    } else {
      bool ok = bot.sendMessageWithReplyKeyboard(chat_id, "Tap the button:", "", replyKeyboard, true, false, false);
      Serial.print("reply keyboard: "); Serial.println(ok ? "OK" : "FAIL");
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, ledState);
  pinMode(relayPin, OUTPUT);
  relayOff();

  // Connect to Wi-Fi
  #ifdef ESP8266
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
  #endif
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi..");
  }
  Serial.print("WiFi IP: "); Serial.println(WiFi.localIP());

  // TLS setup
  #ifdef ESP8266
    client.setInsecure(); // simplify TLS for ESP8266
  #else
    client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  #endif

  // Optional: notify start
  bot.sendMessage(CHAT_ID, String("Bot started on ") + WiFi.localIP().toString(), "");
}

void loop() {
  if (millis() > lastTimeBotRan + botRequestDelay)  {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = millis();
  }
}

 