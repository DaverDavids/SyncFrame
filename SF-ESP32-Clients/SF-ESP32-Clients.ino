#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <Update.h>
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
  const char* ARDUINO_OTA_PASSWORD = "";
#endif

#include "board_config.h"
#include "html.h"
#include "splash.h"

char HOSTNAME[32];
char MAC_STR[18];
static const char* HOST_PREFIX = "syncframe-";

// ---------------------------------------------------------------------------
// Draw mutex
// ---------------------------------------------------------------------------
static SemaphoreHandle_t drawMutex = nullptr;

// ---------------------------------------------------------------------------
// Boot timestamp for uptime reporting
// ---------------------------------------------------------------------------
static unsigned long bootTimeMs = 0;

// ---------------------------------------------------------------------------
// Deferred draw flags
// ---------------------------------------------------------------------------
static volatile bool mqttRefreshPending = false;
static volatile bool webRefreshPending  = false;

// ---------------------------------------------------------------------------
// Log buffer spinlock
// ---------------------------------------------------------------------------
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// Captive Portal DNS + Web portal state
// ---------------------------------------------------------------------------
DNSServer dnsServer;
static bool portalActive = false;
static bool portalDone   = false;
static unsigned long portalStartMs = 0;
static const uint32_t PORTAL_TIMEOUT_MS = 180000;

// ---------------------------------------------------------------------------
// Config / Prefs
// ---------------------------------------------------------------------------
Preferences prefs;
struct Config {
  String wifiSsid;
  String wifiPass;
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
  String updateUrl;
  uint32_t updateIntervalMin;
  String webUser;
  String webPass;
} cfg;

// ---------------------------------------------------------------------------
// OTA build token - persisted in NVS as "fwtoken".
//
// The manifest line format is:
//   <hostname> <filename_or_url> <token>
//
// <token> is any opaque string the server writes when it drops a new binary
// (e.g. a Unix timestamp: $(date +%s), a git short-SHA, an MD5, etc.).
// The device stores the token it last successfully flashed. On every manifest
// check it compares the manifest token against the stored one. If they match
// the binary is already current and the flash is skipped entirely.
// This works correctly even when the filename never changes.
// ---------------------------------------------------------------------------
static String installedFwToken    = "";
static String installedFwFilename = "";  // kept for /api/status display only

static const char* PREF_NS = "syncframe";
static const char* DEFAULT_PHOTO_BASEURL       = "https://192.168.6.202:8369/syncframe/";
static const char* DEFAULT_PHOTO_FILE          = "photo.800x480.jpg";
static const char* DEFAULT_HTTP_USER           = "david";
static const char* DEFAULT_MQTT_USER           = "david";
static const char* DEFAULT_MQTT_HOST           = "192.168.6.202";
static const uint16_t DEFAULT_MQTT_PORT        = 8368;
static const char* DEFAULT_MQTT_TOPIC          = "photos";
static const char* DEFAULT_UPDATE_URL          = "";
static const uint32_t DEFAULT_UPDATE_INTERVAL_MIN = 60;
static const char* DEFAULT_WEB_USER            = "admin";
static const char* DEFAULT_WEB_PASS            = "";
static const size_t MAX_JPG = 1200 * 1024;

WebServer server(80);
WiFiClient   mqttNetPlain;
WiFiClientSecure mqttNetSecure;
PubSubClient mqtt;

bool networkServicesStarted = false;
unsigned long lastWifiAttemptMs  = 0;
unsigned long lastMqttAttemptMs  = 0;

volatile bool mqttConnected  = false;
volatile bool lastDownloadOk = false;
String lastDownloadErr = "";
unsigned long lastDownloadMs = 0;
unsigned long lastMqttMsgMs  = 0;

uint8_t* currentJpg    = nullptr;
size_t   currentJpgLen = 0;
uint8_t* lastJpg       = nullptr;
size_t   lastJpgLen    = 0;
bool showingLast        = false;
bool wifiEverConnected  = false;

static TaskHandle_t otaTaskHandle  = nullptr;
static volatile bool otaInProgress = false;

static const uint8_t LOG_CAP = 48;
static const size_t LOG_MSG_LEN = 96;
struct LogEntry {
  uint32_t seq;
  uint32_t ms;
  char tag[8];
  char msg[LOG_MSG_LEN];
};
static LogEntry logBuf[LOG_CAP];
static uint8_t  logHead  = 0;
static uint8_t  logCount = 0;
static uint32_t logSeq   = 0;

