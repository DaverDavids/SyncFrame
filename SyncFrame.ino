#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <stdarg.h>
#include <esp_system.h>

#define DEBUG_SERIAL 1
#if DEBUG_SERIAL
  #define DBG_BEGIN(x) Serial.begin(x)
  #define DBG(...)     Serial.printf(__VA_ARGS__)
  #define DBGLN(x)     Serial.println(x)
#else
  #define DBG_BEGIN(x)
  #define DBG(...)
  #define DBGLN(x)
#endif

#if __has_include(<Secrets.h>)
  #include <Secrets.h>
#else
  const char* MYSSID = "";
  const char* MYPSK = "";
  const char* test_http_password = "";
#endif

#include "board_config.h"
#include "html.h"
#include "splash.h"

char HOSTNAME[32];
char MAC_STR[18];   // "AA:BB:CC:DD:EE:FF\0"
static const char* HOST_PREFIX = "syncframe-";

Preferences prefs;
struct Config {
  String photoBaseUrl;
  String photoFilename;
  bool   httpsInsecure;
  String httpUser;
  String httpPass;
  String mqttHost;
  uint16_t mqttPort;
  String mqttTopic;
  String mqttUser;
  String mqttPass;
  bool mqttUseTLS;
  bool mqttTlsInsecure;
} cfg;

static const char* PREF_NS = "syncframe";
static const char* DEFAULT_PHOTO_BASEURL = "https://192.168.6.202:8369/syncframe/";
static const char* DEFAULT_PHOTO_FILE    = "photo.800x480.jpg";
static const char* DEFAULT_HTTP_USER = "david";
static const char* DEFAULT_MQTT_USER = "david";
static const char* DEFAULT_MQTT_HOST  = "192.168.6.202";
static const uint16_t DEFAULT_MQTT_PORT = 8368;
static const char* DEFAULT_MQTT_TOPIC = "photos";
static const size_t MAX_JPG = 1200 * 1024;

WebServer server(80);
WiFiClient mqttNetPlain;
WiFiClientSecure mqttNetSecure;
PubSubClient mqtt;

bool networkServicesStarted = false;
unsigned long lastWifiAttemptMs = 0;
unsigned long lastMqttAttemptMs = 0;
unsigned long lastPortalStartMs = 0;

volatile bool mqttConnected = false;
volatile bool lastDownloadOk = false;
String lastDownloadErr = "";
unsigned long lastDownloadMs = 0;
unsigned long lastMqttMsgMs = 0;

uint8_t* currentJpg = nullptr;
size_t   currentJpgLen = 0;
uint8_t* lastJpg = nullptr;
size_t   lastJpgLen = 0;
bool showingLast = false;
bool wifiEverConnected = false;

static const uint8_t LOG_CAP = 48;
static const size_t LOG_MSG_LEN = 96;
struct LogEntry {
  uint32_t seq;
  uint32_t ms;
  char tag[8];
  char msg[LOG_MSG_LEN];
};
static LogEntry logBuf[LOG_CAP];
static uint8_t logHead = 0;
static uint8_t logCount = 0;
static uint32_t logSeq = 0;

// ---------------------- Hardware Callbacks ----------------------
bool hasLastPhoto() {
  return (lastJpg != nullptr && lastJpgLen > 0);
}

void showCurrentPhoto() {
  if (currentJpg && currentJpgLen) {
    showingLast = false;
    board_draw_jpeg(currentJpg, currentJpgLen);
  }
}

void showLastPhoto() {
  if (lastJpg && lastJpgLen) {
    showingLast = true;
    board_draw_jpeg(lastJpg, lastJpgLen);
  }
}

static bool downloadAndShowPhoto();
void triggerPhotoDownload() {
  downloadAndShowPhoto();
}

