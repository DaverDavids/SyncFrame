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
#include <mbedtls/base64.h>
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
  const char* ARDUINO_OTA_PASSWORD = "";
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
static bool portalDone   = false;
static unsigned long portalStartMs = 0;
static const uint32_t PORTAL_TIMEOUT_MS = 180000;

Preferences prefs;
struct Config {
  String wifiSsid;
  String wifiPass;
  String photoBaseUrl;
  String photoFilename;
  bool   httpsInsecure;
  String httpUser;
  String httpPass;
  String webUser;
  String webPass;
  int    streamReconnectMin;
  int    peekButtonPin;
} cfg;

static String installedFwToken    = "";
static String installedFwFilename = "";

static const char* PREF_NS = "syncframe";
static const char* DEFAULT_PHOTO_BASEURL       = "https://192.168.6.202:8369/syncframe/";
static const char* DEFAULT_HTTP_USER           = "david";
static const char* DEFAULT_WEB_USER            = "admin";
static const char* DEFAULT_WEB_PASS            = "";

WebServer server(80);

bool networkServicesStarted = false;
static bool webServerStarted = false;

unsigned long lastWifiAttemptMs  = 0;
bool wifiEverConnected  = false;

// Note: currentJpg/lastJpg no longer used for streaming, but kept for /img endpoints
uint8_t* currentJpg    = nullptr;
size_t   currentJpgLen = 0;
uint8_t* lastJpg       = nullptr;
size_t   lastJpgLen    = 0;
bool showingLast        = false;

// Stream (mjpeg)
static WiFiClientSecure* streamClient     = nullptr;
static bool              mjpegConnected   = false;
static unsigned long     lastMjpegConnectMs = 0;
static unsigned long     lastMjpegAttemptMs = 0;
static bool              mjpegForceReconnect = false;
static volatile bool    mjpegRequestRefresh = false;
static char              currentPhotoEtag[24] = "";

static const uint8_t WIFI_MAX_ATTEMPTS = 6;
static uint8_t wifiAttemptCount = 0;

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
// Forward declarations
// ============================================================
static void handleRoot();
static void handleConfigPage();
static void handleStatusJson();
static void handleLogJson();
static void handlePostConfig();
static void handleImgCurrent();
static void handleImgLast();
static void handleActionRefresh();
static void handleActionReboot();

// ============================================================
// Web Authentication
// ============================================================
static bool requireWebAuth() {
  bool imgEndpoint = false;
  if (server.uri().startsWith("/img/")) imgEndpoint = true;
  int threshold = imgEndpoint ? 10000 : 20000;
  if (ESP.getFreeHeap() < threshold) {
    server.send(503, "application/json", "{\"ok\":false,\"err\":\"low memory\"}");
    return false;
  }
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
      boardDrawActive = false;
      xSemaphoreGive(drawMutex);
    }
  }
}

void showLastPhoto() {
  if (lastJpg && lastJpgLen) {
    showingLast = true;
    if (xSemaphoreTake(drawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      board_draw_jpeg(lastJpg, lastJpgLen);
      boardDrawActive = false;
      xSemaphoreGive(drawMutex);
    }
  }
}

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

static void buildCompileId() {
  static const char* months[12] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
  };
  const char* dateStr = __DATE__;
  const char* timeStr = __TIME__;
  char monthBuf[4] = {};
  strncpy(monthBuf, dateStr, 3);
  int monthNum = 0;
  for (int i = 0; i < 12; i++) {
    if (strcmp(monthBuf, months[i]) == 0) { monthNum = i + 1; break; }
  }
  int day = (dateStr[4] == ' ') ? (dateStr[5] - '0') :
            ((dateStr[4] - '0') * 10 + (dateStr[5] - '0'));
  char yearBuf[5] = {};
  strncpy(yearBuf, dateStr + 7, 4);
  char timeBuf[7] = {};
  int ti = 0;
  for (int i = 0; timeStr[i] && ti < 6; i++) {
    if (timeStr[i] != ':') timeBuf[ti++] = timeStr[i];
  }
  snprintf(compileIdStr, sizeof(compileIdStr), "%s%02d%02d-%s",
           yearBuf, monthNum, day, timeBuf);
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
static void appendJsonEscaped(String& out, const String& s) { appendJsonEscaped(out, s.c_str()); }
static void appendJsonPassword(String& out, const String& pass) {
  if (pass.length() > 0) appendJsonEscaped(out, String("********"));
}

static void freeBuf(uint8_t*& p, size_t& n) {
  if (p) { free(p); p = nullptr; }
  n = 0;
}

static uint32_t crc32buf(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
  }
  return ~crc;
}

