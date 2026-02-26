#include <Arduino.h>

#define DEBUG_SERIAL 0
#if DEBUG_SERIAL
  #define DBG_BEGIN(x) Serial.begin(x)
  #define DBG(...)     Serial.printf(__VA_ARGS__)
  #define DBGLN(x)     Serial.println(x)
#else
  #define DBG_BEGIN(x)
  #define DBG(...)
  #define DBGLN(x)
#endif

#include <Secrets.h>

// You wanted HOSTNAME here (not in Secrets.h)
char HOSTNAME[32];
static const char* HOST_PREFIX = "sf8048-";  // adjust if you like

#include "html.h"

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

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <TAMC_GT911.h>
#include <JPEGDEC.h>

static JPEGDEC jpeg;

static void buildHostAndClientId() {
  // 3 hex chars from efuse MAC (lower 12 bits)
  uint32_t mac = (uint32_t)ESP.getEfuseMac();
  uint16_t shortId = (mac >> 20) & 0x0FFF;  // 12 bits -> up to FFF
  //uint32_t shortId = mac;  // 12 bits -> up to FFF

  // Build "sf8048-XYZ"
  snprintf(HOSTNAME, sizeof(HOSTNAME), "%s%03X", HOST_PREFIX, shortId);
}

// ---------------------- Display / Touch ----------------------
static const int SCREEN_W = 800;
static const int SCREEN_H = 480;

#define GFX_BL 2   // common BL pin on this board

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40, 41, 39, 42,         // DE, VSYNC, HSYNC, PCLK
  45, 48, 47, 21, 14,     // R0-R4
  5,  6,  7,  15, 16, 4,  // G0-G5
  8,  3,  46, 9,  1,      // B0-B4
  0, 8, 4, 24,             // HSYNC timing
  0, 8, 4, 16,             // VSYNC timing
  1, 15400000,
  false, 0, 0, 800*10
);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel);

#define TOUCH_SDA 19
#define TOUCH_SCL 20
#define TOUCH_INT 18
#define TOUCH_RST 38
TAMC_GT911 ts(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, SCREEN_W, SCREEN_H);

// ---------------------- Config stored in flash ----------------------
Preferences prefs;

struct Config {
  String photoBaseUrl;      // e.g. "https://192.168.1.10:9369/syncframe/"
  String photoFilename;     // "photo.800x480.jpg"

  bool   httpsInsecure;     // true => setInsecure() for self-signed certs

  String httpUser;          // Basic Auth user (required in your case)
  String httpPass;          // Basic Auth pass

  String mqttHost;
  uint16_t mqttPort;
  String mqttTopic;
  String mqttUser;
  String mqttPass;
  bool mqttUseTLS;
  bool mqttTlsInsecure; 
} cfg;

static const char* PREF_NS = "syncframe";

// Defaults (edit to match your environment)
static const char* DEFAULT_PHOTO_BASEURL = "https://192.168.6.202:8369/syncframe/";
static const char* DEFAULT_PHOTO_FILE    = "photo.800x480.jpg";

static const char* DEFAULT_HTTP_USER = "david";
static const char* DEFAULT_HTTP_PASS = test_http_password;

static const char* DEFAULT_MQTT_USER = "david";
static const char* DEFAULT_MQTT_PASS = test_http_password;

static const char* DEFAULT_MQTT_HOST  = "192.168.6.202";
static const uint16_t DEFAULT_MQTT_PORT = 8368;
static const char* DEFAULT_MQTT_TOPIC = "photos";

// ---------------------- Runtime status ----------------------
WebServer server(80);

WiFiClient mqttNetPlain;
WiFiClientSecure mqttNetSecure;
PubSubClient mqtt;  // Don't initialize with client yet

bool networkServicesStarted = false;

unsigned long lastWifiAttemptMs = 0;
unsigned long lastMqttAttemptMs = 0;

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

static int JPEGDraw(JPEGDRAW *pDraw) {
  gfx->draw16bitRGBBitmap(
    pDraw->x, pDraw->y,
    (uint16_t*)pDraw->pPixels,
    pDraw->iWidth, pDraw->iHeight
  );
  return 1;
}

// ---------------------- Helpers ----------------------
static String jsonEscape(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i=0;i<s.length();i++) {
    char c = s[i];
    if (c == '\\' || c == '\"') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else o += c;
  }
  return o;
}

static void freeBuf(uint8_t*& p, size_t& n) {
  if (p) { free(p); p = nullptr; }
  n = 0;
}

static bool touchPressed() {
  ts.read();
  return ts.isTouched;
}