// ---------------------- Helpers ----------------------
static void buildHostAndClientId() {
  uint64_t fullMac = ESP.getEfuseMac();
  // ESP.getEfuseMac() returns bytes in little-endian order
  uint8_t m[6];
  m[0] = (fullMac >>  0) & 0xFF;
  m[1] = (fullMac >>  8) & 0xFF;
  m[2] = (fullMac >> 16) & 0xFF;
  m[3] = (fullMac >> 24) & 0xFF;
  m[4] = (fullMac >> 32) & 0xFF;
  m[5] = (fullMac >> 40) & 0xFF;
  snprintf(MAC_STR, sizeof(MAC_STR), "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
  uint32_t mac32 = (uint32_t)fullMac;
  uint16_t shortId = (mac32 >> 20) & 0x0FFF;
  snprintf(HOSTNAME, sizeof(HOSTNAME), "%s%03X", HOST_PREFIX, shortId);
}

static const char* resetReasonToStr(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:   return "unknown";
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "other";
  }
}

static void logEvent(const char* tag, const char* fmt, ...) {
  LogEntry& e = logBuf[logHead];
  e.seq = ++logSeq;
  e.ms = millis();
  strncpy(e.tag, tag ? tag : "LOG", sizeof(e.tag) - 1);
  e.tag[sizeof(e.tag) - 1] = 0;

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
  va_end(ap);

  logHead = (logHead + 1) % LOG_CAP;
  if (logCount < LOG_CAP) logCount++;
  DBG("[%s] %s\n", e.tag, e.msg);
}

static void appendJsonEscaped(String& out, const char* s) {
  if (!s) return;
  while (*s) {
    char c = *s++;
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:    out += c; break;
    }
  }
}

static void appendJsonEscaped(String& out, const String& s) {
  appendJsonEscaped(out, s.c_str());
}

static void freeBuf(uint8_t*& p, size_t& n) {
  if (p) {
    free(p);
    p = nullptr;
  }
  n = 0;
}

static void applyWifiDefaults() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(false);
}

static void loadConfig() {
  prefs.begin(PREF_NS, true);
  cfg.photoBaseUrl = prefs.getString("pbase", DEFAULT_PHOTO_BASEURL);
  cfg.photoFilename = prefs.getString("pfile", DEFAULT_PHOTO_FILE);
  cfg.httpsInsecure = prefs.getBool("pinsec", true);
  cfg.httpUser = prefs.getString("puser", DEFAULT_HTTP_USER);
  cfg.httpPass = prefs.getString("ppass", String(test_http_password));
  cfg.mqttHost = prefs.getString("mhost", DEFAULT_MQTT_HOST);
  cfg.mqttPort = prefs.getUShort("mport", DEFAULT_MQTT_PORT);
  cfg.mqttTopic = prefs.getString("mtopic", DEFAULT_MQTT_TOPIC);
  cfg.mqttUser = prefs.getString("muser", DEFAULT_MQTT_USER);
  cfg.mqttPass = prefs.getString("mpass", String(test_http_password));
  cfg.mqttUseTLS = prefs.getBool("mtls", true);
  cfg.mqttTlsInsecure = prefs.getBool("mtlsins", true);
  prefs.end();
}

static void saveConfig() {
  prefs.begin(PREF_NS, false);
  prefs.putString("pbase", cfg.photoBaseUrl);
  prefs.putString("pfile", cfg.photoFilename);
  prefs.putBool("pinsec", cfg.httpsInsecure);
  prefs.putString("puser", cfg.httpUser);
  prefs.putString("ppass", cfg.httpPass);
  prefs.putString("mhost", cfg.mqttHost);
  prefs.putUShort("mport", cfg.mqttPort);
  prefs.putString("mtopic", cfg.mqttTopic);
  prefs.putString("muser", cfg.mqttUser);
  prefs.putString("mpass", cfg.mqttPass);
  prefs.putBool("mtls", cfg.mqttUseTLS);
  prefs.putBool("mtlsins", cfg.mqttTlsInsecure);
  prefs.end();
}

static String makePhotoUrl() {
  String base = cfg.photoBaseUrl;
  if (!base.endsWith("/")) base += "/";
  return base + cfg.photoFilename;
}

