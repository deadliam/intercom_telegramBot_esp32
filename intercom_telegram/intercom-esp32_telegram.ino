// Telegram Intercom — ESP32 / ESP32-C3
// Web portal (plain HTML), WiFiManager, ElegantOTA, mDNS (<host>.local)
// All secrets (bot token, users, web password) are entered in the web portal
// and stored in NVS — nothing secret is compiled into the firmware.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <EEPROM.h>
#include <WiFiManager.h>          // https://github.com/tzapu/WiFiManager
#include <UniversalTelegramBot.h>
#include <ElegantOTA.h>
#include <PubSubClient.h>         // MQTT for Home Assistant
#include <esp_task_wdt.h>
#include <time.h>
#include <functional>

#define FW_VERSION "2.1.0"

// ---------- Pins ----------
#if CONFIG_IDF_TARGET_ESP32C3
  #define BOARD_NAME "ESP32-C3"
  const int PIN_DOOR   = 10;   // PhotoMOS (solid-state relay) input via 330 Ohm
  const int PIN_CAMERA = 5;    // display/camera button (full type only)
  const int PIN_CALL   = 3;    // ADC1_CH3, call indicator (full type only)
  const int PIN_LED    = 8;    // on-board LED, active LOW
  const bool LED_ACTIVE_LOW = true;
#else
  #define BOARD_NAME "ESP32"
  const int PIN_DOOR   = 19;
  const int PIN_CAMERA = 18;
  const int PIN_CALL   = 36;   // ADC1_CH0
  const int PIN_LED    = 2;
  const bool LED_ACTIVE_LOW = false;
#endif

#define INTERCOM_FULL   0     // screen + camera button + call detection
#define INTERCOM_SIMPLE 1     // no screen: only "open door"

const uint32_t DISPLAY_ON_MS   = 32000;  // intercom screen stays on this long
const uint32_t TG_POLL_MS      = 1000;
const uint32_t TG_CHECK_MS     = 300000; // getMe health check
const uint32_t WDT_TIMEOUT_MS  = 60000;
const int      LOG_SIZE        = 25;
const long     CALL_BTN_S      = 180;    // "Open" button in a call notification works this long
const long     CONFIRM_BTN_S   = 60;     // confirmation button works this long
const int      REQ_GUEST_H     = 24;     // guest duration offered on an access request
const uint32_t MQTT_RETRY_MS   = 30000;

// ---------- Configuration (NVS) ----------
struct Config {
  String token;       // Telegram bot token
  String admin;       // admin chat id
  String users;       // lines: "<chat_id> <name>"
  String guests;      // lines: "<chat_id> <expiry epoch> <name>"
  String host;        // mDNS name -> <host>.local
  String webPass;     // web portal / OTA password (user "admin")
  String tz;          // POSIX TZ string
  String mqttHost, mqttUser, mqttPass;
  uint8_t  type;
  uint16_t doorMs, camMs, threshold, debounceMs, maxAgeS, mqttPort;
  bool notifyAll, tlsVerify, confirmOpen, mqttOn;
} cfg;

Preferences prefs;

enum Role { R_NONE, R_GUEST, R_USER, R_ADMIN };

// ---------- Runtime state ----------
WebServer server(80);
WiFiClientSecure tgClient;
UniversalTelegramBot bot("", tgClient);
WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);
uint32_t lastMqttTry = 0;
bool mqttWasUp = false;
String lastReqId; uint32_t lastReqMs = 0;   // access-request anti-spam

String csrf;                   // per-boot token for POST forms
uint32_t bootCount = 0;

bool tgOk = false;
String tgBotName;
uint32_t lastPoll = 0, lastTgCheck = 0, lastUnauthReply = 0;
bool tgCheckedOnce = false;

uint32_t displayStart = 0, displayLen = 0;   // screen-on window
uint32_t ignoreStart = 0, ignoreLen = 0;     // call detection blackout
bool lastCallState = false;
uint32_t lastCallMs = 0;
bool busy = false;

struct LogEntry { time_t t; uint32_t ms; String text; };
LogEntry logBuf[LOG_SIZE];
int logHead = 0, logCount = 0;

// ---------- Helpers ----------
inline bool hasDisplay() { return cfg.type != INTERCOM_SIMPLE; }
inline bool timeValid() { return time(nullptr) > 1700000000; }

void led(bool on) { digitalWrite(PIN_LED, (on ^ LED_ACTIVE_LOW) ? HIGH : LOW); }

void delayWdt(uint32_t ms) {
  uint32_t st = millis();
  while (millis() - st < ms) { esp_task_wdt_reset(); delay(10); }
}

bool windowActive(uint32_t start, uint32_t len) { return millis() - start < len; }

// Extend a [start, len) window so that it lasts at least `ms` from now (overflow-safe)
void extendWindow(uint32_t &start, uint32_t &len, uint32_t ms) {
  uint32_t now = millis(), el = now - start;
  uint32_t remaining = el < len ? len - el : 0;
  if (ms > remaining) { start = now; len = ms; }
}

String esc(const String &s) {
  String o; o.reserve(s.length() + 8);
  for (char c : s) {
    switch (c) {
      case '&': o += F("&amp;"); break;
      case '<': o += F("&lt;"); break;
      case '>': o += F("&gt;"); break;
      case '"': o += F("&quot;"); break;
      case '\'': o += F("&#39;"); break;
      default: o += c;
    }
  }
  return o;
}

String fmtUptime(uint32_t sec) {
  uint32_t d = sec / 86400, h = sec / 3600 % 24, m = sec / 60 % 60;
  String s; if (d) s += String(d) + " д ";
  s += String(h) + " ч " + String(m) + " мин"; return s;
}

String fmtTime(time_t t, uint32_t ms) {
  if (t > 1700000000) {
    struct tm tmv; localtime_r(&t, &tmv); char b[24];
    strftime(b, sizeof b, "%d.%m %H:%M:%S", &tmv); return String(b);
  }
  return "через " + fmtUptime(ms / 1000) + " после старта";
}

String mqttBase() { return "intercom/" + cfg.host; }

void addLog(const String &text) {
  logBuf[logHead] = { time(nullptr), millis(), text };
  logHead = (logHead + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;
  Serial.println("[log] " + text);
  if (mqtt.connected()) mqtt.publish((mqttBase() + "/event").c_str(), text.c_str(), true);
}

long nowEpoch() { return timeValid() ? (long)time(nullptr) : 0; }

// Name safe for callback_data / one-line storage: no quotes, backslashes, colons, control chars,
// cut to maxBytes without splitting a UTF-8 character.
String cleanName(const String &in, size_t maxBytes) {
  String o;
  for (size_t i = 0; i < in.length(); i++) {
    unsigned char c = in[i];
    if (c < 32 || c == '"' || c == '\\' || c == ':') continue;
    o += (char)c;
  }
  o.trim();
  if (o.length() > maxBytes) {
    size_t cut = maxBytes;
    while (cut > 0 && ((unsigned char)o[cut] & 0xC0) == 0x80) cut--;
    o = o.substring(0, cut);
  }
  return o;
}

bool validChatId(const String &s) {
  if (s.length() == 0 || s.length() > 20) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!(isdigit((unsigned char)c) || (c == '-' && i == 0))) return false;
  }
  return s != "-";
}