static void loadConfig() {
  prefs.begin(PREF_NS, true);
  cfg.photoBaseUrl = prefs.getString("pbase", DEFAULT_PHOTO_BASEURL);
  cfg.photoFilename = prefs.getString("pfile", DEFAULT_PHOTO_FILE);
  cfg.httpsInsecure = prefs.getBool("pinsec", true);
  cfg.httpUser = prefs.getString("puser", DEFAULT_HTTP_USER);
  cfg.httpPass = prefs.getString("ppass", DEFAULT_HTTP_PASS);
  cfg.mqttHost = prefs.getString("mhost", DEFAULT_MQTT_HOST);
  cfg.mqttPort = prefs.getUShort("mport", DEFAULT_MQTT_PORT);
  cfg.mqttTopic = prefs.getString("mtopic", DEFAULT_MQTT_TOPIC);
  cfg.mqttUser = prefs.getString("muser", DEFAULT_MQTT_USER);
  cfg.mqttPass = prefs.getString("mpass", DEFAULT_MQTT_PASS);
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

void drawJpegFullScreen(const uint8_t* jpg, size_t len) {
  if (!jpg || !len) return;

  gfx->fillScreen(gfx->color565(0, 0, 0));

  if (jpeg.openRAM((uint8_t*)jpg, (int)len, JPEGDraw)) {
    int w = jpeg.getWidth();
    int h = jpeg.getHeight();
    DBG("JPEG size: %d x %d\n", w, h);

    int x = (SCREEN_W - w) / 2;
    int y = (SCREEN_H - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    jpeg.decode(x, y, 0);
    jpeg.close();
  }
}

static bool httpDownloadToBuffer(uint8_t** outBuf, size_t* outLen, String* outErr) {
  *outBuf = nullptr;
  *outLen = 0;
  if (outErr) *outErr = "";

  String url = makePhotoUrl();
  if (!url.startsWith("https://")) {
    if (outErr) *outErr = "URL must be https://";
    return false;
  }

  WiFiClientSecure client;
  if (cfg.httpsInsecure) client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    if (outErr) *outErr = "http.begin failed";
    return false;
  }

  http.setAuthorization(cfg.httpUser.c_str(), cfg.httpPass.c_str());

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (outErr) *outErr = "HTTP " + String(code);
    http.end();
    return false;
  }

  int64_t total = http.getSize();
  const size_t MAX_JPG = 1200 * 1024;
  size_t allocSize = (total > 0 && total < (int64_t)MAX_JPG) ? (size_t)total : MAX_JPG;

  uint8_t* buf = (uint8_t*)ps_malloc(allocSize);
  if (!buf) buf = (uint8_t*)malloc(allocSize);
  if (!buf) {
    if (outErr) *outErr = "malloc failed";
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t readTotal = 0;
  unsigned long start = millis();
  while (http.connected() && (millis() - start) < 15000) {
    size_t avail = stream->available();
    if (!avail) { delay(5); continue; }
    size_t toRead = avail;
    if (readTotal + toRead > allocSize) toRead = allocSize - readTotal;
    if (toRead == 0) break;
    int n = stream->readBytes(buf + readTotal, toRead);
    if (n <= 0) break;
    readTotal += (size_t)n;
  }

  http.end();

  if (readTotal < 16) {
    free(buf);
    if (outErr) *outErr = "short read";
    return false;
  }

  if (readTotal == allocSize && total > (int64_t)allocSize) {
    free(buf);
    if (outErr) *outErr = "image too large (cap hit)";
    return false;
  }

  *outBuf = buf;
  *outLen = readTotal;
  return true;
}

static bool downloadAndShowPhoto() {
  uint8_t* newBuf = nullptr;
  size_t newLen = 0;
  String err;
  
  if (WiFi.status() != WL_CONNECTED) {
    lastDownloadOk = false;
    lastDownloadErr = "no wifi";
    return false;
  }

  bool ok = httpDownloadToBuffer(&newBuf, &newLen, &err);
  lastDownloadMs = millis();

  if (!ok) {
    lastDownloadOk = false;
    lastDownloadErr = err;
    DBG("Download failed: %s\n", err.c_str());
    return false;
  }

  // Promote current -> last, new -> current
  freeBuf(lastJpg, lastJpgLen);
  lastJpg = currentJpg; lastJpgLen = currentJpgLen;
  currentJpg = newBuf; currentJpgLen = newLen;

  lastDownloadOk = true;
  lastDownloadErr = "";

  showingLast = false;
  drawJpegFullScreen(currentJpg, currentJpgLen);
  return true;
}

// ---------------------- MQTT ----------------------
static void mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
  lastMqttMsgMs = millis();
  String msg;
  msg.reserve(length);
  for (unsigned int i=0;i<length;i++) msg += (char)payload[i];

  DBG("MQTT [%s] %s\n", topic, msg.c_str());

  // Treat any message as refresh; matches your current 'refresh' payload too.
  downloadAndShowPhoto();
}