static bool httpDownloadToBuffer(uint8_t** outBuf, size_t* outLen, String* outErr) {
  *outBuf = nullptr;
  *outLen = 0;
  if (outErr) *outErr = "";

  String url = makePhotoUrl();
  if (!url.startsWith("https://") && !url.startsWith("http://")) {
    if (outErr) *outErr = "URL must start with http:// or https://";
    logEvent("PHOTO", "bad url %s", url.c_str());
    return false;
  }

  logEvent("PHOTO", "GET %s", url.c_str());

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  HTTPClient http;

  secureClient.setTimeout(5);
  plainClient.setTimeout(5);
  http.setConnectTimeout(4000);
  http.setTimeout(5000);

  if (url.startsWith("https://")) {
    if (cfg.httpsInsecure) secureClient.setInsecure();
    if (!http.begin(secureClient, url)) {
      if (outErr) *outErr = "http.begin failed";
      logEvent("HTTP", "begin failed https");
      return false;
    }
  } else {
    if (!http.begin(plainClient, url)) {
      if (outErr) *outErr = "http.begin failed";
      logEvent("HTTP", "begin failed http");
      return false;
    }
  }

  if (cfg.httpUser.length() > 0) {
    http.setAuthorization(cfg.httpUser.c_str(), cfg.httpPass.c_str());
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    String err = (code < 0) ? http.errorToString(code) : ("HTTP " + String(code));
    if (outErr) *outErr = err;
    logEvent("HTTP", "GET failed %s", err.c_str());
    http.end();
    return false;
  }

  int64_t total = http.getSize();
  if (total > (int64_t)MAX_JPG) {
    if (outErr) *outErr = "image too large";
    logEvent("PHOTO", "too large %lld", (long long)total);
    http.end();
    return false;
  }

  size_t allocSize = (total > 0) ? (size_t)total : MAX_JPG;
  uint8_t* buf = (uint8_t*)ps_malloc(allocSize);
  if (!buf) buf = (uint8_t*)malloc(allocSize);
  if (!buf) {
    if (outErr) *outErr = "malloc failed";
    logEvent("PHOTO", "buffer alloc failed %u", (unsigned)allocSize);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t readTotal = 0;
  unsigned long lastProgressMs = millis();

  while ((http.connected() || stream->available()) && (millis() - lastProgressMs) < 5000) {
    size_t avail = stream->available();
    if (!avail) {
      delay(1);
      continue;
    }

    size_t toRead = avail;
    if (readTotal + toRead > allocSize) toRead = allocSize - readTotal;
    if (toRead == 0) {
      free(buf);
      if (outErr) *outErr = "image too large";
      logEvent("PHOTO", "buffer full at %u", (unsigned)readTotal);
      http.end();
      return false;
    }

    int n = stream->readBytes(buf + readTotal, toRead);
    if (n <= 0) break;
    readTotal += (size_t)n;
    lastProgressMs = millis();
  }
  http.end();

  if ((total > 0 && readTotal != (size_t)total) || readTotal < 16) {
    free(buf);
    if (outErr) *outErr = "short read";
    logEvent("PHOTO", "short read %u/%u", (unsigned)readTotal, (unsigned)((total > 0) ? total : 0));
    return false;
  }

  *outBuf = buf;
  *outLen = readTotal;
  logEvent("PHOTO", "download ok bytes=%u", (unsigned)readTotal);
  return true;
}

static bool downloadAndShowPhoto() {
  uint8_t* newBuf = nullptr;
  size_t newLen = 0;
  String err;

  if (WiFi.status() != WL_CONNECTED) {
    lastDownloadOk = false;
    lastDownloadErr = "no wifi";
    logEvent("PHOTO", "skipped no wifi");
    return false;
  }

  if (!currentJpg && !lastJpg) {
    board_draw_boot_status("Downloading Photo...");
  }

  bool ok = httpDownloadToBuffer(&newBuf, &newLen, &err);
  lastDownloadMs = millis();

  if (!ok) {
    lastDownloadOk = false;
    lastDownloadErr = err;
    logEvent("PHOTO", "download failed %s", err.c_str());

    if (currentJpg && currentJpgLen) {
      showCurrentPhoto();
    } else if (lastJpg && lastJpgLen) {
      showLastPhoto();
    } else {
      board_draw_boot_status((String("Download failed: ") + err).c_str());
    }
    return false;
  }

  freeBuf(lastJpg, lastJpgLen);
  lastJpg = currentJpg;
  lastJpgLen = currentJpgLen;
  currentJpg = newBuf;
  currentJpgLen = newLen;

  lastDownloadOk = true;
  lastDownloadErr = "";
  showingLast = false;

  board_draw_jpeg(currentJpg, currentJpgLen);
  logEvent("PHOTO", "showing new photo bytes=%u", (unsigned)currentJpgLen);
  return true;
}

// ---------------------- MQTT ----------------------
static void mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
  (void)payload;
  lastMqttMsgMs = millis();
  logEvent("MQTT", "message topic=%s bytes=%u", topic ? topic : "", length);
  downloadAndShowPhoto();
}