// ============================================================
// Web Authentication
// ============================================================
static bool requireWebAuth() {
  if (cfg.webPass.length() == 0) return true;
  if (server.authenticate(cfg.webUser.c_str(), cfg.webPass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "SyncFrame", "Authentication required");
  return false;
}

// ---------------------- Hardware Callbacks ----------------------
bool hasLastPhoto() { return (lastJpg != nullptr && lastJpgLen > 0); }

void showCurrentPhoto() {
  if (currentJpg && currentJpgLen) {
    showingLast = false;
    if (xSemaphoreTake(drawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      board_draw_jpeg(currentJpg, currentJpgLen);
      xSemaphoreGive(drawMutex);
    }
  }
}

void showLastPhoto() {
  if (lastJpg && lastJpgLen) {
    showingLast = true;
    if (xSemaphoreTake(drawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      board_draw_jpeg(lastJpg, lastJpgLen);
      xSemaphoreGive(drawMutex);
    }
  }
}

static bool downloadAndShowPhoto();
void triggerPhotoDownload() { downloadAndShowPhoto(); }

// ---------------------- Helpers ----------------------
static void buildHostAndClientId() {
  uint64_t fullMac = ESP.getEfuseMac();
  uint8_t m[6];
  m[0] = (fullMac >>  0) & 0xFF;
  m[1] = (fullMac >>  8) & 0xFF;
  m[2] = (fullMac >> 16) & 0xFF;
  m[3] = (fullMac >> 24) & 0xFF;
  m[4] = (fullMac >> 32) & 0xFF;
  m[5] = (fullMac >> 40) & 0xFF;
  snprintf(MAC_STR, sizeof(MAC_STR), "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
  uint32_t mac32   = (uint32_t)fullMac;
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
  portENTER_CRITICAL(&logMux);
  LogEntry& e = logBuf[logHead];
  e.seq = ++logSeq;
  e.ms  = millis();
  strncpy(e.tag, tag ? tag : "LOG", sizeof(e.tag) - 1);
  e.tag[sizeof(e.tag) - 1] = 0;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
  va_end(ap);
  logHead = (logHead + 1) % LOG_CAP;
  if (logCount < LOG_CAP) logCount++;
  portEXIT_CRITICAL(&logMux);
  DBG("[%s] %s\n", e.tag, e.msg);
}

static void appendJsonEscaped(String& out, const char* s) {
  if (!s) return;
  while (*s) {
    char c = *s++;
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:    out += c;     break;
    }
  }
}

static void appendJsonEscaped(String& out, const String& s) {
  appendJsonEscaped(out, s.c_str());
}

static void appendJsonPassword(String& out, const String& pass) {
  if (pass.length() > 0) {
    appendJsonEscaped(out, String("********"));
  }
}

static void freeBuf(uint8_t*& p, size_t& n) {
  if (p) { free(p); p = nullptr; }
  n = 0;
}

static void applyWifiDefaults() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(false);
}

static void sanitizeStoredString(String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((uint8_t)c >= 0x20) out += c;
  }
  s = out;
}

static void loadConfig() {
  prefs.begin(PREF_NS, true);
  cfg.wifiSsid          = prefs.getString("wssid",   "");
  cfg.wifiPass          = prefs.getString("wpsk",    "");
  cfg.photoBaseUrl      = prefs.getString("pbase",   DEFAULT_PHOTO_BASEURL);
  cfg.photoFilename     = prefs.getString("pfile",   DEFAULT_PHOTO_FILE);
  cfg.httpsInsecure     = prefs.getBool("pinsec",    true);
  cfg.httpUser          = prefs.getString("puser",   DEFAULT_HTTP_USER);
  cfg.httpPass          = prefs.getString("ppass",   String(test_http_password));
  cfg.mqttHost          = prefs.getString("mhost",   DEFAULT_MQTT_HOST);
  cfg.mqttPort          = prefs.getUShort("mport",   DEFAULT_MQTT_PORT);
  cfg.mqttTopic         = prefs.getString("mtopic",  DEFAULT_MQTT_TOPIC);
  cfg.mqttUser          = prefs.getString("muser",   DEFAULT_MQTT_USER);
  cfg.mqttPass          = prefs.getString("mpass",   String(test_http_password));
  cfg.mqttUseTLS        = prefs.getBool("mtls",      true);
  cfg.mqttTlsInsecure   = prefs.getBool("mtlsins",   true);
  cfg.updateUrl         = prefs.getString("updurl",  DEFAULT_UPDATE_URL);
  cfg.updateIntervalMin = prefs.getUInt("updint",    DEFAULT_UPDATE_INTERVAL_MIN);
  cfg.webUser           = prefs.getString("wbuser",  DEFAULT_WEB_USER);
  cfg.webPass           = prefs.getString("wbpass",  DEFAULT_WEB_PASS);
  installedFwToken      = prefs.getString("fwtoken", "");
  installedFwFilename   = prefs.getString("fwfile",  "");
  prefs.end();

  bool webPassValid = true;
  for (size_t i = 0; i < cfg.webPass.length(); i++) {
    char c = cfg.webPass[i];
    if ((uint8_t)c < 0x20 || c == '"' || c == '\\') { webPassValid = false; break; }
  }
  if (!webPassValid) {
    logEvent("CFG", "webPass contained invalid chars, clearing");
    cfg.webPass = "";
    prefs.begin(PREF_NS, false);
    prefs.putString("wbpass", "");
    prefs.end();
  }

  if (cfg.wifiSsid.length() == 0 && MYSSID && strlen(MYSSID)) {
    cfg.wifiSsid = String(MYSSID);
    cfg.wifiPass = String(MYPSK);
  }
}

static void saveConfig() {
  prefs.begin(PREF_NS, false);
  prefs.putString("wssid",  cfg.wifiSsid);
  prefs.putString("wpsk",   cfg.wifiPass);
  prefs.putString("pbase",  cfg.photoBaseUrl);
  prefs.putString("pfile",  cfg.photoFilename);
  prefs.putBool("pinsec",   cfg.httpsInsecure);
  prefs.putString("puser",  cfg.httpUser);
  prefs.putString("ppass",  cfg.httpPass);
  prefs.putString("mhost",  cfg.mqttHost);
  prefs.putUShort("mport",  cfg.mqttPort);
  prefs.putString("mtopic", cfg.mqttTopic);
  prefs.putString("muser",  cfg.mqttUser);
  prefs.putString("mpass",  cfg.mqttPass);
  prefs.putBool("mtls",     cfg.mqttUseTLS);
  prefs.putBool("mtlsins",  cfg.mqttTlsInsecure);
  prefs.putString("updurl", cfg.updateUrl);
  prefs.putUInt("updint",   cfg.updateIntervalMin);
  prefs.putString("wbuser", cfg.webUser);
  prefs.putString("wbpass", cfg.webPass);
  prefs.end();
}

// Save both the token (for change detection) and filename (for display)
static void saveInstalledFw(const char* filename, const char* token) {
  installedFwFilename = filename;
  installedFwToken    = token;
  prefs.begin(PREF_NS, false);
  prefs.putString("fwfile",  filename);
  prefs.putString("fwtoken", token);
  prefs.end();
  logEvent("OTA", "installed fw: file=%s token=%s", filename, token);
}

static String makePhotoUrl() {
  String base = cfg.photoBaseUrl;
  if (!base.endsWith("/")) base += "/";
  return base + cfg.photoFilename;
}

// ============================================================
// Captive Portal
// ============================================================
static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>SyncFrame Wi-Fi Setup</title>
<style>
 body{font-family:sans-serif;background:#111;color:#eee;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0}
 .box{background:#222;border-radius:12px;padding:2em;width:90%;max-width:340px;box-shadow:0 4px 24px #0008}
 h2{text-align:center;margin-top:0}label{display:block;margin:.5em 0 .2em}
 input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:.5em;border-radius:6px;border:1px solid #555;background:#333;color:#eee;font-size:1em}
 button{width:100%;padding:.7em;margin-top:1em;border:none;border-radius:8px;background:#2979ff;color:#fff;font-size:1.1em;cursor:pointer}
 button:hover{background:#1565c0}.note{font-size:.85em;color:#aaa;text-align:center;margin-top:1em}
</style></head><body>
<div class='box'>
 <h2>&#128247; SyncFrame</h2>
 <p style='text-align:center;color:#aaa'>Wi-Fi Setup</p>
 <form method='POST' action='/portal/save'>
  <label>Network (SSID)</label>
  <input type='text' name='ssid' placeholder='Your Wi-Fi name' required>
  <label>Password</label>
  <input type='password' name='pass' placeholder='Wi-Fi password'>
  <button type='submit'>Connect</button>
 </form>
 <p class='note'>Device: HOSTNAME_PLACEHOLDER</p>
</div></body></html>
)rawliteral";

static const char PORTAL_SAVED_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Saved</title>
<style>body{font-family:sans-serif;background:#111;color:#eee;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0}.box{background:#222;border-radius:12px;padding:2em;width:90%;max-width:340px;text-align:center}</style>
</head><body>
<div class='box'>
 <h2>&#9989; Saved!</h2>
 <p>Credentials saved. The device will now attempt to connect.</p>
 <p style='color:#aaa;font-size:.9em'>You can close this window.</p>
</div></body></html>
)rawliteral";

static void handleCaptiveRedirect() {
  server.sendHeader("Location", "http://192.168.4.1/portal", true);
  server.send(302, "text/plain", "");
}

static void handlePortalPage() {
  String html = FPSTR(PORTAL_HTML);
  html.replace("HOSTNAME_PLACEHOLDER", String(HOSTNAME));
  server.send(200, "text/html; charset=utf-8", html);
}

static void handlePortalSave() {
  if (server.hasArg("ssid") && server.arg("ssid").length() > 0) {
    cfg.wifiSsid = server.arg("ssid");
    cfg.wifiPass = server.hasArg("pass") ? server.arg("pass") : "";
    saveConfig();
    logEvent("PORTAL", "creds saved ssid=%s", cfg.wifiSsid.c_str());
    portalDone = true;
  }
  server.send(200, "text/html; charset=utf-8", FPSTR(PORTAL_SAVED_HTML));
}

static void setupPortalRoutes() {
  server.on("/portal",              HTTP_GET,  handlePortalPage);
  server.on("/portal/save",         HTTP_POST, handlePortalSave);
  server.on("/hotspot-detect.html", HTTP_GET,  handleCaptiveRedirect);
  server.on("/generate_204",        HTTP_GET,  handleCaptiveRedirect);
  server.on("/gen_204",             HTTP_GET,  handleCaptiveRedirect);
  server.on("/ncsi.txt",            HTTP_GET,  handleCaptiveRedirect);
  server.on("/connecttest.txt",     HTTP_GET,  handleCaptiveRedirect);
  server.on("/redirect",            HTTP_GET,  handleCaptiveRedirect);
  server.onNotFound(handleCaptiveRedirect);
}

static void startPortalMode() {
  if (portalActive) return;
  String apName = String(HOSTNAME) + "-setup";
  logEvent("PORTAL", "starting AP %s", apName.c_str());
  board_draw_boot_status(("Wi-Fi Setup: " + apName).c_str());

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str());
  delay(100);

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());

  setupPortalRoutes();
  server.begin();

  portalActive  = true;
  portalDone    = false;
  portalStartMs = millis();
  logEvent("PORTAL", "active ip=%s", WiFi.softAPIP().toString().c_str());
}

static void handlePortalLoop() {
  if (!portalActive) return;
  dnsServer.processNextRequest();
  server.handleClient();

  bool timedOut = (millis() - portalStartMs) > PORTAL_TIMEOUT_MS;
  if (portalDone || timedOut) {
    logEvent("PORTAL", "stopping done=%d timeout=%d", (int)portalDone, (int)timedOut);
    dnsServer.stop();
    server.stop();
    WiFi.softAPdisconnect(true);
    portalActive      = false;
    wifiEverConnected = false;
    lastWifiAttemptMs = 0;
  }
}

// ============================================================
// HTTP photo download
// ============================================================
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

  WiFiClientSecure* secureClient = nullptr;
  WiFiClient*       plainClient  = nullptr;
  HTTPClient*       http         = new HTTPClient();
  if (!http) { if (outErr) *outErr = "alloc failed"; return false; }

  http->setConnectTimeout(4000);
  http->setTimeout(5000);

  bool isHttps = url.startsWith("https://");
  bool beginOk = false;

  if (isHttps) {
    secureClient = new WiFiClientSecure();
    if (!secureClient) { delete http; if (outErr) *outErr = "alloc failed"; return false; }
    secureClient->setTimeout(5);
    if (cfg.httpsInsecure) secureClient->setInsecure();
    beginOk = http->begin(*secureClient, url);
  } else {
    plainClient = new WiFiClient();
    if (!plainClient) { delete http; if (outErr) *outErr = "alloc failed"; return false; }
    plainClient->setTimeout(5);
    beginOk = http->begin(*plainClient, url);
  }

  if (!beginOk) {
    if (outErr) *outErr = "http.begin failed";
    delete http; delete secureClient; delete plainClient;
    return false;
  }

  if (cfg.httpUser.length() > 0)
    http->setAuthorization(cfg.httpUser.c_str(), cfg.httpPass.c_str());

  int code = http->GET();
  if (code != HTTP_CODE_OK) {
    String err = (code < 0) ? http->errorToString(code) : ("HTTP " + String(code));
    if (outErr) *outErr = err;
    logEvent("HTTP", "GET failed %s", err.c_str());
    http->end(); delete http; delete secureClient; delete plainClient;
    return false;
  }

  int64_t total = http->getSize();
  if (total > (int64_t)MAX_JPG) {
    if (outErr) *outErr = "image too large";
    http->end(); delete http; delete secureClient; delete plainClient;
    return false;
  }

  size_t allocSize = (total > 0) ? (size_t)total : MAX_JPG;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t*)malloc(allocSize);
  if (!buf) {
    if (outErr) *outErr = "malloc failed";
    http->end(); delete http; delete secureClient; delete plainClient;
    return false;
  }

  WiFiClient* stream = http->getStreamPtr();
  size_t readTotal = 0;
  unsigned long lastProgressMs = millis();

  while ((http->connected() || stream->available()) && (millis() - lastProgressMs) < 5000) {
    size_t avail = stream->available();
    if (!avail) { delay(1); continue; }
    size_t toRead = avail;
    if (readTotal + toRead > allocSize) toRead = allocSize - readTotal;
    if (toRead == 0) {
      free(buf); if (outErr) *outErr = "image too large";
      http->end(); delete http; delete secureClient; delete plainClient;
      return false;
    }
    int n = stream->readBytes(buf + readTotal, toRead);
    if (n <= 0) break;
    readTotal += (size_t)n;
    lastProgressMs = millis();
    if (total > 0 && readTotal >= (size_t)total) break;
  }

  http->end(); delete http; delete secureClient; delete plainClient;

  if ((total > 0 && readTotal != (size_t)total) || readTotal < 16) {
    free(buf); if (outErr) *outErr = "short read";
    logEvent("PHOTO", "short read %u/%u", (unsigned)readTotal, (unsigned)((total > 0) ? total : 0));
    return false;
  }

  *outBuf = buf;
  *outLen = readTotal;
  logEvent("PHOTO", "download ok bytes=%u", (unsigned)readTotal);
  return true;
}