static void applyWifiDefaults() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(false);
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
  cfg.webUser           = prefs.getString("wbuser",  DEFAULT_WEB_USER);
  cfg.webPass           = prefs.getString("wbpass",  DEFAULT_WEB_PASS);
  cfg.streamReconnectMin = prefs.getInt("sreconn",   10);
  cfg.peekButtonPin      = prefs.getInt("peekpin",   -1);
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
  prefs.putString("wbuser", cfg.webUser);
  prefs.putString("wbpass", cfg.webPass);
  prefs.putInt("sreconn",   cfg.streamReconnectMin);
  prefs.putInt("peekpin",   cfg.peekButtonPin);
  prefs.end();
}

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
  <label for="ssid">WiFi SSID</label>
  <div style="display:grid;grid-template-columns:1fr 36px;gap:6px;align-items:stretch;">
    <input type="text" id="ssid" name="ssid" placeholder="Network name" required
      style="width:100%;margin:0;">
    <button type="button" id="scanBtn" onclick="doScan()"
      title="Scan for networks"
      style="width:36px;height:100%;padding:0;margin:0;font-size:13px;
            background:#444;color:#fff;border:1px solid #666;border-radius:4px;
            cursor:pointer;display:flex;align-items:center;justify-content:center;">
      &#x1F50D;
    </button>
  </div>
  <select id="ssidPick" onchange="document.getElementById('ssid').value=this.value"
    style="display:none;width:100%;margin-top:4px;">
  </select>
  <label style="margin-top:12px">Password</label>
  <input type='password' name='pass' placeholder='Wi-Fi password'>
  <button type='submit'>Connect</button>
 </form>
 <p style="margin-top:16px;text-align:center;font-size:13px;">
  <a href="/config" style="color:#4af;">&#x1F4CB; Config & Log</a>
 </p>
 <p class='note'>Device: HOSTNAME_PLACEHOLDER</p>
</div>
<script>
function doScan(){
  var btn=document.getElementById('scanBtn');
  var sel=document.getElementById('ssidPick');
  btn.textContent='...';
  btn.disabled=true;
  sel.style.display='none';
  fetch('/scan').then(function(r){return r.json();}).then(function(nets){
    if(nets.scanning){ setTimeout(doScan,1500); return; }
    sel.innerHTML='<option value="">-- select scanned network --</option>';
    nets.forEach(function(n){
      var o=document.createElement('option');
      o.value=n.ssid;
      o.textContent=n.ssid+(n.enc?' \uD83D\uDD12':'')+' ('+n.rssi+'dBm)';
      sel.appendChild(o);
    });
    sel.style.display='block';
    btn.textContent='\uD83D\uDD0D';
    btn.disabled=false;
  }).catch(function(){
    btn.textContent='\uD83D\uDD0D';
    btn.disabled=false;
  });
}
</script></body></html>
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
  server.on("/hotspot-detect.html", HTTP_GET, []() {
    server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  });
  server.on("/library/test/success.html", HTTP_GET, []() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  });
  server.on("/generate_204",        HTTP_GET,  handleCaptiveRedirect);
  server.on("/gen_204",             HTTP_GET,  handleCaptiveRedirect);
  server.on("/ncsi.txt",            HTTP_GET,  handleCaptiveRedirect);
  server.on("/connecttest.txt",     HTTP_GET,  handleCaptiveRedirect);
  server.on("/redirect",            HTTP_GET,  handleCaptiveRedirect);
  server.on("/scan", HTTP_GET, []() {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      server.send(200, "application/json", "{\"scanning\":true}");
      return;
    }
    if (n == WIFI_SCAN_FAILED || n < 0) {
      WiFi.scanNetworks(/*async=*/true);
      server.send(200, "application/json", "{\"scanning\":true}");
      return;
    }
    String json = "[";
    for (int i = 0; i < n; i++) {
      if (i > 0) json += ",";
      String ssid = WiFi.SSID(i);
      ssid.replace("\"", "\\\"");
      json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
              ",\"enc\":" + (WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    json += "]";
    WiFi.scanDelete();
    server.send(200, "application/json", json);
  });
  server.on("/config", HTTP_GET, handleConfigPage);
  setupCoredumpRoute();
  server.onNotFound(handleCaptiveRedirect);
}