static void mqttSetupClient() {
  mqtt.setCallback(mqttCallback);
  
  // Choose the right client based on TLS setting
  if (cfg.mqttUseTLS) {
    if (cfg.mqttTlsInsecure) {
      mqttNetSecure.setInsecure();
    }
    // Don't set CA cert unless you want to validate
    mqtt.setClient(mqttNetSecure);
    DBG("MQTT: Using TLS client\n");
  } else {
    mqtt.setClient(mqttNetPlain);
    DBG("MQTT: Using plain client\n");
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
  
  // Rate limit connection attempts
  if (millis() - lastMqttAttemptMs < 15000) return;
  lastMqttAttemptMs = millis();
  
  // Skip if host is empty
  if (cfg.mqttHost.length() == 0) {
    DBG("MQTT: no host configured\n");
    return;
  }
  
  DBG("MQTT: connecting to %s:%u as %s\n",
  cfg.mqttHost.c_str(), cfg.mqttPort, HOSTNAME);

  bool ok;
  mqttNetPlain.setTimeout(1);
  mqttNetSecure.setTimeout(1);

  if (cfg.mqttUser.length()) {
    ok = mqtt.connect(HOSTNAME, cfg.mqttUser.c_str(), cfg.mqttPass.c_str());
  } else {
    ok = mqtt.connect(HOSTNAME);
  }
  if (ok) {
    mqttConnected = true;
    mqtt.subscribe(cfg.mqttTopic.c_str());
    DBG("MQTT: connected, subscribed: %s\n", cfg.mqttTopic.c_str());
  } else {
    DBG("MQTT: connect failed, rc=%d\n", mqtt.state());
    // Don't block - just try again later
  }
}

// ---------------------- WiFi / Captive portal ----------------------
static void startNetworkServicesOnce() {
  if (networkServicesStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  DBG("Starting mDNS + OTA as: %s\n", HOSTNAME);  // add this

  bool mdnsOk = MDNS.begin(HOSTNAME);
  DBG("mDNS.begin: %s\n", mdnsOk ? "ok" : "FAILED");

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.begin();

  networkServicesStarted = true;
  DBG("mDNS + OTA started - Hostname: %s\n", HOSTNAME);
}

static void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) { startNetworkServicesOnce(); return; }
  // Only rate-limit if we've attempted at least once
  if (lastWifiAttemptMs != 0 && (millis() - lastWifiAttemptMs) < 5000) return;
  lastWifiAttemptMs = millis();

  // Fast path: try your compiled-in creds first
  if (MYSSID && strlen(MYSSID)) {
    DBG("WiFi.begin(%s)\n", MYSSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(MYSSID, MYPSK);
    WiFi.setTxPower(WIFI_POWER_11dBm);
  }

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) delay(150);
  if (WiFi.status() == WL_CONNECTED) { startNetworkServicesOnce(); return; }

  // Captive portal fallback; saves to flash automatically
  DBG("WiFi failed -> captive portal\n");
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(15);
  wm.setConnectRetries(3);

  String ap = String(HOSTNAME) + "-setup";
  bool res = wm.autoConnect(ap.c_str());
  DBG("Portal result: %s\n", res ? "connected" : "failed/timeout");
  if (WiFi.status() == WL_CONNECTED) startNetworkServicesOnce();
}

// ---------------------- Web UI ----------------------
static void handleRoot() { server.send(200, "text/html; charset=utf-8", FPSTR(INDEX_HTML)); }
static void handleConfigPage() { server.send(200, "text/html; charset=utf-8", FPSTR(CONFIG_HTML)); }

static void handleStatusJson() {
  bool wifiOk = (WiFi.status() == WL_CONNECTED);

  String j = "{";
  j += "\"hostname\":\"" + jsonEscape(HOSTNAME) + "\",";
  j += "\"wifi\":" + String(wifiOk ? "true":"false") + ",";
  j += "\"ip\":\"" + (wifiOk ? WiFi.localIP().toString() : String("")) + "\",";
  j += "\"mdns\":" + String(networkServicesStarted ? "true":"false") + ",";
  j += "\"ota\":" + String(networkServicesStarted ? "true":"false") + ",";
  j += "\"mqtt\":" + String(mqttConnected ? "true":"false") + ",";
  j += "\"lastDownloadOk\":" + String(lastDownloadOk ? "true":"false") + ",";
  j += "\"lastDownloadErr\":\"" + jsonEscape(lastDownloadErr) + "\",";
  j += "\"lastDownloadMs\":" + String(lastDownloadMs) + ",";
  j += "\"lastMqttMsgMs\":" + String(lastMqttMsgMs) + ",";
  j += "\"screenW\":" + String(SCREEN_W) + ",";
  j += "\"screenH\":" + String(SCREEN_H);
  j += "}";
  server.send(200, "application/json", j);
}