static bool downloadAndShowPhoto() {
  if (otaInProgress) return false;

  uint8_t* newBuf = nullptr;
  size_t   newLen = 0;
  String   err;

  if (WiFi.status() != WL_CONNECTED) {
    lastDownloadOk  = false;
    lastDownloadErr = "no wifi";
    logEvent("PHOTO", "skipped no wifi");
    return false;
  }

  if (!currentJpg && !lastJpg) board_draw_boot_status("Downloading Photo...");

  bool ok = httpDownloadToBuffer(&newBuf, &newLen, &err);
  lastDownloadMs = millis();

  if (!ok) {
    lastDownloadOk  = false;
    lastDownloadErr = err;
    logEvent("PHOTO", "download failed %s", err.c_str());
    if (xSemaphoreTake(drawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (currentJpg && currentJpgLen)        board_draw_jpeg(currentJpg, currentJpgLen);
      else if (lastJpg && lastJpgLen)          board_draw_jpeg(lastJpg, lastJpgLen);
      else board_draw_boot_status((String("Download failed: ") + err).c_str());
      xSemaphoreGive(drawMutex);
    }
    return false;
  }

  if (currentJpg && newLen == currentJpgLen &&
      memcmp(newBuf, currentJpg, newLen) == 0) {
    free(newBuf);
    lastDownloadOk  = true;
    lastDownloadErr = "";
    logEvent("PHOTO", "same image, no rotation bytes=%u", (unsigned)newLen);
    showingLast = false;
    if (xSemaphoreTake(drawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      board_draw_jpeg(currentJpg, currentJpgLen);
      xSemaphoreGive(drawMutex);
    }
    return true;
  }

  freeBuf(lastJpg, lastJpgLen);
  lastJpg       = currentJpg;
  lastJpgLen    = currentJpgLen;
  currentJpg    = newBuf;
  currentJpgLen = newLen;
  lastDownloadOk  = true;
  lastDownloadErr = "";
  showingLast = false;

  if (xSemaphoreTake(drawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    board_draw_jpeg(currentJpg, currentJpgLen);
    xSemaphoreGive(drawMutex);
  }
  logEvent("PHOTO", "showing new photo bytes=%u", (unsigned)currentJpgLen);
  return true;
}

// ============================================================
// Background OTA update task  (pinned Core 1, stack 16384)
//
// Manifest line format (space-separated):
//   <hostname>  <filename_or_url>  <token>
//
// <token> is an opaque string the server writes each time it drops
// a new binary - e.g. a Unix timestamp or git short-SHA.
// The device stores the last flashed token in NVS ("fwtoken").
// If manifest token == stored token  ->  skip (already current).
// If manifest token != stored token  ->  flash, then store new token.
// This is correct even when the filename never changes.
// ============================================================
static void otaUpdateTask(void* pv) {
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(5000));
  vTaskDelay(pdMS_TO_TICKS(30000));

  for (;;) {
    char     updateUrlSnap[256]    = {};
    char     installedTokenSnap[64] = {};
    uint32_t intervalMinSnap       = DEFAULT_UPDATE_INTERVAL_MIN;
    strncpy(updateUrlSnap,      cfg.updateUrl.c_str(),        sizeof(updateUrlSnap)    - 1);
    strncpy(installedTokenSnap, installedFwToken.c_str(),     sizeof(installedTokenSnap) - 1);
    intervalMinSnap = cfg.updateIntervalMin;

    uint32_t intervalMs = intervalMinSnap * 60UL * 1000UL;
    if (intervalMs == 0) intervalMs = 3600000UL;

    if (updateUrlSnap[0] != '\0' && WiFi.status() == WL_CONNECTED && !otaInProgress) {
      logEvent("OTA", "checking manifest %s", updateUrlSnap);

      HTTPClient*       http  = new HTTPClient();
      WiFiClientSecure* sec   = nullptr;
      WiFiClient*       plain = nullptr;
      char fwFilename[128] = {};
      char fwToken[64]     = {};
      char binUrl[256]     = {};

      if (http) {
        http->setConnectTimeout(5000);
        http->setTimeout(6000);
        bool ok = false;
        bool isHttps = (strncmp(updateUrlSnap, "https://", 8) == 0);
        if (isHttps) {
          sec = new WiFiClientSecure();
          if (sec) { sec->setInsecure(); ok = http->begin(*sec, updateUrlSnap); }
        } else {
          plain = new WiFiClient();
          if (plain) ok = http->begin(*plain, updateUrlSnap);
        }

        if (ok) {
          char compiledBuf[32];
          snprintf(compiledBuf, sizeof(compiledBuf), "%s %s", __DATE__, __TIME__);
          char uptimeBuf[16];
          snprintf(uptimeBuf, sizeof(uptimeBuf), "%lu", (unsigned long)((millis() - bootTimeMs) / 1000));
          http->addHeader("X-SF-Hostname", HOSTNAME);
          http->addHeader("X-SF-Compiled", compiledBuf);
          http->addHeader("X-SF-Uptime",   uptimeBuf);

          if (http->GET() == HTTP_CODE_OK) {
            WiFiClient* s = http->getStreamPtr();
            if (!s) {
              logEvent("OTA", "null stream after GET");
              delete http; delete sec; delete plain;
              http = nullptr; sec = nullptr; plain = nullptr;
            } else {
              char line[320] = {};
              size_t lineLen = 0;
              unsigned long t = millis();

              while ((http->connected() || s->available()) && millis() - t < 8000) {
                if (s->available()) {
                  char c = (char)s->read();
                  if (c == '\n' || c == '\r') {
                    // Trim trailing whitespace
                    while (lineLen > 0 && (line[lineLen-1] == ' ' || line[lineLen-1] == '\t'))
                      lineLen--;
                    line[lineLen] = '\0';

                    // Trim leading whitespace
                    const char* trimmed = line;
                    while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

                    if (lineLen > 0 && strstr(trimmed, HOSTNAME) != nullptr) {
                      // Parse up to 3 space-separated fields:
                      // field[0] = hostname (already matched)
                      // field[1] = filename or full URL
                      // field[2] = token (optional but required for skip logic)
                      char fields[3][256] = {};
                      int  fieldCount = 0;
                      const char* p = trimmed;
                      while (*p && fieldCount < 3) {
                        while (*p == ' ' || *p == '\t') p++;
                        if (!*p) break;
                        size_t fi = 0;
                        while (*p && *p != ' ' && *p != '\t' && fi < 255)
                          fields[fieldCount][fi++] = *p++;
                        fields[fieldCount][fi] = '\0';
                        fieldCount++;
                      }

                      // field[1] = filename or URL, field[2] = token
                      if (fieldCount >= 2) {
                        const char* fileOrUrl = fields[1];
                        if (strncmp(fileOrUrl, "http", 4) == 0) {
                          strncpy(binUrl, fileOrUrl, sizeof(binUrl) - 1);
                          const char* lastSlash = strrchr(fileOrUrl, '/');
                          strncpy(fwFilename,
                                  lastSlash ? lastSlash + 1 : fileOrUrl,
                                  sizeof(fwFilename) - 1);
                        } else {
                          strncpy(fwFilename, fileOrUrl, sizeof(fwFilename) - 1);
                          char baseDir[256] = {};
                          const char* lastSlash = strrchr(updateUrlSnap, '/');
                          if (lastSlash && (lastSlash - updateUrlSnap) > 7) {
                            size_t baseLen = (size_t)(lastSlash - updateUrlSnap) + 1;
                            if (baseLen < sizeof(baseDir)) {
                              memcpy(baseDir, updateUrlSnap, baseLen);
                              baseDir[baseLen] = '\0';
                            }
                          } else {
                            strncpy(baseDir, updateUrlSnap, sizeof(baseDir) - 1);
                          }
                          snprintf(binUrl, sizeof(binUrl), "%sfirmware/%s", baseDir, fileOrUrl);
                        }
                        if (fieldCount >= 3)
                          strncpy(fwToken, fields[2], sizeof(fwToken) - 1);
                        logEvent("OTA", "manifest: file=%s token=%s", fwFilename, fwToken[0] ? fwToken : "(none)");
                      }
                      break;
                    }
                    lineLen = 0;
                    line[0] = '\0';
                    t = millis();
                  } else {
                    if (lineLen < sizeof(line) - 1) line[lineLen++] = c;
                  }
                } else {
                  vTaskDelay(1);
                }
              }
            }
          } else {
            logEvent("OTA", "manifest fetch failed");
          }
        }
        if (http) { http->end(); delete http; delete sec; delete plain; }
      }

      if (binUrl[0] != '\0') {
        // Skip if token matches - this is the primary update gate.
        // If no token in manifest, fall back to filename comparison.
        bool alreadyCurrent = false;
        if (fwToken[0] != '\0' && installedTokenSnap[0] != '\0') {
          alreadyCurrent = (strcmp(fwToken, installedTokenSnap) == 0);
        } else if (fwToken[0] == '\0') {
          // Old-style manifest (no token) - compare filename as before
          char installedFileSnap[128] = {};
          strncpy(installedFileSnap, installedFwFilename.c_str(), sizeof(installedFileSnap) - 1);
          alreadyCurrent = (fwFilename[0] != '\0' && strcmp(fwFilename, installedFileSnap) == 0);
        }

        if (alreadyCurrent) {
          logEvent("OTA", "already current (token=%s), skipping", fwToken[0] ? fwToken : fwFilename);
        } else {
          otaInProgress = true;
          logEvent("OTA", "update needed token=%s->%s, downloading %s",
                   installedTokenSnap[0] ? installedTokenSnap : "(none)",
                   fwToken[0]            ? fwToken            : "(none)",
                   binUrl);

          HTTPClient*       fhttp  = new HTTPClient();
          WiFiClientSecure* fsec   = nullptr;
          WiFiClient*       fplain = nullptr;
          int32_t     fwSize   = -1;
          WiFiClient* fwStream = nullptr;
          bool fetchOk = false;

          if (fhttp) {
            fhttp->setConnectTimeout(10000);
            fhttp->setTimeout(60000);
            bool fok = false;
            bool binIsHttps = (strncmp(binUrl, "https://", 8) == 0);
            if (binIsHttps) {
              fsec = new WiFiClientSecure();
              if (fsec) { fsec->setInsecure(); fok = fhttp->begin(*fsec, binUrl); }
            } else {
              fplain = new WiFiClient();
              if (fplain) fok = fhttp->begin(*fplain, binUrl);
            }
            if (fok) {
              int code = fhttp->GET();
              if (code == HTTP_CODE_OK) {
                fwSize   = fhttp->getSize();
                fwStream = fhttp->getStreamPtr();
                if (!fwStream) logEvent("OTA", "null fw stream");
                else fetchOk = true;
              } else {
                logEvent("OTA", "firmware fetch failed code=%d", code);
              }
            }
          }

          if (fetchOk && fwStream) {
            logEvent("OTA", "waiting for draw mutex before flash");
            if (xSemaphoreTake(drawMutex, portMAX_DELAY) == pdTRUE) {
              logEvent("OTA", "starting flash size=%d", fwSize);
              bool flashed = false;
              if (Update.begin(fwSize > 0 ? (size_t)fwSize : UPDATE_SIZE_UNKNOWN)) {
                size_t written = Update.writeStream(*fwStream);
                if (Update.end() && Update.isFinished()) {
                  logEvent("OTA", "flash ok bytes=%u", (unsigned)written);
                  flashed = true;
                } else {
                  logEvent("OTA", "Update.end error: %s", Update.errorString());
                }
              } else {
                logEvent("OTA", "Update.begin error: %s", Update.errorString());
              }
              xSemaphoreGive(drawMutex);
              if (flashed) {
                saveInstalledFw(fwFilename, fwToken[0] ? fwToken : fwFilename);
                fhttp->end(); delete fhttp; delete fsec; delete fplain;
                delay(1500);
                ESP.restart();
              }
            }
          }

          if (fhttp) { fhttp->end(); delete fhttp; delete fsec; delete fplain; }
          otaInProgress = false;
        }
      } else {
        logEvent("OTA", "no update for %s", HOSTNAME);
      }
    }

    uint32_t elapsed = 0;
    while (elapsed < intervalMs) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      elapsed += 1000;
      uint32_t newInterval = cfg.updateIntervalMin * 60UL * 1000UL;
      if (newInterval != intervalMs) break;
    }
  }
}

static void startOtaTask() {
  if (cfg.updateUrl.length() == 0) return;
  if (otaTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(
    otaUpdateTask, "ota_check", 16384, nullptr,
    1, &otaTaskHandle, 1
  );
  logEvent("OTA", "task started interval=%umin url=%s token=%s",
           (unsigned)cfg.updateIntervalMin, cfg.updateUrl.c_str(),
           installedFwToken.length() ? installedFwToken.c_str() : "none");
}

// ---------------------- MQTT ----------------------
static void mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
  (void)payload;
  lastMqttMsgMs = millis();
  logEvent("MQTT", "message topic=%s bytes=%u", topic ? topic : "", length);
  mqttRefreshPending = true;
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
  if (mqtt.connected()) { mqttConnected = true; return; }
  mqttConnected = false;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttemptMs < 15000) return;
  lastMqttAttemptMs = millis();
  if (cfg.mqttHost.length() == 0) return;

  mqttNetPlain.setTimeout(1);
  mqttNetSecure.setTimeout(1);
  logEvent("MQTT", "connect %s:%u tls=%s", cfg.mqttHost.c_str(), cfg.mqttPort,
           cfg.mqttUseTLS ? "yes" : "no");

  bool ok = cfg.mqttUser.length()
    ? mqtt.connect(HOSTNAME, cfg.mqttUser.c_str(), cfg.mqttPass.c_str())
    : mqtt.connect(HOSTNAME);

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
  if (ARDUINO_OTA_PASSWORD && strlen(ARDUINO_OTA_PASSWORD) > 0)
    ArduinoOTA.setPassword(ARDUINO_OTA_PASSWORD);
  ArduinoOTA.begin();
  networkServicesStarted = true;
  wifiEverConnected      = true;
  logEvent("WIFI", "connected ip=%s mac=%s", WiFi.localIP().toString().c_str(), MAC_STR);
  logEvent("NET", "services mdns=%s ota=on", mdnsOk ? "on" : "off");
}