static void startPortalMode() {
  if (portalActive) return;
  String apName = String(HOSTNAME) + "-setup";
  logEvent("PORTAL", "starting AP %s", apName.c_str());
  board_draw_boot_status(("Wi-Fi Setup: " + apName).c_str());

  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str());
  delay(100);

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());

  setupPortalRoutes();
  server.begin();
  webServerStarted = true;

  ArduinoOTA.setHostname(HOSTNAME);
  if (ARDUINO_OTA_PASSWORD && strlen(ARDUINO_OTA_PASSWORD) > 0)
    ArduinoOTA.setPassword(ARDUINO_OTA_PASSWORD);
  ArduinoOTA.begin();

  portalActive  = true;
  portalDone    = false;
  portalStartMs = millis();
  wifiAttemptCount = 0;
  logEvent("PORTAL", "active ip=%s", WiFi.softAPIP().toString().c_str());
}

static void handlePortalLoop() {
  if (!portalActive) return;
  dnsServer.processNextRequest();
  server.handleClient();
  ArduinoOTA.handle();

  bool timedOut = (millis() - portalStartMs) > PORTAL_TIMEOUT_MS;
  if (portalDone || timedOut) {
    logEvent("PORTAL", "stopping done=%d timeout=%d", (int)portalDone, (int)timedOut);
    dnsServer.stop();
    server.stop();
    WiFi.softAPdisconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    portalActive           = false;
    webServerStarted       = false;
    networkServicesStarted = false;
    lastWifiAttemptMs      = 0;
    wifiAttemptCount       = 0;
  }
}

// Returns true if this build has PSRAM available at runtime.
static inline bool hasPsram() {
#if defined(BOARD_HAS_PSRAM)
  return (ESP.getPsramSize() > 0);
#else
  return false;
#endif
}