bool validToken(const String &s) {
  int colon = s.indexOf(':');
  if (colon < 5 || s.length() - colon - 1 < 30 || s.length() > 100) return false;
  for (int i = 0; i < colon; i++) if (!isdigit((unsigned char)s[i])) return false;
  for (size_t i = colon + 1; i < s.length(); i++) {
    char c = s[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
  }
  return true;
}

bool validHost(const String &s) {
  if (s.length() == 0 || s.length() > 30 || s[0] == '-' || s[s.length() - 1] == '-') return false;
  for (char c : s) if (!(isdigit((unsigned char)c) || (c >= 'a' && c <= 'z') || c == '-')) return false;
  return true;
}

// Iterate "id name" lines; callback returns false to stop
void forEachUser(const String &users, std::function<bool(const String &, const String &)> cb) {
  int s = 0, n = users.length();
  while (s < n) {
    int e = users.indexOf('\n', s); if (e < 0) e = n;
    String line = users.substring(s, e); line.trim(); s = e + 1;
    if (!line.length()) continue;
    int sp = line.indexOf(' ');
    String id = sp < 0 ? line : line.substring(0, sp);
    String name = sp < 0 ? "" : line.substring(sp + 1); name.trim();
    if (!cb(id, name)) return;
  }
}

bool isListedUserIn(const String &users, const String &id) {
  bool found = false;
  forEachUser(users, [&](const String &u, const String &) { if (u == id) { found = true; return false; } return true; });
  return found;
}

bool isListedUser(const String &id) { return isListedUserIn(cfg.users, id); }

bool isAuthorized(const String &id) { return (cfg.admin.length() && id == cfg.admin) || isListedUser(id); }

// ---------- Guests (temporary access) ----------
void forEachGuest(std::function<bool(const String &, long, const String &)> cb) {
  forEachUser(cfg.guests, [&](const String &id, const String &rest) {
    int sp = rest.indexOf(' ');
    long exp = (sp < 0 ? rest : rest.substring(0, sp)).toInt();
    String name = sp < 0 ? "" : rest.substring(sp + 1);
    return cb(id, exp, name);
  });
}

long guestExpiry(const String &id) {
  long exp = 0;
  forEachGuest([&](const String &g, long e, const String &) { if (g == id) { exp = e; return false; } return true; });
  return exp;
}

bool isGuest(const String &id) { long e = guestExpiry(id); return e > 0 && nowEpoch() > 0 && nowEpoch() < e; }

String guestName(const String &id) {
  String n;
  forEachGuest([&](const String &g, long, const String &nm) { if (g == id) { n = nm; return false; } return true; });
  return n;
}

// Rewrite the guest list without `dropId` and without expired entries
void rebuildGuests(const String &dropId) {
  String out; long now = nowEpoch();
  forEachGuest([&](const String &g, long e, const String &n) {
    if (g != dropId && (now == 0 || e > now)) out += g + " " + String(e) + (n.length() ? " " + n : "") + "\n";
    return true;
  });
  if (out != cfg.guests) { cfg.guests = out; prefs.putString("guests", cfg.guests); }
}

bool addGuest(const String &id, int hours, const String &name) {
  if (!validChatId(id) || !nowEpoch()) return false;
  hours = constrain(hours, 1, 720);
  rebuildGuests(id);
  String n = cleanName(name, 32);
  cfg.guests += id + " " + String(nowEpoch() + hours * 3600L) + (n.length() ? " " + n : "") + "\n";
  prefs.putString("guests", cfg.guests);
  return true;
}

Role roleOf(const String &id) {
  if (cfg.admin.length() && id == cfg.admin) return R_ADMIN;
  if (isListedUser(id)) return R_USER;
  if (isGuest(id)) return R_GUEST;
  return R_NONE;
}

String userLabel(const String &id, const String &fallbackName = "") {
  if (cfg.admin.length() && id == cfg.admin) return "админ";
  String label;
  forEachUser(cfg.users, [&](const String &u, const String &n) { if (u == id) { label = n; return false; } return true; });
  if (!label.length()) { String g = guestName(id); if (g.length()) label = "гость " + g; }
  if (!label.length()) label = fallbackName;
  return label.length() ? label + " (" + id + ")" : id;
}

String fmtUntil(long exp) {
  time_t t = exp; struct tm tmv; localtime_r(&t, &tmv); char b[20];
  strftime(b, sizeof b, "%d.%m %H:%M", &tmv); return String(b);
}

// Validate + normalise textarea input. Appends problems to errs.
String normalizeUsers(const String &raw, String &errs) {
  String out; int lineNo = 0, s = 0, n = raw.length();
  while (s <= n) {
    int e = raw.indexOf('\n', s); if (e < 0) e = n;
    String line = raw.substring(s, e); line.replace("\r", ""); line.replace("\t", " "); line.trim();
    s = e + 1; lineNo++;
    if (!line.length() || line[0] == '#') { if (e >= n) break; continue; }
    int sp = line.indexOf(' ');
    String id = sp < 0 ? line : line.substring(0, sp);
    String name = sp < 0 ? "" : line.substring(sp + 1); name.trim();
    if (name.length() > 32) name = name.substring(0, 32);
    if (!validChatId(id)) errs += "Строка " + String(lineNo) + ": «" + id + "» — не chat_id, пропущена. ";
    else if (!isListedUserIn(out, id)) out += id + (name.length() ? " " + name : "") + "\n";
    if (e >= n) break;
  }
  return out;
}

// ---------- Config storage ----------
void saveConfig() {
  prefs.putString("token", cfg.token);
  prefs.putString("admin", cfg.admin);
  prefs.putString("users", cfg.users);
  prefs.putString("host", cfg.host);
  prefs.putString("webPass", cfg.webPass);
  prefs.putString("tz", cfg.tz);
  prefs.putUChar("type", cfg.type);
  prefs.putUShort("doorMs", cfg.doorMs);
  prefs.putUShort("camMs", cfg.camMs);
  prefs.putUShort("thresh", cfg.threshold);
  prefs.putUShort("debMs", cfg.debounceMs);
  prefs.putUShort("maxAge", cfg.maxAgeS);
  prefs.putBool("notifyAll", cfg.notifyAll);
  prefs.putBool("tlsVerify", cfg.tlsVerify);
  prefs.putString("guests", cfg.guests);
  prefs.putBool("confirm", cfg.confirmOpen);
  prefs.putBool("mqttOn", cfg.mqttOn);
  prefs.putString("mqttHost", cfg.mqttHost);
  prefs.putUShort("mqttPort", cfg.mqttPort);
  prefs.putString("mqttUser", cfg.mqttUser);
  prefs.putString("mqttPass", cfg.mqttPass);
}

void loadConfig() {
  cfg.token      = prefs.getString("token", "");
  cfg.admin      = prefs.getString("admin", "");
  cfg.users      = prefs.getString("users", "");
  cfg.host       = prefs.getString("host", "intercom");
  cfg.webPass    = prefs.getString("webPass", "");
  cfg.tz         = prefs.getString("tz", "CET-1CEST,M3.5.0,M10.5.0/3");
  cfg.type       = prefs.getUChar("type", INTERCOM_FULL);
  cfg.doorMs     = prefs.getUShort("doorMs", 500);
  cfg.camMs      = prefs.getUShort("camMs", 2000);
  cfg.threshold  = prefs.getUShort("thresh", 15);
  cfg.debounceMs = prefs.getUShort("debMs", 5000);
  cfg.maxAgeS    = prefs.getUShort("maxAge", 60);
  cfg.notifyAll  = prefs.getBool("notifyAll", false);
  cfg.tlsVerify  = prefs.getBool("tlsVerify", true);
  cfg.guests     = prefs.getString("guests", "");
  cfg.confirmOpen = prefs.getBool("confirm", false);
  cfg.mqttOn     = prefs.getBool("mqttOn", false);
  cfg.mqttHost   = prefs.getString("mqttHost", "");
  cfg.mqttPort   = prefs.getUShort("mqttPort", 1883);
  cfg.mqttUser   = prefs.getString("mqttUser", "");
  cfg.mqttPass   = prefs.getString("mqttPass", "");
  if (cfg.type > INTERCOM_SIMPLE) cfg.type = INTERCOM_FULL;
  if (!validHost(cfg.host)) cfg.host = "intercom";
}

// Firmware 1.x kept settings in EEPROM. Copy them once into NVS.
struct LegacySettings {
  int cameraActivationTime, callThreshold, callDebounceTime, doorActivationTime;
  char botToken[50]; char chatId[20]; char authorizedChats[200];
  unsigned long rebootCount, lastUptime; int watchdogTimeout; uint8_t intercomType;
};

void migrateLegacy() {
  LegacySettings s;
  EEPROM.begin(sizeof(LegacySettings));
  EEPROM.get(0, s);
  EEPROM.end();
  bool valid = s.cameraActivationTime >= 100 && s.cameraActivationTime <= 10000 &&
               s.callThreshold >= 0 && s.callThreshold <= 1023 &&
               s.callDebounceTime >= 1000 && s.callDebounceTime <= 60000 &&
               s.watchdogTimeout >= 1 && s.watchdogTimeout <= 60;
  if (!valid) { Serial.println("No legacy settings"); return; }
  s.botToken[sizeof s.botToken - 1] = 0; s.chatId[sizeof s.chatId - 1] = 0;
  s.authorizedChats[sizeof s.authorizedChats - 1] = 0;
  cfg.camMs = s.cameraActivationTime; cfg.threshold = s.callThreshold; cfg.debounceMs = s.callDebounceTime;
  if (s.doorActivationTime >= 100 && s.doorActivationTime <= 10000) cfg.doorMs = s.doorActivationTime;
  if (s.intercomType <= INTERCOM_SIMPLE) cfg.type = s.intercomType;
  if (validToken(String(s.botToken))) cfg.token = s.botToken;
  if (validChatId(String(s.chatId))) cfg.admin = s.chatId;
  String csv = s.authorizedChats; csv.replace(",", "\n"); String ignored;
  cfg.users = normalizeUsers(csv, ignored);
  bootCount = s.rebootCount;
  Serial.println("Legacy settings migrated");
}

// ---------- Hardware ----------
void pulse(int pin, uint16_t ms) {
  led(true); digitalWrite(pin, HIGH);
  delayWdt(ms);
  digitalWrite(pin, LOW); led(false);
}

bool displayOn() { return windowActive(displayStart, displayLen); }

bool viewDisplay() {
  if (!hasDisplay() || busy) return false;
  busy = true;
  extendWindow(ignoreStart, ignoreLen, cfg.camMs + 1000);
  pulse(PIN_CAMERA, cfg.camMs);
  extendWindow(displayStart, displayLen, DISPLAY_ON_MS);
  extendWindow(ignoreStart, ignoreLen, DISPLAY_ON_MS);
  busy = false;
  return true;
}

bool openDoor() {
  if (busy) return false;
  busy = true;
  extendWindow(ignoreStart, ignoreLen, 60000);    // no call detection while we operate
  if (hasDisplay() && !displayOn()) {
    pulse(PIN_CAMERA, cfg.camMs);                  // pressing again would turn the screen off
    extendWindow(displayStart, displayLen, DISPLAY_ON_MS);
    delayWdt(500);
  }
  pulse(PIN_DOOR, cfg.doorMs);
  ignoreLen = 0;
  extendWindow(ignoreStart, ignoreLen, hasDisplay() && displayOn() ? DISPLAY_ON_MS : 2000);
  busy = false;
  return true;
}

// ---------- Telegram ----------
String mainKeyboard(Role r = R_USER) {
  if (r == R_GUEST) return hasDisplay() ? "[[\"🚪 Open Intercom\", \"📺 View Display\"]]" : "[[\"🚪 Open Intercom\"]]";
  return hasDisplay() ? "[[\"🚪 Open Intercom\", \"📺 View Display\"], [\"📊 Status\"]]"
                      : "[[\"🚪 Open Intercom\"], [\"📊 Status\"]]";
}

String btn(const String &text, const String &data) {
  return "{\"text\":\"" + text + "\",\"callback_data\":\"" + data + "\"}";
}

// Replace the text of a message that carried inline buttons and drop the buttons
void editMsg(telegramMessage &m, const String &text) {
  bot.sendMessageWithInlineKeyboard(m.chat_id, text, "", "[]", m.message_id);
}

void applyTelegram() {
  bot.updateToken(cfg.token);
  if (cfg.tlsVerify) tgClient.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  else tgClient.setInsecure();
  tgOk = false; tgBotName = ""; tgCheckedOnce = false;
}

// Call notification with an "Open" button right in the message
void notifyCall(const String &msg) {
  String ts = String(nowEpoch());
  String kb = "[[" + btn("🚪 Открыть", "c:" + ts);
  if (hasDisplay()) kb += "," + btn("📺 Экран", "v:" + ts);
  kb += "]]";
  if (cfg.admin.length()) bot.sendMessageWithInlineKeyboard(cfg.admin, msg, "", kb);
  if (cfg.notifyAll) forEachUser(cfg.users, [&](const String &id, const String &) {
    if (id != cfg.admin) { esp_task_wdt_reset(); bot.sendMessageWithInlineKeyboard(id, msg, "", kb); }
    return true;
  });
}

void notifyAdmin(const String &id, const String &msg) {
  if (cfg.admin.length() && id != cfg.admin) bot.sendMessage(cfg.admin, msg, "");
}

String statusText() {
  String s = "📊 Статус\n";
  s += "Тип: " + String(hasDisplay() ? "с экраном" : "простой (без экрана)") + "\n";
  s += "WiFi: " + WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)\n";
  s += "Адрес: http://" + cfg.host + ".local  (" + WiFi.localIP().toString() + ")\n";
  s += "Работает: " + fmtUptime(millis() / 1000) + "\n";
  s += "Прошивка: " FW_VERSION " / " BOARD_NAME;
  return s;
}