static void ensureWifi() {
  if (portalActive) { handlePortalLoop(); return; }

  if (WiFi.status() == WL_CONNECTED) { startNetworkServicesOnce(); return; }
  if (lastWifiAttemptMs != 0 && (millis() - lastWifiAttemptMs) < 5000) return;
  lastWifiAttemptMs = millis();

  applyWifiDefaults();

  if (wifiEverConnected) { logEvent("WIFI", "reconnect attempt"); WiFi.reconnect(); return; }

  if (cfg.wifiSsid.length() > 0) {
    logEvent("WIFI", "connect begin ssid=%s", cfg.wifiSsid.c_str());
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) delay(100);
    if (WiFi.status() == WL_CONNECTED) { startNetworkServicesOnce(); return; }
    logEvent("WIFI", "connect timed out status=%d", WiFi.status());
  } else {
    logEvent("WIFI", "no saved credentials");
  }

  startPortalMode();
}

// ---------------------- Web UI ----------------------
static void handleRoot() {
  if (!requireWebAuth()) return;
  server.send(200, "text/html; charset=utf-8", FPSTR(INDEX_HTML));
}

static void handleConfigPage() {
  if (!requireWebAuth()) return;

  String j;
  j.reserve(512);
  j += "{";
  j += "\"hostname\":\"";       appendJsonEscaped(j, HOSTNAME);          j += "\",";
  j += "\"photoBaseUrl\":\"";   appendJsonEscaped(j, cfg.photoBaseUrl);  j += "\",";
  j += "\"photoFilename\":\"";  appendJsonEscaped(j, cfg.photoFilename); j += "\",";
  j += "\"httpsInsecure\":";    j += (cfg.httpsInsecure ? "true" : "false"); j += ",";
  j += "\"httpUser\":\"";       appendJsonEscaped(j, cfg.httpUser);      j += "\",";
  j += "\"httpPass\":\"";       appendJsonPassword(j, cfg.httpPass);     j += "\",";
  j += "\"mqttHost\":\"";       appendJsonEscaped(j, cfg.mqttHost);      j += "\",";
  j += "\"mqttPort\":";         j += String(cfg.mqttPort);               j += ",";
  j += "\"mqttTopic\":\"";      appendJsonEscaped(j, cfg.mqttTopic);     j += "\",";
  j += "\"mqttUser\":\"";       appendJsonEscaped(j, cfg.mqttUser);      j += "\",";
  j += "\"mqttPass\":\"";       appendJsonPassword(j, cfg.mqttPass);     j += "\",";
  j += "\"mqttUseTLS\":";       j += (cfg.mqttUseTLS ? "true" : "false");  j += ",";
  j += "\"mqttTlsInsecure\":";  j += (cfg.mqttTlsInsecure ? "true" : "false"); j += ",";
  j += "\"updateUrl\":\"";      appendJsonEscaped(j, cfg.updateUrl);     j += "\",";
  j += "\"updateIntervalMin\":";j += String(cfg.updateIntervalMin);      j += ",";
  j += "\"webUser\":\"";        appendJsonEscaped(j, cfg.webUser);       j += "\",";
  j += "\"webPass\":\"";        appendJsonPassword(j, cfg.webPass);      j += "\"}";

  String html = FPSTR(CONFIG_HTML);
  html.replace("CFG_INJECT_PLACEHOLDER",
               String("<script>window._cfg=") + j + String(";</script>"));

  server.send(200, "text/html; charset=utf-8", html);
}