// ---------------------- MJPEG Stream ----------------------
static void mjpegTask(void* pv) {
  (void)pv;
  String url = cfg.photoBaseUrl;
  if (!url.startsWith("http://") && !url.startsWith("https://")) {
    url = "https://" + url;
  }
  if (!url.endsWith("/")) url += "/";
  url += "stream";

  bool isHttps = url.startsWith("https://");
  int port = isHttps ? 443 : 80;
  String host;
  int slashPos = url.indexOf("/", 8);
  if (slashPos > 0) {
    String hostPart = url.substring(8, slashPos);
    if (hostPart.indexOf(":") > 0) {
      int colonPos = hostPart.indexOf(":");
      host = hostPart.substring(0, colonPos);
      port = hostPart.substring(colonPos + 1).toInt();
    } else {
      host = hostPart;
    }
  } else {
    host = url.substring(url.indexOf("//") + 2);
  }

  // Guard against bad parse
  if (host.length() == 0 || host.startsWith("/")) {
    logEvent("STREAM", "bad host parse: %s", host.c_str());
    mjpegConnected = false;
    vTaskDelete(NULL);
    return;
  }

  String path = (slashPos > 0) ? url.substring(slashPos) : "/";

  WiFiClient* client = streamClient;
  if (!client) {
    client = new WiFiClientSecure();
    streamClient = (WiFiClientSecure*)client;
  }
  if (cfg.httpsInsecure) ((WiFiClientSecure*)client)->setInsecure();

  if (!client->connect(host.c_str(), port)) {
    logEvent("STREAM", "connect failed %s:%d", host.c_str(), port);
    delete client;
    streamClient = nullptr;
    mjpegConnected = false;
    lastMjpegAttemptMs = millis();
    vTaskDelete(NULL);
    return;
  }

  // Build MAC without colons
  char macNaked[13] = {};
  int mi = 0;
  for (int i = 0; MAC_STR[i] && mi < 12; i++) {
    if (MAC_STR[i] != ':') macNaked[mi++] = MAC_STR[i];
  }

  client->print("GET " + path + " HTTP/1.1\r\n");
  client->print("Host: " + host + "\r\n");
  client->print("User-Agent: SyncFrame/1.0\r\n");
  client->print("X-SF-Hostname: " + String(HOSTNAME) + "\r\n");
  client->print("X-SF-MAC: " + String(macNaked) + "\r\n");
  client->print("X-SF-Uptime: " + String((unsigned long)(millis() / 1000)) + "\r\n");
  client->print("X-SF-Compiled: " + String(compileIdStr) + "\r\n");
  client->print("X-SF-Photo-Etag: " + String(currentPhotoEtag) + "\r\n");
  client->print("X-SF-Resolution: " + String(SCREEN_W) + "x" + String(SCREEN_H) + "\r\n");
  if (cfg.httpUser.length() > 0) {
    String auth = cfg.httpUser + ":" + cfg.httpPass;
    size_t len = auth.length();
    size_t b64BufSize = ((len + 2) / 3) * 4 + 1;
    uint8_t* buf = (uint8_t*)malloc(b64BufSize);
    if (buf) {
      size_t outLen = 0;
      mbedtls_base64_encode(buf, b64BufSize, &outLen, (const uint8_t*)auth.c_str(), len);
      client->print("Authorization: Basic ");
      client->write(buf, outLen);
      client->print("\r\n");
      free(buf);
    }
  }
  client->print("Accept: multipart/x-mixed-replace\r\n");
  client->print("Connection: keep-alive\r\n");
    client->print("\r\n");

    String statusLine = client->readStringUntil('\n');
    if (!statusLine.startsWith("HTTP/1.1 200")) {
      logEvent("STREAM", "status %s", statusLine.c_str());
      client->stop();
      mjpegConnected = false;
      vTaskDelete(NULL);
      return;
    }

    unsigned long lastDataMs = 0;

    // Drain HTTP response headers — wait for data, don't bail early
    while (client->connected()) {
      while (!client->available()) {
        if (!client->connected()) goto stream_done;
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      String line = client->readStringUntil('\n');
      line.trim();
      if (line.length() == 0) break;
    }

    logEvent("STREAM", "connected");
    lastMjpegConnectMs = millis();
    lastDataMs = millis();
  	
    while (client->connected() || client->available()) {
      if (mjpegRequestRefresh) {
        mjpegRequestRefresh = false;
        logEvent("STREAM", "refresh requested, reconnecting");
        break;
      }
      if (millis() - lastDataMs > 90000) {
        logEvent("STREAM", "idle timeout");
        break;
      }

      // Wait for boundary line
      String boundary = "";
      while (client->connected()) {
        if (!client->available()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        boundary = client->readStringUntil('\n');
        break;
      }
      if (!boundary.startsWith("--frame")) {
        if (!client->connected()) break;
        continue;
      }

      // Drain frame headers
      int contentLength = 0;
      String frameType;
      while (client->connected()) {
        while (!client->available()) {
          if (!client->connected()) goto stream_done;
          vTaskDelay(pdMS_TO_TICKS(5));
        }
        String line = client->readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
        if (line.startsWith("Content-Length:")) {
          String val = line.substring(15); val.trim();
          contentLength = val.toInt();
        } else if (line.startsWith("X-SF-Frame-Type:")) {
          String val = line.substring(16); val.trim();
          frameType = val;
        } else if (line.startsWith("X-SF-Etag:")) {
          String val = line.substring(10); val.trim();
          if (val.length() > 0 && val.length() < sizeof(currentPhotoEtag)) {
            strncpy(currentPhotoEtag, val.c_str(), sizeof(currentPhotoEtag) - 1);
            currentPhotoEtag[sizeof(currentPhotoEtag) - 1] = '\0';
          }
        }
      }

      if (contentLength == 0) { lastDataMs = millis(); continue; }

      if (frameType == "ota") {
        logEvent("STREAM", "OTA frame size=%d", contentLength);
        if (Update.begin(contentLength)) {
          size_t written = Update.writeStream(*client);
          if (Update.end() && Update.isFinished()) {
            logEvent("STREAM", "OTA flash ok");
            delay(500);
            ESP.restart();
          }
        }
        break;
      }

      uint8_t* buf = nullptr;
      size_t allocSize = contentLength;
      static const size_t C3_MAX_PREALLOC = 20480;
      if (!hasPsram() && contentLength > C3_MAX_PREALLOC) {
        logEvent("STREAM", "frame too large %d, draining", contentLength);
        size_t drain = contentLength;
        while (drain > 0 && client->connected()) {
          size_t avail = client->available();
          if (avail == 0) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
          size_t toRead = (drain < avail) ? drain : avail;
          uint8_t tmp[256];
          client->read(tmp, toRead);
          drain -= toRead;
        }
        lastDataMs = millis();
        continue;
      }

      if (hasPsram()) {
        buf = (uint8_t*)heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (uint8_t*)malloc(allocSize);
      } else {
        // Free current copy to reclaim heap before allocating frame buffer
        if (xSemaphoreTake(drawMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
          freeBuf(currentJpg, currentJpgLen);
          xSemaphoreGive(drawMutex);
        }
        if (allocSize > MAX_JPG) allocSize = MAX_JPG;
        buf = (uint8_t*)malloc(allocSize);
      }

      if (!buf) {
        logEvent("STREAM", "alloc failed %d", contentLength);
        break;
      }

      size_t remaining = contentLength;
      size_t readTotal = 0;
      while (remaining > 0 && client->connected()) {
        size_t avail = client->available();
        if (avail == 0) {
          vTaskDelay(pdMS_TO_TICKS(1));
          continue;
        }
        size_t toRead = (remaining < avail) ? remaining : avail;
        int n = client->read(buf + readTotal, toRead);
        if (n <= 0) break;
        readTotal += n;
        remaining -= n;
      }

      if (readTotal == contentLength) {
        bool changed = true;
        lastDataMs = millis();

        // Promote current → last before overwriting (only on new content)
        if (xSemaphoreTake(drawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          if (changed && currentJpg && currentJpgLen) {
            freeBuf(lastJpg, lastJpgLen);
            lastJpg    = currentJpg;
            lastJpgLen = currentJpgLen;
            currentJpg    = nullptr;
            currentJpgLen = 0;
          } else {
            freeBuf(currentJpg, currentJpgLen);
          }
          currentJpg    = buf;
          currentJpgLen = readTotal;
          buf           = nullptr;
          if (!showingLast) {
            board_draw_jpeg(currentJpg, currentJpgLen);
            boardDrawActive = false;
          }
          xSemaphoreGive(drawMutex);
        }
        logEvent("STREAM", "frame size=%u etag=%s %s heap=%u",
          (unsigned)readTotal, currentPhotoEtag,
          changed ? "NEW" : "same",
          (unsigned)ESP.getFreeHeap());
      } else {
        logEvent("STREAM", "frame INCOMPLETE read=%u expected=%u", (unsigned)readTotal, (unsigned)contentLength);
        break;
      }

      if (!client->connected()) break;
    }

    stream_done:
    client->stop();

    mjpegConnected = false;
    lastMjpegAttemptMs = 0;
    logEvent("STREAM", "task ended");
    vTaskDelete(NULL);
}

static void mjpegMaybeReconnect() {
  if (mjpegConnected) {
    if (mjpegForceReconnect ||
        millis() - lastMjpegConnectMs >= (unsigned long)cfg.streamReconnectMin * 60000UL) {
      mjpegRequestRefresh = true;
      lastMjpegAttemptMs = 0;
    }
    return;
  }
  if (mjpegForceReconnect) {
    mjpegForceReconnect = false;
    lastMjpegAttemptMs = 0;
  }
  if (WiFi.status() != WL_CONNECTED) return;
  if (cfg.photoBaseUrl.length() == 0) return;
  if (millis() - lastMjpegAttemptMs < 15000) return;

  mjpegRequestRefresh = false;  // clear flag before spawning new task
  mjpegConnected      = true;
  lastMjpegConnectMs = millis();
  lastMjpegAttemptMs = millis();
  BaseType_t taskCreated = xTaskCreatePinnedToCore(mjpegTask, "mjpegTask", 16384, nullptr, 1, nullptr, APP_CORE);
  if (taskCreated != pdPASS) {
    mjpegConnected = false;
    logEvent("STREAM", "task create failed");
  }
}

// ---------------------- Network services ----------------------
static void startNetworkServicesOnce() {
  if (networkServicesStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  bool mdnsOk = MDNS.begin(HOSTNAME);
  ArduinoOTA.setHostname(HOSTNAME);
  if (ARDUINO_OTA_PASSWORD && strlen(ARDUINO_OTA_PASSWORD) > 0)
    ArduinoOTA.setPassword(ARDUINO_OTA_PASSWORD);
  ArduinoOTA.begin();

  if (!webServerStarted) {
    server.on("/",            HTTP_GET,  handleRoot);
    server.on("/config",      HTTP_GET,  handleConfigPage);
    server.on("/api/status",  HTTP_GET,  handleStatusJson);
    server.on("/api/log",     HTTP_GET,  handleLogJson);
    server.on("/api/config",  HTTP_POST, handlePostConfig);
    server.on("/api/refresh", HTTP_POST, handleActionRefresh);
    server.on("/api/reboot",  HTTP_POST, handleActionReboot);
    server.on("/api/reboot",  HTTP_GET,  handleActionReboot);
    server.on("/img/current", HTTP_GET,  handleImgCurrent);
    server.on("/img/last",    HTTP_GET,  handleImgLast);
    setupCoredumpRoute();
    server.on("/panic-test", HTTP_GET, []() {
      server.send(200, "text/plain", "Crashing in 1s...");
      delay(1000);
      abort();
    });
    server.begin();
    webServerStarted = true;
    logEvent("WEB", "server STARTED");
  }

  networkServicesStarted = true;
  wifiEverConnected      = true;
  wifiAttemptCount       = 0;

  logEvent("WIFI", "connected ip=%s mac=%s", WiFi.localIP().toString().c_str(), MAC_STR);
  logEvent("NET",  "mdns=%s ota=on web=on psram=%s heapFree=%u",
           mdnsOk ? "on" : "off", hasPsram() ? "yes" : "no", (unsigned)ESP.getFreeHeap());

  board_draw_boot_status(("IP: " + WiFi.localIP().toString() + "   MAC: " + String(MAC_STR)).c_str());
}

static void ensureWifi() {
  if (portalActive) { handlePortalLoop(); return; }
  if (WiFi.status() == WL_CONNECTED) return;
  if (lastWifiAttemptMs != 0 && (millis() - lastWifiAttemptMs) < 5000) return;

  lastWifiAttemptMs = millis();

  if (cfg.wifiSsid.length() == 0) {
    logEvent("WIFI", "no saved credentials");
    startPortalMode();
    return;
  }

  if (wifiEverConnected) {
    logEvent("WIFI", "reconnect attempt");
    WiFi.reconnect();
    return;
  }

  wifiAttemptCount++;
  if (wifiAttemptCount > WIFI_MAX_ATTEMPTS) {
    logEvent("WIFI", "giving up after %u attempts, starting portal", (unsigned)wifiAttemptCount);
    startPortalMode();
    return;
  }

  applyWifiDefaults();
  logEvent("WIFI", "connect attempt %u/%u ssid=%s",
           (unsigned)wifiAttemptCount, (unsigned)WIFI_MAX_ATTEMPTS, cfg.wifiSsid.c_str());
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
}

// ---------------------- Web UI handlers ----------------------
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
  j += "\"webUser\":\"";        appendJsonEscaped(j, cfg.webUser);       j += "\",";
  j += "\"webPass\":\"";        appendJsonPassword(j, cfg.webPass);      j += "\",";
  j += "\"streamReconnectMin\":"; j += String(cfg.streamReconnectMin); j += ",";
  j += "\"peekButtonPin\":"; j += String(cfg.peekButtonPin);
  j += "}";

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
  j += "\"wifi\":";       j += (wifiOk ? "true" : "false"); j += ",";
  j += "\"wifiEverConnected\":"; j += (wifiEverConnected ? "true" : "false"); j += ",";
  j += "\"ip\":\""; appendJsonEscaped(j, wifiOk ? WiFi.localIP().toString() : String("")); j += "\",";
  j += "\"mdns\":";  j += (networkServicesStarted ? "true" : "false"); j += ",";
  j += "\"ota\":";   j += (networkServicesStarted ? "true" : "false"); j += ",";
  j += "\"mjpeg\":";       j += (mjpegConnected ? "true" : "false"); j += ",";
  j += "\"lastMjpegConnectMs\":"; j += String(lastMjpegConnectMs); j += ",";
  j += "\"photoHash\":\""; appendJsonEscaped(j, currentPhotoEtag); j += "\",";
  j += "\"lastLogSeq\":";     j += String(logSeq); j += ",";
  j += "\"installedFw\":\"";  appendJsonEscaped(j, installedFwFilename); j += "\",";
  j += "\"compiledId\":\"";   appendJsonEscaped(j, compileIdStr); j += "\",";
  j += "\"installedFwId\":\""; appendJsonEscaped(j, installedFwToken); j += "\",";
  j += "\"uptimeMs\":";       j += String(uptimeMs); j += ",";
  j += "\"screenW\":"; j += String(SCREEN_W); j += ",";
  j += "\"screenH\":"; j += String(SCREEN_H); j += ",";
  j += "\"psram\":";   j += (hasPsram() ? "true" : "false"); j += ",";
  j += "\"heapFree\":"; j += String(ESP.getFreeHeap()); j += ",";
  j += "\"streamReconnectMin\":"; j += String(cfg.streamReconnectMin); j += ",";
  j += "\"peekButtonPin\":"; j += String(cfg.peekButtonPin);
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
  uint8_t skip  = (since == 0 && snapCount > maxItems) ? snapCount - maxItems : 0;

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
    j += "{\"seq\":" + String(e.seq) + ",\"ms\":" + String(e.ms);
    j += ",\"tag\":\""; appendJsonEscaped(j, e.tag); j += "\",";
    j += "\"msg\":\"";  appendJsonEscaped(j, e.msg); j += "\"}";
    sent++;
  }
  j += "],\"nextSince\":" + String(snapSeq) + "}";
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
  if (server.hasArg("webUser") && server.arg("webUser").length() > 0)
    cfg.webUser = server.arg("webUser");
  if (server.hasArg("httpPass") && isRealPassword(server.arg("httpPass"))) cfg.httpPass = server.arg("httpPass");
  if (server.hasArg("webPass")  && isRealPassword(server.arg("webPass")))  cfg.webPass  = server.arg("webPass");
  if (server.hasArg("webPassClear") && server.arg("webPassClear") == "1")  cfg.webPass  = "";
  if (server.hasArg("streamReconnectMin")) {
    int v = server.arg("streamReconnectMin").toInt();
    if (v >= 1 && v <= 1440) cfg.streamReconnectMin = v;
  }
  if (server.hasArg("peekButtonPin")) {
    cfg.peekButtonPin = server.arg("peekButtonPin").toInt();
  }
  cfg.httpsInsecure   = server.hasArg("httpsInsecure");
  saveConfig();
  if (cfg.peekButtonPin >= 0) pinMode(cfg.peekButtonPin, INPUT_PULLUP);
  mjpegForceReconnect = true;
  logEvent("WEB", "config saved");
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleImgCurrent() {
  if (!requireWebAuth()) return;
  uint8_t* copy = nullptr; size_t copyLen = 0;
  if (xSemaphoreTake(drawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (currentJpg && currentJpgLen) {
      copy = (uint8_t*)malloc(currentJpgLen);
      if (copy) { memcpy(copy, currentJpg, currentJpgLen); copyLen = currentJpgLen; }
    }
    xSemaphoreGive(drawMutex);
  }
  if (!copy) { server.send(404, "text/plain", "no image"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", (const char*)copy, copyLen);
  free(copy);
}

static void handleImgLast() {
  if (!requireWebAuth()) return;
  uint8_t* copy = nullptr; size_t copyLen = 0;
  if (xSemaphoreTake(drawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (lastJpg && lastJpgLen) {
      copy = (uint8_t*)malloc(lastJpgLen);
      if (copy) { memcpy(copy, lastJpg, lastJpgLen); copyLen = lastJpgLen; }
    }
    xSemaphoreGive(drawMutex);
  }
  if (!copy) { server.send(404, "text/plain", "no last image"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", (const char*)copy, copyLen);
  free(copy);
}

static void handleActionRefresh() {
  if (!requireWebAuth()) return;
  logEvent("WEB", "manual refresh requested");
  mjpegForceReconnect = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleActionReboot() {
  if (!requireWebAuth()) return;
  logEvent("WEB", "reboot requested");
  server.send(200, "application/json", "{\"ok\":true}");
  delay(500);
  ESP.restart();
}

// ---------------------- Main ----------------------
void setup() {
  DBG_BEGIN(115200);
  delay(50);
  bootTimeMs = millis();
  buildHostAndClientId();
  buildCompileId();
  logEvent("BOOT", "%s mac=%s reset=%s compileId=%s",
           HOSTNAME, MAC_STR, resetReasonToStr(esp_reset_reason()), compileIdStr);

  drawMutex = xSemaphoreCreateBinary();
  xSemaphoreGive(drawMutex);

  loadConfig();
  if (cfg.peekButtonPin >= 0) pinMode(cfg.peekButtonPin, INPUT_PULLUP);
  board_init();

  board_draw_jpeg(splash_logo, splash_logo_len);
  board_draw_boot_status("Connecting to Wi-Fi...");

  applyWifiDefaults();
  if (cfg.wifiSsid.length() > 0) {
    logEvent("WIFI", "connect begin ssid=%s", cfg.wifiSsid.c_str());
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    wifiAttemptCount = 1;
  } else {
    logEvent("WIFI", "no saved credentials - starting portal");
    startPortalMode();
  }
}

void loop() {
  ensureWifi();

  if (WiFi.status() == WL_CONNECTED) {
    startNetworkServicesOnce();

    if (networkServicesStarted) {
      ArduinoOTA.handle();
      if (ESP.getFreeHeap() > 20000) server.handleClient();

      mjpegMaybeReconnect();
    }
  }

  board_loop(cfg.peekButtonPin);
  delay(1);
}