// Person who is not on any list wrote to the bot: tell them their id and ask the admin
void handleStranger(telegramMessage &m) {
  String id = m.chat_id;
  addLog("⛔ Отказ: " + m.from_name + " (" + id + ")");
  if (millis() - lastUnauthReply < 10000) return;          // simple anti-spam
  lastUnauthReply = millis();
  bool ask = cfg.admin.length() && (id != lastReqId || millis() - lastReqMs > 600000);
  bot.sendMessage(id, "❌ Нет доступа.\nВаш chat_id: " + id +
                  (ask ? "\nЗапрос отправлен администратору." : "\nПопросите администратора добавить его."), "");
  if (!ask) return;
  lastReqId = id; lastReqMs = millis();
  String name = cleanName(m.from_name, 40);
  String kb = "[[" + btn("🕐 Гость на " + String(REQ_GUEST_H) + " ч", "g:" + id + ":" + name) + "," +
              btn("✅ Добавить", "u:" + id + ":" + name) + "]]";
  bot.sendMessageWithInlineKeyboard(cfg.admin, "🙋 " + (name.length() ? name : String("Кто-то")) +
                                    " (" + id + ") просит доступ к домофону", "", kb);
}

bool doOpen(const String &id, const String &who, Role r) {
  if (!openDoor()) return false;
  addLog("🚪 Открыто: " + who);
  if (r == R_GUEST) notifyAdmin(id, "🚪 Дверь открыл " + who);
  return true;
}

// Inline buttons: c:<ts> open from a call, v:<ts> screen from a call, o:<ts> confirmed open,
// x cancel, g:<id>:<name> guest access, u:<id>:<name> permanent access
void handleCallback(telegramMessage &m) {
  String id = m.chat_id, d = m.text;
  Role r = roleOf(id);
  if (r == R_NONE) { bot.answerCallbackQuery(m.query_id, "Нет доступа"); return; }
  String who = userLabel(id, m.from_name);
  int c1 = d.indexOf(':');
  String kind = c1 < 0 ? d : d.substring(0, c1), arg = c1 < 0 ? "" : d.substring(c1 + 1);

  if (kind == "x") { bot.answerCallbackQuery(m.query_id, "Отменено"); editMsg(m, "Отменено"); return; }

  if (kind == "c" || kind == "v" || kind == "o") {
    long ts = arg.toInt(), win = kind == "o" ? CONFIRM_BTN_S : CALL_BTN_S;
    String head = kind == "o" ? "" : m.reply_to_text + "\n\n";
    if (ts > 0 && nowEpoch() && nowEpoch() - ts > win) {
      bot.answerCallbackQuery(m.query_id, "Кнопка устарела");
      editMsg(m, head + "⏱ Кнопка устарела, дверь не открыта");
      return;
    }
    if (kind == "v") {
      bool ok = viewDisplay();
      bot.answerCallbackQuery(m.query_id, ok ? "Экран включён" : "Занято");
      if (ok) addLog("📺 Экран: " + who);
      return;                                    // keep the "Open" button for later
    }
    bool ok = doOpen(id, who, r);
    bot.answerCallbackQuery(m.query_id, ok ? "Дверь открыта" : "Занято, попробуйте ещё раз");
    if (ok) editMsg(m, head + "✅ Открыто: " + who);
    return;
  }

  if (kind == "g" || kind == "u") {
    if (r != R_ADMIN) { bot.answerCallbackQuery(m.query_id, "Только для администратора"); return; }
    int c2 = arg.indexOf(':');
    String uid = c2 < 0 ? arg : arg.substring(0, c2), name = c2 < 0 ? "" : arg.substring(c2 + 1);
    if (!validChatId(uid)) { bot.answerCallbackQuery(m.query_id, "Неверный chat_id"); return; }
    String res;
    if (kind == "g") {
      if (!addGuest(uid, REQ_GUEST_H, name)) { bot.answerCallbackQuery(m.query_id, "Время не синхронизировано"); return; }
      res = "🕐 Гость до " + fmtUntil(guestExpiry(uid)) + ": " + userLabel(uid, name);
      bot.sendMessageWithReplyKeyboard(uid, "✅ Вам открыт доступ к домофону до " + fmtUntil(guestExpiry(uid)), "", mainKeyboard(R_GUEST), true);
    } else {
      if (!isListedUser(uid)) {
        String errs; cfg.users = normalizeUsers(cfg.users + uid + (name.length() ? " " + name : "") + "\n", errs);
        prefs.putString("users", cfg.users);
      }
      rebuildGuests(uid);
      res = "✅ Добавлен: " + userLabel(uid);
      bot.sendMessageWithReplyKeyboard(uid, "✅ Вам открыт доступ к домофону", "", mainKeyboard(R_USER), true);
    }
    addLog(res);
    bot.answerCallbackQuery(m.query_id, "Готово");
    editMsg(m, m.reply_to_text + "\n\n" + res);
    return;
  }
  bot.answerCallbackQuery(m.query_id, "");
}