static void handleStatusJson() {
  if (!requireWebAuth()) return;
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  unsigned long uptimeMs = millis() - bootTimeMs;
  String j;
  j.reserve(640);
  j += "{";
  j += "\"hostname\":\""; appendJsonEscaped(j, HOSTNAME); j += "\",";
  j += "\"mac\":\"";      appendJsonEscaped(j, MAC_STR);  j += "\",";
  j += "\"wifi\":";       j += (wifiOk ? "true" : "false");           j += ",";
  j += "\"wifiEverConnected\":"; j += (wifiEverConnected ? "true" : "false"); j += ",";
  j += "\"ip\":\""; appendJsonEscaped(j, wifiOk ? WiFi.localIP().toString() : String("")); j += "\",";
  j += "\"mdns\":";  j += (networkServicesStarted ? "true" : "false"); j += ",";
  j += "\"ota\":";   j += (networkServicesStarted ? "true" : "false"); j += ",";
  j += "\"mqtt\":";  j += (mqttConnected ? "true" : "false");          j += ",";
  j += "\"lastDownloadOk\":";  j += (lastDownloadOk ? "true" : "false"); j += ",";
  j += "\"lastDownloadErr\":\""; appendJsonEscaped(j, lastDownloadErr); j += "\",";
  j += "\"lastDownloadMs\":"; j += String(lastDownloadMs); j += ",";
  j += "\"lastMqttMsgMs\":";  j += String(lastMqttMsgMs);  j += ",";
  j += "\"lastLogSeq\":";     j += String(logSeq);          j += ",";
  j += "\"otaInProgress\":";  j += (otaInProgress ? "true" : "false"); j += ",";
  j += "\"installedFw\":\"";  appendJsonEscaped(j, installedFwFilename); j += "\",";
  j += "\"installedFwToken\":\""; appendJsonEscaped(j, installedFwToken); j += "\",";
  j += "\"uptimeMs\":";       j += String(uptimeMs);        j += ",";
  j += "\"screenW\":"; j += String(SCREEN_W); j += ",";
  j += "\"screenH\":"; j += String(SCREEN_H);
  j += "}";
  server.send(200, "application/json", j);
}