static void mqttSetupClient() {
  mqtt.setCallback(mqttCallback);
  if (cfg.mqttUseTLS) {
    if (cfg.mqttTlsInsecure) mqttNetSecure.setInsecure();
    mqtt.setClient(mqttNetSecure);
  } else {
    mqtt.setClient(mqttNetPlain);
  }
  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setSocketTimeout(1);
}

static void mqttMaybeReconnect() {
  if (mqtt.connected()) {
    mqttConnected = true;
    return;
  }

  mqttConnected = false;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttemptMs < 15000) return;
  lastMqttAttemptMs = millis();
  if (cfg.mqttHost.length() == 0) return;

  mqttNetPlain.setTimeout(1);
  mqttNetSecure.setTimeout(1);
  logEvent("MQTT", "connect %s:%u tls=%s", cfg.mqttHost.c_str(), cfg.mqttPort, cfg.mqttUseTLS ? "yes" : "no");

  bool ok;
  if (cfg.mqttUser.length()) {
    ok = mqtt.connect(HOSTNAME, cfg.mqttUser.c_str(), cfg.mqttPass.c_str());
  } else {
    ok = mqtt.connect(HOSTNAME);
  }

  if (ok) {
    mqttConnected = true;
    bool subOk = mqtt.subscribe(cfg.mqttTopic.c_str());
    logEvent("MQTT", "connected topic=%s sub=%s", cfg.mqttTopic.c_str(), subOk ? "ok" : "fail");
  } else {
    logEvent("MQTT", "connect failed state=%d", mqtt.state());
  }
}

// ---------------------- WiFi / Network ----------------------
static void startNetworkServicesOnce() {
  if (networkServicesStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  bool mdnsOk = MDNS.begin(HOSTNAME);
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.begin();

  networkServicesStarted = true;
  wifiEverConnected = true;
  logEvent("WIFI", "connected ip=%s mac=%s", WiFi.localIP().toString().c_str(), MAC_STR);
  logEvent("NET", "services mdns=%s ota=on", mdnsOk ? "on" : "off");
}

static void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    startNetworkServicesOnce();
    return;
  }
  if (lastWifiAttemptMs != 0 && (millis() - lastWifiAttemptMs) < 5000) return;
  lastWifiAttemptMs = millis();

  applyWifiDefaults();

  if (wifiEverConnected) {
    logEvent("WIFI", "reconnect attempt");
    WiFi.reconnect();
    return;
  }

  if (MYSSID && strlen(MYSSID)) {
    logEvent("WIFI", "connect begin ssid=%s", MYSSID);
    WiFi.begin(MYSSID, MYPSK);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      startNetworkServicesOnce();
      return;
    }
    logEvent("WIFI", "saved connect timed out status=%d", WiFi.status());
  } else {
    logEvent("WIFI", "no compiled credentials");
  }

  if (lastPortalStartMs != 0 && (millis() - lastPortalStartMs) < 60000) return;
  lastPortalStartMs = millis();

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(15);
  wm.setConnectRetries(3);
  String ap = String(HOSTNAME) + "-setup";
  logEvent("WIFI", "setup portal %s", ap.c_str());
  bool ok = wm.autoConnect(ap.c_str());
  if (ok && WiFi.status() == WL_CONNECTED) {
    logEvent("WIFI", "setup portal connected");
    startNetworkServicesOnce();
  } else {
    logEvent("WIFI", "setup portal timeout");
  }
}

// ---------------------- Web UI ----------------------
static void handleRoot() {
  server.send(200, "text/html; charset=utf-8", FPSTR(INDEX_HTML));
}

static void handleConfigPage() {
  server.send(200, "text/html; charset=utf-8", FPSTR(CONFIG_HTML));
}