void handleMessage(telegramMessage &m) {
  if (m.type == "callback_query") { handleCallback(m); return; }
  String id = m.chat_id, text = m.text; text.trim();
  Role r = roleOf(id);
  if (r == R_NONE) { handleStranger(m); return; }

  bool isOpen = text == "🚪 Open Intercom" || text == "/open";
  bool isView = text == "📺 View Display" || text == "/display";
  String who = userLabel(id, m.from_name);

  // Drop commands that were delivered late (e.g. sent while the device was offline)
  long sent = m.date.toInt();
  long age = nowEpoch() && sent > 0 ? nowEpoch() - sent : 0;
  if ((isOpen || isView) && age > (long)cfg.maxAgeS) {
    addLog("⏱ Устаревшая команда (" + String(age) + " с) от " + who);
    bot.sendMessage(id, "⏱ Команда пришла с опозданием " + String(age) + " с и не выполнена. Нажмите ещё раз.", "");
    return;
  }

  bool isAdmin = r == R_ADMIN;

  if (isOpen) {
    if (cfg.confirmOpen) {
      String kb = "[[" + btn("✅ Да, открыть", "o:" + String(nowEpoch())) + "," + btn("Отмена", "x") + "]]";
      bot.sendMessageWithInlineKeyboard(id, "Открыть дверь?", "", kb);
    } else if (doOpen(id, who, r)) bot.sendMessage(id, "✅ Дверь открыта", "");
    else bot.sendMessage(id, "⏳ Занято, попробуйте ещё раз", "");
  } else if (isView) {
    if (!hasDisplay()) bot.sendMessage(id, "У этого домофона нет экрана", "");
    else if (viewDisplay()) { addLog("📺 Экран: " + who); bot.sendMessage(id, "✅ Экран включён", ""); }
    else bot.sendMessage(id, "⏳ Занято, попробуйте ещё раз", "");
  } else if (text == "/id") {
    bot.sendMessage(id, "Ваш chat_id: " + id, "");
  } else if (r == R_GUEST) {
    bot.sendMessageWithReplyKeyboard(id, "🏠 Домофон\nГостевой доступ до " + fmtUntil(guestExpiry(id)) +
                                     "\nКнопка внизу открывает дверь.", "", mainKeyboard(R_GUEST), true);
  } else if (text == "📊 Status" || text == "/status") {
    bot.sendMessageWithReplyKeyboard(id, statusText(), "", mainKeyboard(), true);
  } else if (text.startsWith("/allow")) {
    if (!isAdmin) { bot.sendMessage(id, "❌ Только для администратора", ""); return; }
    String arg = text.substring(6); arg.trim();
    int sp = arg.indexOf(' ');
    String uid = sp < 0 ? arg : arg.substring(0, sp);
    String name = sp < 0 ? "" : arg.substring(sp + 1); name.trim();
    if (!validChatId(uid)) { bot.sendMessage(id, "Использование: /allow <chat_id> [имя]", ""); return; }
    if (isListedUser(uid)) { bot.sendMessage(id, "Уже есть в списке: " + uid, ""); return; }
    String errs; cfg.users = normalizeUsers(cfg.users + uid + (name.length() ? " " + name : "") + "\n", errs);
    prefs.putString("users", cfg.users);
    rebuildGuests(uid);
    addLog("➕ Добавлен " + userLabel(uid));
    bot.sendMessage(id, "✅ Добавлен: " + userLabel(uid), "");
  } else if (text.startsWith("/deny")) {
    if (!isAdmin) { bot.sendMessage(id, "❌ Только для администратора", ""); return; }
    String uid = text.substring(5); uid.trim();
    bool wasGuest = guestExpiry(uid) > 0;
    if (!isListedUser(uid) && !wasGuest) { bot.sendMessage(id, "Нет в списке: " + uid, ""); return; }
    String label = userLabel(uid), rebuilt;
    forEachUser(cfg.users, [&](const String &u, const String &n) {
      if (u != uid) rebuilt += u + (n.length() ? " " + n : "") + "\n"; return true; });
    cfg.users = rebuilt; prefs.putString("users", cfg.users);
    rebuildGuests(uid);
    addLog("➖ Удалён " + label);
    bot.sendMessage(id, "✅ Удалён: " + label, "");
  } else if (text.startsWith("/guest")) {
    if (!isAdmin) { bot.sendMessage(id, "❌ Только для администратора", ""); return; }
    String arg = text.substring(6); arg.trim();
    int sp = arg.indexOf(' ');
    String uid = sp < 0 ? arg : arg.substring(0, sp), rest = sp < 0 ? "" : arg.substring(sp + 1);
    rest.trim();
    int hours = REQ_GUEST_H; String name = rest;
    if (rest.length() && isdigit((unsigned char)rest[0])) {
      int sp2 = rest.indexOf(' ');
      hours = (sp2 < 0 ? rest : rest.substring(0, sp2)).toInt();
      name = sp2 < 0 ? "" : rest.substring(sp2 + 1);
    }
    if (!validChatId(uid) || hours < 1) { bot.sendMessage(id, "Использование: /guest <chat_id> [часов] [имя]\nНапример: /guest 123456789 48 Курьер", ""); return; }
    if (!addGuest(uid, hours, name)) { bot.sendMessage(id, "⚠️ Время не синхронизировано, попробуйте позже", ""); return; }
    String res = "🕐 Гость до " + fmtUntil(guestExpiry(uid)) + ": " + userLabel(uid);
    addLog(res);
    bot.sendMessage(id, res, "");
  } else if (text == "/list") {
    if (!isAdmin) { bot.sendMessage(id, "❌ Только для администратора", ""); return; }
    rebuildGuests("");
    String list = "👥 Админ: " + cfg.admin + "\nПользователи:\n"; int count = 0;
    forEachUser(cfg.users, [&](const String &u, const String &n) { list += "• " + u + (n.length() ? " — " + n : "") + "\n"; count++; return true; });
    if (!count) list += "(пусто)\n";
    String gl;
    forEachGuest([&](const String &g, long e, const String &n) { gl += "• " + g + (n.length() ? " — " + n : "") + " до " + fmtUntil(e) + "\n"; return true; });
    if (gl.length()) list += "Гости:\n" + gl;
    bot.sendMessage(id, list, "");
  } else {   // /start and anything else
    String w = "🏠 Домофон\n\nКнопки внизу: открыть дверь" + String(hasDisplay() ? ", включить экран" : "") + ", статус.\n";
    w += "Команды: /open /status /id";
    if (hasDisplay()) w += " /display";
    if (isAdmin) w += "\nАдмин: /allow <chat_id> [имя], /deny <chat_id>, /guest <chat_id> [часов] [имя], /list";
    bot.sendMessageWithReplyKeyboard(id, w, "", mainKeyboard(), true);
  }
}