static void handleLogJson() {
  if (!requireWebAuth()) return;
  uint32_t since = 0;
  if (server.hasArg("since")) since = strtoul(server.arg("since").c_str(), nullptr, 10);

  const uint8_t maxItems = 20;

  portENTER_CRITICAL(&logMux);
  uint8_t  snapHead  = logHead;
  uint8_t  snapCount = logCount;
  uint32_t snapSeq   = logSeq;
  LogEntry snapBuf[LOG_CAP];
  memcpy(snapBuf, logBuf, sizeof(logBuf));
  portEXIT_CRITICAL(&logMux);

  uint8_t start = (snapHead + LOG_CAP - snapCount) % LOG_CAP;
  uint8_t skip  = 0;
  if (since == 0 && snapCount > maxItems) skip = snapCount - maxItems;

  String j;
  j.reserve(2048);
  j += "{\"items\":[";
  bool first = true;
  uint8_t sent = 0;
  for (uint8_t i = 0; i < snapCount; i++) {
    uint8_t idx = (start + i) % LOG_CAP;
    const LogEntry& e = snapBuf[idx];
    if (skip) { skip--; continue; }
    if (e.seq <= since) continue;
    if (sent >= maxItems) break;
    if (!first) j += ',';
    first = false;
    j += "{\"seq\":";
    j += String(e.seq);
    j += ",\"ms\":";
    j += String(e.ms);
    j += ",\"tag\":\""; appendJsonEscaped(j, e.tag); j += "\",";
    j += "\"msg\":\"";  appendJsonEscaped(j, e.msg); j += "\"}";
    sent++;
  }
  j += "],\"nextSince\":";
  j += String(snapSeq);
  j += "}";
  server.send(200, "application/json", j);
}