static void handleStatusJson() {
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  String j;
  j.reserve(420);
  j += "{";
  j += "\"hostname\":\"";
  appendJsonEscaped(j, HOSTNAME);
  j += "\",";
  j += "\"mac\":\"";
  appendJsonEscaped(j, MAC_STR);
  j += "\",";
  j += "\"wifi\":";
  j += (wifiOk ? "true" : "false");
  j += ",\"wifiEverConnected\":";
  j += (wifiEverConnected ? "true" : "false");
  j += ",\"ip\":\"";
  appendJsonEscaped(j, wifiOk ? WiFi.localIP().toString() : String(""));
  j += "\",";
  j += "\"mdns\":";
  j += (networkServicesStarted ? "true" : "false");
  j += ",\"ota\":";
  j += (networkServicesStarted ? "true" : "false");
  j += ",\"mqtt\":";
  j += (mqttConnected ? "true" : "false");
  j += ",\"lastDownloadOk\":";
  j += (lastDownloadOk ? "true" : "false");
  j += ",\"lastDownloadErr\":\"";
  appendJsonEscaped(j, lastDownloadErr);
  j += "\",";
  j += "\"lastDownloadMs\":";
  j += String(lastDownloadMs);
  j += ",\"lastMqttMsgMs\":";
  j += String(lastMqttMsgMs);
  j += ",\"lastLogSeq\":";
  j += String(logSeq);
  j += ",\"screenW\":";
  j += String(SCREEN_W);
  j += ",\"screenH\":";
  j += String(SCREEN_H);
  j += "}";
  server.send(200, "application/json", j);
}

static void handleLogJson() {
  uint32_t since = 0;
  if (server.hasArg("since")) {
    since = strtoul(server.arg("since").c_str(), nullptr, 10);
  }

  const uint8_t maxItems = 20;
  uint8_t start = (logHead + LOG_CAP - logCount) % LOG_CAP;
  uint8_t skip = 0;
  if (since == 0 && logCount > maxItems) skip = logCount - maxItems;

  String j;
  j.reserve(2048);
  j += "{\"items\":[";

  bool first = true;
  uint8_t sent = 0;
  for (uint8_t i = 0; i < logCount; i++) {
    uint8_t idx = (start + i) % LOG_CAP;
    const LogEntry& e = logBuf[idx];
    if (skip) {
      skip--;
      continue;
    }
    if (e.seq <= since) continue;
    if (sent >= maxItems) break;

    if (!first) j += ',';
    first = false;
    j += "{\"seq\":";
    j += String(e.seq);
    j += ",\"ms\":";
    j += String(e.ms);
    j += ",\"tag\":\"";
    appendJsonEscaped(j, e.tag);
    j += "\",\"msg\":\"";
    appendJsonEscaped(j, e.msg);
    j += "\"}";
    sent++;
  }

  j += "],\"nextSince\":";
  j += String(logSeq);
  j += "}";
  server.send(200, "application/json", j);
}

static void handleGetConfigJson() {
  String j;
  j.reserve(384);
  j += "{";
  j += "\"photoBaseUrl\":\"";
  appendJsonEscaped(j, cfg.photoBaseUrl);
  j += "\",\"photoFilename\":\"";
  appendJsonEscaped(j, cfg.photoFilename);
  j += "\",\"httpsInsecure\":";
  j += (cfg.httpsInsecure ? "true" : "false");
  j += ",\"httpUser\":\"";
  appendJsonEscaped(j, cfg.httpUser);
  j += "\",\"httpPass\":\"";
  j += (cfg.httpPass.length() ? "********" : "");
  j += "\",\"mqttHost\":\"";
  appendJsonEscaped(j, cfg.mqttHost);
  j += "\",\"mqttPort\":";
  j += String(cfg.mqttPort);
  j += ",\"mqttTopic\":\"";
  appendJsonEscaped(j, cfg.mqttTopic);
  j += "\",\"mqttUser\":\"";
  appendJsonEscaped(j, cfg.mqttUser);
  j += "\",\"mqttPass\":\"";
  j += (cfg.mqttPass.length() ? "********" : "");
  j += "\",\"mqttUseTLS\":";
  j += (cfg.mqttUseTLS ? "true" : "false");
  j += ",\"mqttTlsInsecure\":";
  j += (cfg.mqttTlsInsecure ? "true" : "false");
  j += "}";
  server.send(200, "application/json", j);
}

