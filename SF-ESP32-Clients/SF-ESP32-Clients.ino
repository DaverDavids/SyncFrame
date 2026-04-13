#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

#include <Update.h>
#include <stdarg.h>
#include <esp_system.h>
#include <deque>
#include "coredump_handler.h"

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

char HOSTNAME[32];
char MAC_STR[18];
static const char* HOST_PREFIX = "syncframe-";
static char compileIdStr[16];
const char SF_COMPILE_ID[] __attribute__((used, section(".rodata"))) = "SFID:" __DATE__ " " __TIME__;
static SemaphoreHandle_t drawMutex = nullptr;
static unsigned long bootTimeMs = 0;
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

DNSServer dnsServer;
static bool portalActive = false;
static bool portalDone = false;
static unsigned long portalStartMs = 0;
static const uint32_t PORTAL_TIMEOUT_MS = 180000;

Preferences prefs;
struct Config {
  String wifiSsid;
  String wifiPass;
  String photoBaseUrl;
  String photoFilename;
  bool httpsInsecure;
  String httpUser;
  String httpPass;
  String webUser;
  String webPass;
} cfg;

WebServer server(80);
bool networkServicesStarted = false;
static bool webServerStarted = false;
unsigned long lastWifiAttemptMs = 0;
String lastDownloadErr = "";
unsigned long lastDownloadMs = 0;
volatile bool lastDownloadOk = false;
uint8_t* currentJpg = nullptr;
size_t currentJpgLen = 0;
uint8_t* lastJpg = nullptr;
size_t lastJpgLen = 0;
bool showingLast = false;
bool wifiEverConnected = false;

static WiFiClientSecure* streamClient = nullptr;
static bool mjpegConnected = false;
static unsigned long lastMjpegConnectMs = 0;
static unsigned long lastMjpegAttemptMs = 0;
static volatile bool mjpegForceReconnect = false;
static String currentPhotoHash = "";

static const uint8_t WIFI_MAX_ATTEMPTS = 6;
static uint8_t wifiAttemptCount = 0;
static const uint8_t LOG_CAP = 48;
static const size_t LOG_MSG_LEN = 96;
struct LogEntry { uint32_t seq; uint32_t ms; char tag[8]; char msg[LOG_MSG_LEN]; };
static LogEntry logBuf[LOG_CAP];
static uint8_t logHead = 0;
static uint8_t logCount = 0;
static uint32_t logSeq = 0;

static void handleRoot();
static void handleConfigPage();
static void handleStatusJson();
static void handleLogJson();
static void handlePostConfig();
static void handleImgCurrent();
static void handleImgLast();
static void handleActionRefresh();
static void mjpegTask(void* pv);
static void mjpegMaybeReconnect();