void pollTelegram() {
  if (!cfg.token.length() || WiFi.status() != WL_CONNECTED) return;

  if (!tgCheckedOnce || millis() - lastTgCheck > TG_CHECK_MS) {
    tgCheckedOnce = true; lastTgCheck = millis();
    bool was = tgOk;
    tgOk = bot.getMe();
    if (tgOk) tgBotName = bot.userName;
    if (tgOk != was) addLog(tgOk ? "💬 Telegram подключён (@" + tgBotName + ")" : "⚠️ Telegram недоступен");
  }
  if (millis() - lastPoll < TG_POLL_MS) return;
  lastPoll = millis();

  for (int round = 0; round < 3; round++) {
    esp_task_wdt_reset();
    int n = bot.getUpdates(bot.last_message_received + 1);
    if (n <= 0) break;
    tgOk = true;
    for (int i = 0; i < n; i++) { esp_task_wdt_reset(); handleMessage(bot.messages[i]); }
  }
}

// ---------- Call detection (full type) ----------
void checkCall() {
  if (!hasDisplay() || busy || windowActive(ignoreStart, ignoreLen)) { lastCallState = false; return; }
  int v = analogRead(PIN_CALL);
  bool active = v < cfg.threshold;
  if (active && !lastCallState && millis() - lastCallMs > cfg.debounceMs) {
    lastCallMs = millis();
    addLog("📞 Звонок (A=" + String(v) + ")");
    if (mqtt.connected()) mqtt.publish((mqttBase() + "/call").c_str(), "ON");
    notifyCall("📞 Звонок в домофон!");
  }
  lastCallState = active;
}

// ---------- MQTT / Home Assistant ----------
String devUid() { String m = WiFi.macAddress(); m.replace(":", ""); m.toLowerCase(); return "intercom_" + m; }

void mqttDiscovery() {
  String b = mqttBase(), u = devUid();
  String dev = ",\"avty_t\":\"" + b + "/status\",\"dev\":{\"ids\":[\"" + u + "\"],\"name\":\"Домофон " + cfg.host +
               "\",\"mf\":\"DIY\",\"mdl\":\"" BOARD_NAME "\",\"sw\":\"" FW_VERSION "\",\"cu\":\"http://" + cfg.host + ".local\"}}";
  mqtt.publish(("homeassistant/button/" + u + "/open/config").c_str(),
    ("{\"name\":\"Открыть дверь\",\"uniq_id\":\"" + u + "_open\",\"cmd_t\":\"" + b + "/cmd\",\"pl_prs\":\"open\",\"ic\":\"mdi:door-open\"" + dev).c_str(), true);
  mqtt.publish(("homeassistant/sensor/" + u + "/event/config").c_str(),
    ("{\"name\":\"Последнее событие\",\"uniq_id\":\"" + u + "_event\",\"stat_t\":\"" + b + "/event\",\"ic\":\"mdi:history\"" + dev).c_str(), true);
  String dispTopic = "homeassistant/button/" + u + "/display/config", callTopic = "homeassistant/binary_sensor/" + u + "/call/config";
  if (hasDisplay()) {
    mqtt.publish(dispTopic.c_str(),
      ("{\"name\":\"Включить экран\",\"uniq_id\":\"" + u + "_display\",\"cmd_t\":\"" + b + "/cmd\",\"pl_prs\":\"display\",\"ic\":\"mdi:monitor\"" + dev).c_str(), true);
    mqtt.publish(callTopic.c_str(),
      ("{\"name\":\"Звонок\",\"uniq_id\":\"" + u + "_call\",\"stat_t\":\"" + b + "/call\",\"off_dly\":30,\"ic\":\"mdi:bell-ring\"" + dev).c_str(), true);
  } else {                                   // simple intercom: remove these entities
    mqtt.publish(dispTopic.c_str(), "", true);
    mqtt.publish(callTopic.c_str(), "", true);
  }
}

void onMqtt(char *topic, byte *payload, unsigned int len) {
  String cmd; for (unsigned i = 0; i < len; i++) cmd += (char)payload[i];
  cmd.trim();
  if (cmd == "open") { if (openDoor()) addLog("🚪 Открыто через Home Assistant"); }
  else if (cmd == "display") { if (viewDisplay()) addLog("📺 Экран через Home Assistant"); }
}

void mqttRestart() { mqtt.disconnect(); mqttWasUp = false; lastMqttTry = millis() - MQTT_RETRY_MS; }

void mqttLoop() {
  if (!cfg.mqttOn || !cfg.mqttHost.length() || WiFi.status() != WL_CONNECTED) {
    if (mqtt.connected()) mqtt.disconnect();
    return;
  }
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (mqttWasUp) { mqttWasUp = false; addLog("⚠️ MQTT отключён"); }
  if (millis() - lastMqttTry < MQTT_RETRY_MS) return;
  lastMqttTry = millis();
  esp_task_wdt_reset();
  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  String b = mqttBase();
  bool ok = mqtt.connect(devUid().c_str(),
                         cfg.mqttUser.length() ? cfg.mqttUser.c_str() : nullptr,
                         cfg.mqttUser.length() ? cfg.mqttPass.c_str() : nullptr,
                         (b + "/status").c_str(), 0, true, "offline");
  esp_task_wdt_reset();
  if (!ok) return;
  mqttWasUp = true;
  mqtt.publish((b + "/status").c_str(), "online", true);
  mqtt.subscribe((b + "/cmd").c_str());
  mqttDiscovery();
  addLog("🏠 MQTT подключён (" + cfg.mqttHost + ")");
}

String mqttStatus() {
  if (!cfg.mqttOn) return "<span class='mut'>выключено</span>";
  if (mqtt.connected()) return "<span class='ok'>подключено</span> · " + esc(cfg.mqttHost);
  return "<span class='err'>нет связи</span> · " + esc(cfg.mqttHost) + " (код " + String(mqtt.state()) + ")";
}