// Helper: returns true if the arg is present, non-empty, and is NOT the masked sentinel
static bool isRealPassword(const String& val) {
  return val.length() > 0 && val != "********";
}

static void handlePostConfig() {
  if (server.hasArg("photoBaseUrl")) cfg.photoBaseUrl = server.arg("photoBaseUrl");
  if (server.hasArg("photoFilename")) cfg.photoFilename = server.arg("photoFilename");
  if (server.hasArg("httpUser")) cfg.httpUser = server.arg("httpUser");
  if (server.hasArg("mqttHost")) cfg.mqttHost = server.arg("mqttHost");
  if (server.hasArg("mqttTopic")) cfg.mqttTopic = server.arg("mqttTopic");
  if (server.hasArg("mqttUser")) cfg.mqttUser = server.arg("mqttUser");
  if (server.hasArg("mqttPort")) cfg.mqttPort = (uint16_t)server.arg("mqttPort").toInt();
  // Only update passwords when a real new value was explicitly submitted
  if (server.hasArg("httpPass") && isRealPassword(server.arg("httpPass"))) {
    cfg.httpPass = server.arg("httpPass");
  }
  if (server.hasArg("mqttPass") && isRealPassword(server.arg("mqttPass"))) {
    cfg.mqttPass = server.arg("mqttPass");
  }
  cfg.httpsInsecure = server.hasArg("httpsInsecure");
  cfg.mqttUseTLS = server.hasArg("mqttUseTLS");
  cfg.mqttTlsInsecure = server.hasArg("mqttTlsInsecure");
  saveConfig();
  mqtt.disconnect();
  mqttSetupClient();
  lastMqttAttemptMs = 0;
  logEvent("WEB", "config saved");
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleImgCurrent() {
  if (!currentJpg || !currentJpgLen) {
    server.send(404, "text/plain", "no image");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", (const char*)currentJpg, currentJpgLen);
}

static void handleImgLast() {
  if (!lastJpg || !lastJpgLen) {
    server.send(404, "text/plain", "no last image");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", (const char*)lastJpg, lastJpgLen);
}

static void handleActionRefresh() {
  logEvent("WEB", "manual refresh");
  bool ok = downloadAndShowPhoto();
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfigPage);
  server.on("/api/status", HTTP_GET, handleStatusJson);
  server.on("/api/log", HTTP_GET, handleLogJson);
  server.on("/api/config", HTTP_GET, handleGetConfigJson);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/refresh", HTTP_POST, handleActionRefresh);
  server.on("/img/current", HTTP_GET, handleImgCurrent);
  server.on("/img/last", HTTP_GET, handleImgLast);
  server.begin();
  logEvent("WEB", "server started");
}

// ---------------------- Main ----------------------
void setup() {
  DBG_BEGIN(115200);
  delay(50);
  buildHostAndClientId();
  logEvent("BOOT", "%s mac=%s reset=%s", HOSTNAME, MAC_STR, resetReasonToStr(esp_reset_reason()));

  loadConfig();
  board_init();

  board_draw_jpeg(splash_logo, splash_logo_len);
  board_draw_boot_status("Connecting to Wi-Fi...");

  applyWifiDefaults();
  ensureWifi();

  if (WiFi.status() == WL_CONNECTED) {
    // Show IP on the left, MAC on the right of the same line
    String ipMac = "IP: " + WiFi.localIP().toString() + "   MAC: " + String(MAC_STR);
    board_draw_boot_status(ipMac.c_str());
    delay(800);
  }

  setupWeb();

  board_draw_boot_status("Connecting to MQTT...");
  mqttNetPlain.setTimeout(1);
  mqttNetSecure.setTimeout(1);
  mqttSetupClient();
  mqttMaybeReconnect();
  mqtt.setSocketTimeout(1);

  downloadAndShowPhoto();
}

void loop() {
  ensureWifi();

  if (WiFi.status() == WL_CONNECTED) {
    if (networkServicesStarted) ArduinoOTA.handle();
    server.handleClient();
    mqttMaybeReconnect();
    if (mqtt.connected()) mqtt.loop();
  }

  board_loop();
  delay(10);
}