static bool isRealPassword(const String& val) {
  return val.length() > 0 && val != "********";
}

static void handlePostConfig() {
  if (!requireWebAuth()) return;
  if (server.hasArg("photoBaseUrl"))       cfg.photoBaseUrl       = server.arg("photoBaseUrl");
  if (server.hasArg("photoFilename"))      cfg.photoFilename      = server.arg("photoFilename");
  if (server.hasArg("httpUser"))           cfg.httpUser           = server.arg("httpUser");
  if (server.hasArg("mqttHost"))           cfg.mqttHost           = server.arg("mqttHost");
  if (server.hasArg("mqttTopic"))          cfg.mqttTopic          = server.arg("mqttTopic");
  if (server.hasArg("mqttUser"))           cfg.mqttUser           = server.arg("mqttUser");
  if (server.hasArg("mqttPort"))           cfg.mqttPort           = (uint16_t)server.arg("mqttPort").toInt();
  if (server.hasArg("updateUrl"))          cfg.updateUrl          = server.arg("updateUrl");
  if (server.hasArg("updateIntervalMin"))  cfg.updateIntervalMin  = (uint32_t)server.arg("updateIntervalMin").toInt();
  if (server.hasArg("webUser") && server.arg("webUser").length() > 0)
    cfg.webUser = server.arg("webUser");
  if (server.hasArg("httpPass")  && isRealPassword(server.arg("httpPass")))  cfg.httpPass  = server.arg("httpPass");
  if (server.hasArg("mqttPass")  && isRealPassword(server.arg("mqttPass")))  cfg.mqttPass  = server.arg("mqttPass");
  if (server.hasArg("webPass")   && isRealPassword(server.arg("webPass")))   cfg.webPass   = server.arg("webPass");
  if (server.hasArg("webPassClear") && server.arg("webPassClear") == "1")    cfg.webPass   = "";
  cfg.httpsInsecure   = server.hasArg("httpsInsecure");
  cfg.mqttUseTLS      = server.hasArg("mqttUseTLS");
  cfg.mqttTlsInsecure = server.hasArg("mqttTlsInsecure");
  saveConfig();
  mqtt.disconnect();
  mqttSetupClient();
  lastMqttAttemptMs = 0;
  startOtaTask();
  logEvent("WEB", "config saved");
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleImgCurrent() {
  if (!requireWebAuth()) return;
  if (!currentJpg || !currentJpgLen) { server.send(404, "text/plain", "no image"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", (const char*)currentJpg, currentJpgLen);
}

static void handleImgLast() {
  if (!requireWebAuth()) return;
  if (!lastJpg || !lastJpgLen) { server.send(404, "text/plain", "no last image"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", (const char*)lastJpg, lastJpgLen);
}

static void handleActionRefresh() {
  if (!requireWebAuth()) return;
  if (otaInProgress) {
    server.send(503, "application/json", "{\"ok\":false,\"err\":\"ota in progress\"}");
    return;
  }
  logEvent("WEB", "manual refresh requested");
  webRefreshPending = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

static void setupWeb() {
  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/config",      HTTP_GET,  handleConfigPage);
  server.on("/api/status",  HTTP_GET,  handleStatusJson);
  server.on("/api/log",     HTTP_GET,  handleLogJson);
  server.on("/api/config",  HTTP_POST, handlePostConfig);
  server.on("/api/refresh", HTTP_POST, handleActionRefresh);
  server.on("/img/current", HTTP_GET,  handleImgCurrent);
  server.on("/img/last",    HTTP_GET,  handleImgLast);
  server.begin();
  logEvent("WEB", "server started");
}

// ---------------------- Main ----------------------
void setup() {
  DBG_BEGIN(115200);
  delay(50);
  bootTimeMs = millis();
  buildHostAndClientId();
  logEvent("BOOT", "%s mac=%s reset=%s", HOSTNAME, MAC_STR, resetReasonToStr(esp_reset_reason()));

  drawMutex = xSemaphoreCreateBinary();
  xSemaphoreGive(drawMutex);

  loadConfig();
  board_init();

  board_draw_jpeg(splash_logo, splash_logo_len);
  board_draw_boot_status("Connecting to Wi-Fi...");

  applyWifiDefaults();
  ensureWifi();

  if (!portalActive && WiFi.status() == WL_CONNECTED) {
    String ipMac = "IP: " + WiFi.localIP().toString() + "   MAC: " + String(MAC_STR);
    board_draw_boot_status(ipMac.c_str());
    delay(800);
    setupWeb();
    board_draw_boot_status("Connecting to MQTT...");
    mqttNetPlain.setTimeout(1);
    mqttNetSecure.setTimeout(1);
    mqttSetupClient();
    mqttMaybeReconnect();
    mqtt.setSocketTimeout(1);
    downloadAndShowPhoto();
    startOtaTask();
  } else if (portalActive) {
    logEvent("BOOT", "waiting for Wi-Fi credentials via portal");
  }
}

void loop() {
  ensureWifi();

  if (WiFi.status() == WL_CONNECTED) {
    if (!networkServicesStarted) {
      setupWeb();
      mqttNetPlain.setTimeout(1);
      mqttNetSecure.setTimeout(1);
      mqttSetupClient();
      lastMqttAttemptMs = 0;
      startOtaTask();
    }
    if (networkServicesStarted) ArduinoOTA.handle();

    server.handleClient();

    if (!otaInProgress) {
      mqttMaybeReconnect();
      if (mqtt.connected()) mqtt.loop();
    }

    if (!otaInProgress && (mqttRefreshPending || webRefreshPending)) {
      mqttRefreshPending = false;
      webRefreshPending  = false;
      downloadAndShowPhoto();
    }
  }

  board_loop();
  delay(10);
}