// ---------- Web portal ----------
const char CSS[] PROGMEM = R"CSS(
:root{--bg:#f4f5f7;--card:#fff;--fg:#1d2330;--mut:#6b7280;--acc:#2563eb;--bad:#dc2626;--ok:#15803d;--bd:#e3e6eb}
@media(prefers-color-scheme:dark){:root{--bg:#111318;--card:#1b1e25;--fg:#e8eaf0;--mut:#9aa3b2;--acc:#3b82f6;--bad:#f87171;--ok:#4ade80;--bd:#2c313b}}
*{box-sizing:border-box}body{margin:0;font:15px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--fg)}
nav{display:flex;gap:4px;flex-wrap:wrap;padding:10px 16px;background:var(--card);border-bottom:1px solid var(--bd)}
nav a{color:var(--fg);text-decoration:none;padding:6px 12px;border-radius:8px}nav a.on{background:var(--acc);color:#fff}
main{max-width:760px;margin:0 auto;padding:16px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:16px;margin:0 0 16px}
h2{font-size:16px;margin:0 0 8px}label{display:block;margin:14px 0 4px;font-weight:600}
small,.mut{color:var(--mut)}small{display:block;margin-top:4px}
input,select,textarea{width:100%;padding:9px 10px;border:1px solid var(--bd);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit}
textarea{min-height:130px;font-family:ui-monospace,Menlo,monospace;font-size:14px}
.chk{display:flex;gap:8px;align-items:center;font-weight:400}.chk input{width:auto;margin:0}
button{padding:10px 16px;border:0;border-radius:8px;background:var(--acc);color:#fff;font:inherit;font-weight:600;cursor:pointer}
button.bad{background:var(--bad)}button.big{width:100%;padding:16px;font-size:18px}
table{width:100%;border-collapse:collapse}td{padding:6px 4px;border-bottom:1px solid var(--bd);vertical-align:top;overflow-wrap:anywhere}
td:first-child{color:var(--mut);white-space:nowrap;padding-right:12px;width:1%}
.warn{border-color:var(--bad)}.ok{color:var(--ok)}.err{color:var(--bad)}.row{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}
)CSS";

bool auth() {
  if (!cfg.webPass.length()) return true;
  if (server.authenticate("admin", cfg.webPass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Intercom");
  return false;
}

bool csrfOk() {
  if (server.arg("csrf") == csrf) return true;
  server.send(403, "text/plain; charset=utf-8", "Ошибка проверки формы. Обновите страницу и повторите.");
  return false;
}

String csrfField() { return "<input type='hidden' name='csrf' value='" + csrf + "'>"; }

void redirect(const String &to) { server.sendHeader("Location", to); server.send(303, "text/plain", ""); }

void sendPage(const char *active, const String &title, const String &body) {
  String h; h.reserve(body.length() + 3200);
  h += F("<!doctype html><html lang='ru'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>");
  h += esc(title); h += F("</title><style>"); h += FPSTR(CSS); h += F("</style></head><body><nav>");
  const char *items[][2] = {{"/", "Главная"}, {"/guests", "Гости"}, {"/settings", "Настройки"}, {"/wifi", "WiFi"}, {"/update", "Обновление"}};
  for (auto &it : items) {
    h += "<a href='"; h += it[0]; h += "'"; if (!strcmp(active, it[0])) h += " class='on'"; h += ">"; h += it[1]; h += "</a>";
  }
  h += "</nav><main>"; h += body; h += F("</main></body></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

String row(const String &k, const String &v) { return "<tr><td>" + k + "</td><td>" + v + "</td></tr>"; }

void handleRoot() {
  if (!auth()) return;
  String b;
  if (!cfg.token.length() || !cfg.admin.length())
    b += "<div class='card warn'><b class='err'>Бот не настроен.</b> Укажите токен и chat_id администратора в <a href='/settings'>настройках</a>.</div>";
  if (!cfg.webPass.length())
    b += "<div class='card warn'><b class='err'>Пароль портала не задан.</b> Любой в вашей сети может менять настройки и прошивку. Задайте его в <a href='/settings'>настройках</a>.</div>";

  b += "<div class='card'><form method='post' action='/open'" + String(cfg.confirmOpen ? " onsubmit=\"return confirm('Открыть дверь?')\"" : "") +
       ">" + csrfField() + "<button class='big'>🚪 Открыть дверь</button></form>";
  if (hasDisplay()) b += "<div class='row'><form method='post' action='/display'>" + csrfField() + "<button>📺 Включить экран</button></form></div>";
  b += "</div>";

  String tg;
  if (!cfg.token.length()) tg = "<span class='err'>не настроен</span>";
  else if (tgOk) tg = "<span class='ok'>подключён</span> @" + esc(tgBotName);
  else if (!tgCheckedOnce) tg = "<span class='mut'>проверяется…</span>";
  else tg = "<span class='err'>нет связи</span>" + String(cfg.tlsVerify ? "<small>Если не проходит, проверьте токен или выключите проверку сертификата в настройках.</small>" : "");
  b += "<div class='card'><h2>Состояние</h2><table>";
  b += row("Домофон", hasDisplay() ? "с экраном" : "простой, без экрана");
  b += row("Telegram", tg);
  b += row("Home Assistant", mqttStatus());
  int guests = 0; long now = nowEpoch();
  forEachGuest([&](const String &, long e, const String &) { if (e > now) guests++; return true; });
  b += row("Гости", guests ? "<a href='/guests'>" + String(guests) + " с активным доступом</a>" : "нет");
  b += row("Адрес", "http://" + esc(cfg.host) + ".local · " + WiFi.localIP().toString());
  b += row("WiFi", esc(WiFi.SSID()) + " · " + String(WiFi.RSSI()) + " dBm");
  b += row("Время", timeValid() ? fmtTime(time(nullptr), 0) : "<span class='err'>не синхронизировано</span>");
  b += row("Работает", fmtUptime(millis() / 1000) + " · загрузка №" + String(bootCount));
  b += row("Память", String(ESP.getFreeHeap() / 1024) + " КБ свободно");
  if (hasDisplay()) b += row("Датчик звонка", "A=" + String(analogRead(PIN_CALL)) + " (порог " + String(cfg.threshold) + ")");
  b += row("Прошивка", FW_VERSION " · " BOARD_NAME);
  b += "</table></div>";

  b += "<div class='card'><h2>Журнал</h2>";
  if (!logCount) b += "<p class='mut'>Пока пусто</p>";
  else {
    b += "<table>";
    for (int i = 0; i < logCount; i++) {
      LogEntry &e = logBuf[(logHead - 1 - i + LOG_SIZE) % LOG_SIZE];
      b += row(fmtTime(e.t, e.ms), esc(e.text));
    }
    b += "</table>";
  }
  b += "<p class='mut'>Хранится в памяти до перезагрузки, последние " + String(LOG_SIZE) + " событий.</p></div>";

  b += "<div class='card'><h2>Действия</h2><div class='row'>";
  b += "<form method='post' action='/tgtest'>" + csrfField() + "<button>Тестовое сообщение админу</button></form>";
  b += "<form method='post' action='/reboot' onsubmit=\"return confirm('Перезагрузить?')\">" + csrfField() + "<button class='bad'>Перезагрузить</button></form>";
  b += "</div></div>";
  sendPage("/", "Домофон", b);
}

void handleOpen() {
  if (!auth() || !csrfOk()) return;
  if (openDoor()) addLog("🚪 Открыто через веб-портал");
  redirect("/");
}

void handleDisplay() {
  if (!auth() || !csrfOk()) return;
  if (viewDisplay()) addLog("📺 Экран через веб-портал");
  redirect("/");
}

void handleTgTest() {
  if (!auth() || !csrfOk()) return;
  bool ok = cfg.token.length() && cfg.admin.length() && bot.sendMessage(cfg.admin, "🧪 Тест из веб-портала", "");
  addLog(ok ? "🧪 Тестовое сообщение отправлено" : "🧪 Тестовое сообщение не отправлено");
  redirect("/");
}

void handleReboot() {
  if (!auth() || !csrfOk()) return;
  sendPage("/", "Перезагрузка", "<div class='card'>Перезагрузка… Страница обновится через 15 секунд.</div><meta http-equiv='refresh' content='15;url=/'>");
  delay(500); ESP.restart();
}

String settingsBody(const String &msg, const String &errs) {
  String b;
  if (msg.length()) b += "<div class='card'><b class='ok'>" + esc(msg) + "</b></div>";
  if (errs.length()) b += "<div class='card warn'><b class='err'>Не всё сохранено:</b> " + esc(errs) + "</div>";
  b += "<form method='post' action='/settings'>" + csrfField();

  b += "<div class='card'><h2>Telegram</h2>";
  b += "<label>Токен бота</label><input type='password' name='token' autocomplete='off' placeholder='";
  b += cfg.token.length() ? "сохранён — оставьте пустым, чтобы не менять" : "123456789:AA…";
  b += "'><small>Выдаёт @BotFather. После сохранения токен не показывается.</small>";
  b += "<label>chat_id администратора</label><input name='admin' value='" + esc(cfg.admin) + "' inputmode='numeric'>";
  b += "<small>Админ открывает дверь, получает уведомления и управляет списком через /allow, /deny, /list.</small>";
  b += "<label>Пользователи с доступом</label><textarea name='users' placeholder='123456789 Мама&#10;987654321 Соседка'>" + esc(cfg.users) + "</textarea>";
  b += "<small>По одному в строке: chat_id, затем через пробел имя (по желанию). Свой chat_id человек увидит, если напишет боту.</small>";
  b += "<label class='chk'><input type='checkbox' name='notifyAll'" + String(cfg.notifyAll ? " checked" : "") + ">Уведомлять о звонке всех пользователей, а не только админа</label>";
  b += "<label>Не выполнять команды старше, с</label><input type='number' name='maxAge' min='10' max='3600' value='" + String(cfg.maxAgeS) + "'>";
  b += "<small>Защита от того, что дверь откроется спустя время, если команда застряла, пока не было связи.</small>";
  b += "<label class='chk'><input type='checkbox' name='confirmOpen'" + String(cfg.confirmOpen ? " checked" : "") + ">Спрашивать подтверждение перед открытием</label>";
  b += "<small>После «Открыть» бот пришлёт кнопку «Да, открыть» (действует " + String(CONFIRM_BTN_S) + " с) — защита от случайного нажатия. Кнопка «Открыть» в уведомлении о звонке срабатывает сразу.</small>";
  b += "<label class='chk'><input type='checkbox' name='tlsVerify'" + String(cfg.tlsVerify ? " checked" : "") + ">Проверять сертификат Telegram (рекомендуется)</label>";
  b += "</div>";

  b += "<div class='card'><h2>Home Assistant (MQTT)</h2>";
  b += "<label class='chk'><input type='checkbox' name='mqttOn'" + String(cfg.mqttOn ? " checked" : "") + ">Включить</label>";
  b += "<label>Адрес брокера</label><input name='mqttHost' value='" + esc(cfg.mqttHost) + "' placeholder='IP брокера, например 192.168.1.10'>";
  b += "<label>Порт</label><input type='number' name='mqttPort' min='1' max='65535' value='" + String(cfg.mqttPort) + "'>";
  b += "<label>Логин</label><input name='mqttUser' value='" + esc(cfg.mqttUser) + "' autocomplete='off'>";
  b += "<label>Пароль</label><input type='password' name='mqttPass' autocomplete='off' placeholder='";
  b += cfg.mqttPass.length() ? "сохранён — оставьте пустым, чтобы не менять" : "";
  b += "'><small>Домофон появится в Home Assistant сам (MQTT discovery): кнопки «Открыть дверь» и «Включить экран», датчик «Звонок», «Последнее событие». Топики: " +
       esc(mqttBase()) + "/…</small>";
  b += "</div>";

  b += "<div class='card'><h2>Домофон</h2><label>Тип</label><select name='type'>";
  b += "<option value='1'" + String(cfg.type == INTERCOM_SIMPLE ? " selected" : "") + ">Простой — без экрана, только «Открыть»</option>";
  b += "<option value='0'" + String(cfg.type == INTERCOM_FULL ? " selected" : "") + ">С экраном — камера, экран, датчик звонка</option></select>";
  b += "<label>Сколько держать кнопку «Открыть», мс</label><input type='number' name='doorMs' min='100' max='10000' value='" + String(cfg.doorMs) + "'>";
  b += "<small>Выход GPIO" + String(PIN_DOOR) + " → твердотельное реле. 100–10000 мс.</small>";
  b += "<details" + String(hasDisplay() ? " open" : "") + "><summary class='mut' style='margin-top:14px'>Только для домофона с экраном</summary>";
  b += "<label>Сколько держать кнопку экрана, мс</label><input type='number' name='camMs' min='100' max='10000' value='" + String(cfg.camMs) + "'>";
  b += "<label>Порог датчика звонка (0–1023)</label><input type='number' name='threshold' min='0' max='1023' value='" + String(cfg.threshold) + "'>";
  b += "<small>Сейчас датчик показывает " + String(analogRead(PIN_CALL)) + ". Звонок — когда значение ниже порога.</small>";
  b += "<label>Пауза между уведомлениями о звонке, мс</label><input type='number' name='debounceMs' min='1000' max='60000' value='" + String(cfg.debounceMs) + "'>";
  b += "</details></div>";

  b += "<div class='card'><h2>Устройство</h2>";
  b += "<label>Имя в сети</label><input name='host' value='" + esc(cfg.host) + "' pattern='[a-z0-9-]{1,30}'>";
  b += "<small>Портал будет доступен по адресу http://<b>" + esc(cfg.host) + "</b>.local. Латиница, цифры и дефис.</small>";
  b += "<label>Часовой пояс (POSIX TZ)</label><input name='tz' value='" + esc(cfg.tz) + "'>";
  b += "<small>Для Испании: CET-1CEST,M3.5.0,M10.5.0/3</small>";
  b += "<label>Новый пароль портала и OTA</label><input type='password' name='pass1' autocomplete='new-password' placeholder='";
  b += cfg.webPass.length() ? "задан — оставьте пустым, чтобы не менять" : "не задан";
  b += "'><input type='password' name='pass2' autocomplete='new-password' placeholder='повторите пароль' style='margin-top:6px'>";
  b += "<small>Логин: <b>admin</b>. Минимум 8 символов. Этот же пароль ставится на точку доступа WiFi-настройки.</small>";
  if (cfg.webPass.length()) b += "<label class='chk'><input type='checkbox' name='passClear'>Убрать пароль</label>";
  b += "</div><button class='big'>💾 Сохранить</button></form>";
  return b;
}

void handleSettings() {
  if (!auth()) return;
  sendPage("/settings", "Настройки", settingsBody("", ""));
}

void startMdns() {
  MDNS.end();
  if (MDNS.begin(cfg.host.c_str())) MDNS.addService("http", "tcp", 80);
}

void applyOtaAuth() {
  if (cfg.webPass.length()) ElegantOTA.setAuth("admin", cfg.webPass.c_str());
  else ElegantOTA.clearAuth();
}

void handleSettingsSave() {
  if (!auth() || !csrfOk()) return;
  String errs;
  String oldHost = cfg.host, oldToken = cfg.token;
  bool oldTls = cfg.tlsVerify;

  String tok = server.arg("token"); tok.trim();
  if (tok.length()) { if (validToken(tok)) cfg.token = tok; else errs += "Токен не похож на токен бота. "; }

  String adm = server.arg("admin"); adm.trim();
  if (!adm.length() || validChatId(adm)) cfg.admin = adm; else errs += "chat_id администратора — только цифры. ";

  cfg.users = normalizeUsers(server.arg("users"), errs);
  cfg.notifyAll = server.hasArg("notifyAll");
  cfg.tlsVerify = server.hasArg("tlsVerify");
  cfg.confirmOpen = server.hasArg("confirmOpen");
  uint8_t oldType = cfg.type;
  bool oldMqttOn = cfg.mqttOn;
  String oldMqtt = cfg.mqttHost + ":" + cfg.mqttPort + ":" + cfg.mqttUser + ":" + cfg.mqttPass;
  cfg.mqttOn = server.hasArg("mqttOn");
  String mh = server.arg("mqttHost"); mh.trim();
  if (mh.length() < 64) cfg.mqttHost = mh;
  cfg.mqttPort = constrain(server.arg("mqttPort").toInt(), 1, 65535);
  String mu = server.arg("mqttUser"); mu.trim();
  if (mu.length() < 64) cfg.mqttUser = mu;
  String mp = server.arg("mqttPass");
  if (mp.length() && mp.length() < 64) cfg.mqttPass = mp;
  if (!mu.length()) cfg.mqttPass = "";
  if (cfg.mqttOn && !cfg.mqttHost.length()) errs += "MQTT: укажите адрес брокера. ";
  cfg.maxAgeS   = constrain(server.arg("maxAge").toInt(), 10, 3600);
  cfg.type      = server.arg("type").toInt() == INTERCOM_SIMPLE ? INTERCOM_SIMPLE : INTERCOM_FULL;
  cfg.doorMs    = constrain(server.arg("doorMs").toInt(), 100, 10000);
  cfg.camMs     = constrain(server.arg("camMs").toInt(), 100, 10000);
  cfg.threshold = constrain(server.arg("threshold").toInt(), 0, 1023);
  cfg.debounceMs = constrain(server.arg("debounceMs").toInt(), 1000, 60000);

  String host = server.arg("host"); host.trim(); host.toLowerCase();
  if (validHost(host)) cfg.host = host; else errs += "Имя в сети: только a-z, 0-9 и дефис. ";

  String tz = server.arg("tz"); tz.trim();
  if (tz.length() && tz.length() < 60) cfg.tz = tz;

  String p1 = server.arg("pass1"), p2 = server.arg("pass2");
  if (server.hasArg("passClear")) cfg.webPass = "";
  else if (p1.length()) {
    if (p1 != p2) errs += "Пароли не совпадают. ";
    else if (p1.length() < 8) errs += "Пароль короче 8 символов. ";
    else cfg.webPass = p1;
  }

  saveConfig();
  if (cfg.token != oldToken || cfg.tlsVerify != oldTls) applyTelegram();
  if (cfg.host != oldHost) { startMdns(); WiFi.setHostname(cfg.host.c_str()); }
  setenv("TZ", cfg.tz.c_str(), 1); tzset();
  applyOtaAuth();
  String newMqtt = cfg.mqttHost + ":" + cfg.mqttPort + ":" + cfg.mqttUser + ":" + cfg.mqttPass;
  if (newMqtt != oldMqtt || cfg.mqttOn != oldMqttOn || cfg.host != oldHost) mqttRestart();
  else if (cfg.type != oldType && mqtt.connected()) mqttDiscovery();
  addLog("⚙️ Настройки изменены");

  String msg = "Сохранено.";
  if (cfg.host != oldHost) msg += " Новый адрес: http://" + cfg.host + ".local";
  sendPage("/settings", "Настройки", settingsBody(msg, errs));
}

void guestsPage(const String &msg, bool bad) {
  rebuildGuests("");
  String b;
  if (msg.length()) b += "<div class='card" + String(bad ? " warn'><b class='err'>" : "'><b class='ok'>") + esc(msg) + "</b></div>";
  if (!nowEpoch()) b += "<div class='card warn'><b class='err'>Время не синхронизировано</b> — гостевой доступ не работает, пока устройство не получит время из интернета.</div>";
  b += "<div class='card'><h2>Гости с доступом</h2>";
  int n = 0; String t = "<table>";
  forEachGuest([&](const String &g, long e, const String &nm) {
    n++;
    t += "<tr><td>до " + fmtUntil(e) + "</td><td>" + esc(nm.length() ? nm : String("—")) + " <span class='mut'>" + esc(g) + "</span>";
    t += "<form method='post' action='/guests/del' style='margin-top:4px'>" + csrfField() + "<input type='hidden' name='id' value='" + esc(g) +
         "'><button class='bad' style='padding:4px 10px'>Удалить</button></form></td></tr>";
    return true;
  });
  b += n ? t + "</table>" : String("<p class='mut'>Нет</p>");
  b += "</div><div class='card'><h2>Добавить гостя</h2><form method='post' action='/guests'>" + csrfField();
  b += "<label>chat_id</label><input name='id' inputmode='numeric' required>";
  b += "<label>Имя</label><input name='name' maxlength='32' placeholder='Курьер'>";
  b += "<label>На сколько часов</label><input type='number' name='hours' min='1' max='720' value='" + String(REQ_GUEST_H) + "'>";
  b += "<small>Гость может только открывать дверь" + String(hasDisplay() ? " и включать экран" : "") +
       ". Когда гость открывает дверь, админу приходит сообщение. По истечении срока доступ пропадает сам.</small>";
  b += "<div class='row'><button>Добавить</button></div></form></div>";
  b += "<div class='card'><h2>Проще через Telegram</h2><p>Пусть гость напишет боту что угодно. Админу придёт запрос с кнопками "
       "<b>«Гость на " + String(REQ_GUEST_H) + " ч»</b> и <b>«Добавить»</b>. Или командой: <code>/guest &lt;chat_id&gt; [часов] [имя]</code>, "
       "убрать — <code>/deny &lt;chat_id&gt;</code>.</p></div>";
  sendPage("/guests", "Гости", b);
}

void handleGuests() { if (auth()) guestsPage("", false); }

void handleGuestAdd() {
  if (!auth() || !csrfOk()) return;
  String id = server.arg("id"); id.trim();
  int hours = server.arg("hours").toInt();
  if (!validChatId(id)) { guestsPage("chat_id — только цифры", true); return; }
  if (cfg.admin == id || isListedUser(id)) { guestsPage("Этот chat_id уже есть в постоянном списке", true); return; }
  if (!addGuest(id, hours, server.arg("name"))) { guestsPage("Время не синхронизировано, попробуйте позже", true); return; }
  addLog("🕐 Гость до " + fmtUntil(guestExpiry(id)) + ": " + userLabel(id) + " (веб-портал)");
  guestsPage("Добавлен до " + fmtUntil(guestExpiry(id)), false);
}

void handleGuestDel() {
  if (!auth() || !csrfOk()) return;
  String id = server.arg("id");
  String label = userLabel(id);
  rebuildGuests(id);
  addLog("➖ Гость удалён: " + label);
  redirect("/guests");
}

void handleWifi() {
  if (!auth()) return;
  String b = "<div class='card'><h2>Подключение</h2><table>";
  b += row("Сеть", esc(WiFi.SSID()));
  b += row("Сигнал", String(WiFi.RSSI()) + " dBm");
  b += row("IP", WiFi.localIP().toString());
  b += row("Шлюз", WiFi.gatewayIP().toString());
  b += row("MAC", WiFi.macAddress());
  b += "</table></div><div class='card'><h2>Сменить сеть</h2>";
  b += "<p>Устройство забудет текущую сеть, перезагрузится и создаст точку доступа <b>Intercom-Setup</b>";
  b += cfg.webPass.length() >= 8 ? " (пароль — как у портала)" : " (без пароля)";
  b += ". Подключитесь к ней с телефона — откроется страница выбора сети (или зайдите на 192.168.4.1). Если за 5 минут сеть не выбрана, устройство перезагрузится и снова включит точку доступа.</p>";
  b += "<form method='post' action='/wifi/reset' onsubmit=\"return confirm('Забыть текущую сеть WiFi?')\">" + csrfField() + "<button class='bad'>Забыть сеть и настроить заново</button></form></div>";
  sendPage("/wifi", "WiFi", b);
}

void handleWifiReset() {
  if (!auth() || !csrfOk()) return;
  sendPage("/wifi", "WiFi", "<div class='card'>Сеть сброшена. Подключитесь к точке доступа <b>Intercom-Setup</b>.</div>");
  delay(500);
  WiFiManager wm; wm.resetSettings();
  ESP.restart();
}

void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/open", HTTP_POST, handleOpen);
  server.on("/display", HTTP_POST, handleDisplay);
  server.on("/tgtest", HTTP_POST, handleTgTest);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/settings", HTTP_POST, handleSettingsSave);
  server.on("/guests", HTTP_GET, handleGuests);
  server.on("/guests", HTTP_POST, handleGuestAdd);
  server.on("/guests/del", HTTP_POST, handleGuestDel);
  server.on("/wifi", HTTP_GET, handleWifi);
  server.on("/wifi/reset", HTTP_POST, handleWifiReset);
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });

  ElegantOTA.begin(&server);
  applyOtaAuth();
  ElegantOTA.onStart([] { addLog("🔄 Обновление прошивки…"); });
  ElegantOTA.onProgress([](size_t, size_t) { esp_task_wdt_reset(); });
  ElegantOTA.onEnd([](bool ok) { addLog(ok ? "✅ Прошивка обновлена" : "❌ Ошибка обновления"); });
  server.begin();
}

// ---------- Setup / loop ----------
void setup() {
  // Outputs LOW before enabling them, so the relay never blinks on boot
  digitalWrite(PIN_DOOR, LOW);   pinMode(PIN_DOOR, OUTPUT);
  digitalWrite(PIN_CAMERA, LOW); pinMode(PIN_CAMERA, OUTPUT);
  pinMode(PIN_LED, OUTPUT); led(false);

  Serial.begin(115200);
  delay(300);
  Serial.println("\nIntercom " FW_VERSION " / " BOARD_NAME);

  prefs.begin("intercom", false);
  loadConfig();
  if (!prefs.getBool("init", false)) {
    migrateLegacy();
    saveConfig();
    prefs.putBool("init", true);
  }
  bootCount = prefs.getUInt("boots", bootCount) + 1;
  prefs.putUInt("boots", bootCount);

  analogReadResolution(10);
  for (int i = 0; i < 4; i++) { char h[9]; snprintf(h, sizeof h, "%08lx", (unsigned long)esp_random()); csrf += h; }

  // WiFi via WiFiManager (captive portal "Intercom-Setup" when no network is saved)
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.host.c_str());
  WiFiManager wm;
  wm.setTitle("Intercom");
  wm.setHostname(cfg.host.c_str());
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(300);
  const char *apPass = cfg.webPass.length() >= 8 ? cfg.webPass.c_str() : nullptr;
  if (!wm.autoConnect("Intercom-Setup", apPass)) { Serial.println("WiFi failed, restarting"); ESP.restart(); }
  WiFi.setAutoReconnect(true);
  Serial.println("WiFi: " + WiFi.localIP().toString());

  // Time (needed for TLS checks and command age)
  configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 50 && !timeValid(); i++) delay(100);

  startMdns();
  setupWeb();
  applyTelegram();
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);
  mqtt.setCallback(onMqtt);
  lastMqttTry = millis() - MQTT_RETRY_MS;

  esp_task_wdt_config_t wdt = { .timeout_ms = WDT_TIMEOUT_MS, .idle_core_mask = 0, .trigger_panic = true };
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  esp_task_wdt_add(NULL);

  addLog("🚀 Старт, загрузка №" + String(bootCount));
  Serial.println("Portal: http://" + cfg.host + ".local/");
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  ElegantOTA.loop();
  checkCall();
  mqttLoop();
  pollTelegram();

  if (Serial.available()) {
    String c = Serial.readStringUntil('\n'); c.trim();
    if (c == "open") { openDoor(); addLog("🚪 Открыто через Serial"); }
    else if (c == "status") Serial.println(statusText());
    else if (c == "restart") ESP.restart();
    else if (c == "resetwifi") { WiFiManager wm; wm.resetSettings(); ESP.restart(); }
    else Serial.println("Commands: open, status, restart, resetwifi");
  }
  delay(5);
}