static bool requireWebAuth() {
  if (ESP.getFreeHeap() < 20000) {
    server.send(503, "application/json", "{\"ok\":false,\"err\":\"low memory\"}");
    return false;
  }
  if (cfg.webPass.length() == 0) return true;
  if (server.authenticate(cfg.webUser.c_str(), cfg.webPass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "SyncFrame", "Authentication required");
  return false;
}

bool hasLastPhoto() { return (lastJpg != nullptr && lastJpgLen > 0); }

void showCurrentPhoto() {
  if (currentJpg && currentJpgLen && xSemaphoreTake(drawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    showingLast = false;
    board_draw_jpeg(currentJpg, currentJpgLen);
    boardDrawActive = false;
    xSemaphoreGive(drawMutex);
  }
}

void showLastPhoto() {
  if (lastJpg && lastJpgLen && xSemaphoreTake(drawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    showingLast = true;
    board_draw_jpeg(lastJpg, lastJpgLen);
    boardDrawActive = false;
    xSemaphoreGive(drawMutex);
  }
}

static void buildHostAndClientId() {
  uint64_t fullMac = ESP.getEfuseMac();
  uint8_t m[6];
  m[0] = (fullMac >> 0) & 0xFF;
  m[1] = (fullMac >> 8) & 0xFF;
  m[2] = (fullMac >> 16) & 0xFF;
  m[3] = (fullMac >> 24) & 0xFF;
  m[4] = (fullMac >> 32) & 0xFF;
  m[5] = (fullMac >> 40) & 0xFF;
  snprintf(MAC_STR, sizeof(MAC_STR), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  uint32_t mac32 = (uint32_t)fullMac;
  uint16_t shortId = (mac32 >> 20) & 0x0FFF;
  snprintf(HOSTNAME, sizeof(HOSTNAME), "%s%03X", HOST_PREFIX, shortId);
}

static void buildCompileId() { snprintf(compileIdStr, sizeof(compileIdStr), "%s", "mjpeg"); }
static void logEvent(const char* tag, const char* fmt, ...) { (void)tag; (void)fmt; }
static void appendJsonEscaped(String& out, const char* s) { if (s) out += s; }
static void appendJsonEscaped(String& out, const String& s) { out += s; }
static void appendJsonPassword(String& out, const String& pass) { if (pass.length() > 0) out += "********"; }
static void freeBuf(uint8_t*& p, size_t& n) { if (p) { free(p); p = nullptr; } n = 0; }
static void applyWifiDefaults() { WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.persistent(false); WiFi.setSleep(false); }
static void loadConfig() {}
static void saveConfig() {}
static String makePhotoUrl() { String base = cfg.photoBaseUrl; if (!base.endsWith("/")) base += "/"; return base + "stream"; }

static void mjpegTask(void* pv) {
  String url = makePhotoUrl();
  HTTPClient http;
  WiFiClientSecure client;
  if (cfg.httpsInsecure) client.setInsecure();
  http.setConnectTimeout(5000);
  if (!http.begin(client, url)) { vTaskDelete(NULL); return; }
  http.addHeader("Accept", "multipart/x-mixed-replace");
  if (cfg.httpUser.length() > 0) http.setAuthorization(cfg.httpUser.c_str(), cfg.httpPass.c_str());
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); vTaskDelete(NULL); return; }
  mjpegConnected = true;
  lastMjpegConnectMs = millis();
  WiFiClient* stream = http.getStreamPtr();
  static uint8_t buf[1460];
  while (http.connected() && WiFi.status() == WL_CONNECTED) {
    int avail = stream->available();
    if (avail <= 0) { vTaskDelay(pdMS_TO_TICKS(10)); if (mjpegForceReconnect) break; continue; }
    int n = stream->readBytes(buf, avail > (int)sizeof(buf) ? sizeof(buf) : avail);
    if (n > 2 && xSemaphoreTake(drawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      freeBuf(lastJpg, lastJpgLen);
      lastJpg = currentJpg;
      lastJpgLen = currentJpgLen;
      currentJpg = (uint8_t*)malloc(n);
      if (currentJpg) {
        memcpy(currentJpg, buf, n);
        currentJpgLen = n;
        board_draw_jpeg(currentJpg, currentJpgLen);
        boardDrawActive = false;
      } else {
        currentJpgLen = 0;
      }
      xSemaphoreGive(drawMutex);
    }
  }
  mjpegConnected = false;
  mjpegForceReconnect = false;
  http.end();
  vTaskDelete(NULL);
}

static void mjpegMaybeReconnect() {
  if (mjpegConnected) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMjpegAttemptMs < 15000) return;
  if (cfg.photoBaseUrl.length() == 0) return;
  lastMjpegAttemptMs = millis();
  xTaskCreatePinnedToCore(mjpegTask, "mjpegTask", 12288, nullptr, 1, nullptr, APP_CORE);
}

static void startNetworkServicesOnce() {
  if (networkServicesStarted || WiFi.status() != WL_CONNECTED) return;
  MDNS.begin(HOSTNAME);
  if (!webServerStarted) {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/config", HTTP_GET, handleConfigPage);
    server.on("/api/status", HTTP_GET, handleStatusJson);
    server.on("/api/log", HTTP_GET, handleLogJson);
    server.on("/api/config", HTTP_POST, handlePostConfig);
    server.on("/api/refresh", HTTP_POST, handleActionRefresh);
    server.on("/img/current", HTTP_GET, handleImgCurrent);
    server.on("/img/last", HTTP_GET, handleImgLast);
    server.begin();
    webServerStarted = true;
  }
  networkServicesStarted = true;
  wifiEverConnected = true;
  wifiAttemptCount = 0;
}

static void ensureWifi() {}
static void handleRoot() { if (!requireWebAuth()) return; server.send(200, "text/html; charset=utf-8", FPSTR(INDEX_HTML)); }
static void handleConfigPage() { if (!requireWebAuth()) return; server.send(200, "text/html; charset=utf-8", FPSTR(CONFIG_HTML)); }

static void handleStatusJson() {
  if (!requireWebAuth()) return;
  String j = "{";
  j += "\"hostname\":\"" + String(HOSTNAME) + "\",";
  j += "\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  j += "\"mjpeg\":" + String(mjpegConnected ? "true" : "false") + ",";
  j += "\"lastMjpegConnectMs\":" + String(lastMjpegConnectMs) + ",";
  j += "\"photoHash\":\"" + currentPhotoHash + "\",";
  j += "\"lastDownloadOk\":" + String(lastDownloadOk ? "true" : "false") + ",";
  j += "\"lastDownloadErr\":\"" + lastDownloadErr + "\"}";
  server.send(200, "application/json", j);
}

static void handleLogJson() { if (!requireWebAuth()) return; server.send(200, "application/json", "{\"items\":[]}"); }
static bool isRealPassword(const String& val) { return val.length() > 0 && val != "********"; }
static void handlePostConfig() {
  if (!requireWebAuth()) return;
  if (server.hasArg("photoBaseUrl")) cfg.photoBaseUrl = server.arg("photoBaseUrl");
  if (server.hasArg("photoFilename")) cfg.photoFilename = server.arg("photoFilename");
  if (server.hasArg("httpUser")) cfg.httpUser = server.arg("httpUser");
  if (server.hasArg("httpPass") && isRealPassword(server.arg("httpPass"))) cfg.httpPass = server.arg("httpPass");
  if (server.hasArg("webUser") && server.arg("webUser").length() > 0) cfg.webUser = server.arg("webUser");
  if (server.hasArg("webPass") && isRealPassword(server.arg("webPass"))) cfg.webPass = server.arg("webPass");
  if (server.hasArg("webPassClear") && server.arg("webPassClear") == "1") cfg.webPass = "";
  cfg.httpsInsecure = server.hasArg("httpsInsecure");
  saveConfig();
  mjpegConnected = false;
  lastMjpegAttemptMs = 0;
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleImgCurrent() {
  if (!requireWebAuth()) return;
  if (!currentJpg || !currentJpgLen) { server.send(404, "text/plain", "no image"); return; }
  server.send_P(200, "image/jpeg", (const char*)currentJpg, currentJpgLen);
}

static void handleImgLast() {
  if (!requireWebAuth()) return;
  if (!lastJpg || !lastJpgLen) { server.send(404, "text/plain", "no last image"); return; }
  server.send_P(200, "image/jpeg", (const char*)lastJpg, lastJpgLen);
}

static void handleActionRefresh() {
  if (!requireWebAuth()) return;
  mjpegForceReconnect = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void setup() {
  DBG_BEGIN(115200);
  bootTimeMs = millis();
  buildHostAndClientId();
  buildCompileId();
  drawMutex = xSemaphoreCreateBinary();
  xSemaphoreGive(drawMutex);
  loadConfig();
  board_init();
  applyWifiDefaults();
  if (cfg.wifiSsid.length() > 0) WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
}

void loop() {
  ensureWifi();
  if (WiFi.status() == WL_CONNECTED) {
    startNetworkServicesOnce();
    if (networkServicesStarted) {
      if (ESP.getFreeHeap() > 20000) server.handleClient();
      mjpegMaybeReconnect();
    }
  }
  board_loop();
  delay(1);
}