static void handleGetConfigJson() {
  String j = "{";
  j += "\"photoBaseUrl\":\"" + jsonEscape(cfg.photoBaseUrl) + "\",";
  j += "\"photoFilename\":\"" + jsonEscape(cfg.photoFilename) + "\",";
  j += "\"httpsInsecure\":" + String(cfg.httpsInsecure ? "true":"false") + ",";  // Match form name
  j += "\"httpUser\":\"" + jsonEscape(cfg.httpUser) + "\",";
  j += "\"httpPass\":\"" + String(cfg.httpPass.length() ? "********" : "") + "\",";
  j += "\"mqttHost\":\"" + jsonEscape(cfg.mqttHost) + "\",";
  j += "\"mqttPort\":" + String(cfg.mqttPort) + ",";
  j += "\"mqttTopic\":\"" + jsonEscape(cfg.mqttTopic) + "\",";
  j += "\"mqttUser\":\"" + jsonEscape(cfg.mqttUser) + "\",";
  j += "\"mqttPass\":\"" + String(cfg.mqttPass.length() ? "********" : "") + "\",";
  j += "\"mqttUseTLS\":" + String(cfg.mqttUseTLS ? "true":"false") + ",";
  j += "\"mqttTlsInsecure\":" + String(cfg.mqttTlsInsecure ? "true":"false");
  j += "}";
  server.send(200, "application/json", j);
}

static void handlePostConfig() {
  DBG("=== POST /api/config ===\n");
  
  // Always update these fields
  if (server.hasArg("photoBaseUrl")) cfg.photoBaseUrl = server.arg("photoBaseUrl");
  if (server.hasArg("photoFilename")) cfg.photoFilename = server.arg("photoFilename");
  if (server.hasArg("httpUser")) cfg.httpUser = server.arg("httpUser");
  if (server.hasArg("mqttHost")) cfg.mqttHost = server.arg("mqttHost");
  if (server.hasArg("mqttTopic")) cfg.mqttTopic = server.arg("mqttTopic");
  if (server.hasArg("mqttUser")) cfg.mqttUser = server.arg("mqttUser");
  
  if (server.hasArg("mqttPort")) {
    cfg.mqttPort = (uint16_t)server.arg("mqttPort").toInt();
  }
  
  // Only update passwords if provided
  if (server.hasArg("httpPass") && server.arg("httpPass").length() > 0) {
    cfg.httpPass = server.arg("httpPass");
  }
  if (server.hasArg("mqttPass") && server.arg("mqttPass").length() > 0) {
    cfg.mqttPass = server.arg("mqttPass");
  }
  
  // Checkboxes
  cfg.httpsInsecure = server.hasArg("httpsInsecure");
  cfg.mqttUseTLS = server.hasArg("mqttUseTLS");
  cfg.mqttTlsInsecure = server.hasArg("mqttTlsInsecure");
  
  saveConfig();
  mqttSetupClient();
  
  // Return JSON instead of redirect
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleImgCurrent() {
  if (!currentJpg || !currentJpgLen) { server.send(404, "text/plain", "no image"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", (const char*)currentJpg, currentJpgLen);
}

static void handleImgLast() {
  if (!lastJpg || !lastJpgLen) { server.send(404, "text/plain", "no last image"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", (const char*)lastJpg, lastJpgLen);
}

static void handleActionRefresh() {
  bool ok = downloadAndShowPhoto();
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfigPage);

  server.on("/api/status", HTTP_GET, handleStatusJson);
  server.on("/api/config", HTTP_GET, handleGetConfigJson);
  server.on("/api/config", HTTP_POST, handlePostConfig);

  server.on("/api/refresh", HTTP_POST, handleActionRefresh);

  server.on("/img/current", HTTP_GET, handleImgCurrent);
  server.on("/img/last", HTTP_GET, handleImgLast);

  server.begin();
}

// ---------------------- Arduino setup/loop ----------------------
void setup() {
  DBG_BEGIN(115200);
  buildHostAndClientId();
  DBG("\nBoot %s\n", HOSTNAME);

  loadConfig();

  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  gfx->begin();
  gfx->fillScreen(gfx->color565(0, 0, 0));
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  ts.begin();
  ts.setRotation(0);

  ensureWifi();

  setupWeb();

  mqttNetPlain.setTimeout(1);
  mqttNetSecure.setTimeout(1);

  mqttSetupClient();
  mqtt.setSocketTimeout(1);  // seconds (PubSubClient socket timeout) [web:113]

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

  bool pressed = touchPressed();
  if (pressed && !showingLast && lastJpg && lastJpgLen) {
    showingLast = true;
    drawJpegFullScreen(lastJpg, lastJpgLen);
  } else if (!pressed && showingLast && currentJpg && currentJpgLen) {
    showingLast = false;
    drawJpegFullScreen(currentJpg, currentJpgLen);
  }

  delay(10);
}
