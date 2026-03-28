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
#include <deque>

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

static char compileIdStr[16];
const char SF_COMPILE_ID[] __attribute__((used, section(".rodata"))) = "SFID:" __DATE__ " " __TIME__;

static SemaphoreHandle_t drawMutex = nullptr;
static unsigned long bootTimeMs = 0;

static volatile bool mqttRefreshPending = false;
static volatile bool webRefreshPending  = false;

static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;
static std::deque<String> logBuffer;

static void addLog(const String& msg) {
    String entry = "[" + String(millis() / 1000) + "s] " + msg;
    if (logBuffer.size() >= 50) logBuffer.pop_front();
    logBuffer.push_back(entry);
    DBGLN(entry);
}

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

static String installedFwToken    = "";
static String installedFwFilename = "";

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
static bool webServerStarted = false;

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
char lastUploadEtag[32] = "";

static TaskHandle_t otaTaskHandle  = nullptr;
static volatile bool otaInProgress = false;
static volatile bool photoTaskRunning = false;

// MQTT reconnect runs in its own task so mqtt.connect() can never block loop()
static volatile bool mqttTaskRunning = false;

// How many consecutive 5-second WiFi attempts before falling back to portal
static const uint8_t WIFI_MAX_ATTEMPTS = 6;  // ~30 seconds
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
  server.on("/hotspot-detect.html", HTTP_GET,  handleCaptiveRedirect);
  server.on("/generate_204",        HTTP_GET,  handleCaptiveRedirect);
  server.on("/gen_204",             HTTP_GET,  handleCaptiveRedirect);
  server.on("/ncsi.txt",            HTTP_GET,  handleCaptiveRedirect);
  server.on("/connecttest.txt",     HTTP_GET,  handleCaptiveRedirect);
  server.on("/redirect",            HTTP_GET,  handleCaptiveRedirect);
  server.on("/logpoll", HTTP_GET, []() {
    String json = "[";
    for (size_t i = 0; i < logBuffer.size(); i++) {
      String entry = logBuffer[i];
      entry.replace("\"", "'");
      json += "\"" + entry + "\"";
      if (i < logBuffer.size() - 1) json += ",";
    }
    json += "]";
    server.send(200, "application/json", json);
  });
  // Non-blocking async WiFi scan
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
    // server.clearAllHandlers() is not available in WebServer — handlers are
    // cleared automatically when the server is stopped and restarted.
    WiFi.softAPdisconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    portalActive           = false;
    webServerStarted       = false;
    networkServicesStarted = false;
    wifiEverConnected      = false;
    lastWifiAttemptMs      = 0;
    wifiAttemptCount       = 0;
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
      if (currentJpg && currentJpgLen)  board_draw_jpeg(currentJpg, currentJpgLen);
      else if (lastJpg && lastJpgLen)   board_draw_jpeg(lastJpg, lastJpgLen);
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

  bool shouldRotate = true;
  if (lastUploadEtag[0] != '\0') {
    String url = makePhotoUrl();
    WiFiClientSecure* sec = nullptr;
    WiFiClient* plain = nullptr;
    HTTPClient* h = new HTTPClient();
    if (url.startsWith("https://")) {
      sec = new WiFiClientSecure();
      sec->setTimeout(5);
      if (cfg.httpsInsecure) sec->setInsecure();
      h->begin(*sec, url);
    } else {
      plain = new WiFiClient();
      plain->setTimeout(5);
      h->begin(*plain, url);
    }
    if (cfg.httpUser.length() > 0)
      h->setAuthorization(cfg.httpUser.c_str(), cfg.httpPass.c_str());
    int headCode = h->sendRequest("HEAD");
    if (headCode == HTTP_CODE_OK) {
      String etag = h->header("ETag");
      if (etag.length() > 0) {
        etag.replace("\"", "");
        if (etag == lastUploadEtag) {
          shouldRotate = false;
          logEvent("PHOTO", "etag unchanged, no rotation");
        } else {
          strncpy(lastUploadEtag, etag.c_str(), sizeof(lastUploadEtag) - 1);
          lastUploadEtag[sizeof(lastUploadEtag) - 1] = '\0';
          logEvent("PHOTO", "etag changed to %s, rotating", lastUploadEtag);
        }
      }
    }
    h->end(); delete h; delete sec; delete plain;
  } else {
    strcpy(lastUploadEtag, "init");
  }

  if (!shouldRotate) {
    free(newBuf);
    lastDownloadOk = true;
    lastDownloadErr = "";
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

// Helper: spawn a photo download task (used for both first-boot and MQTT/web triggers)
static void spawnPhotoTask() {
  if (photoTaskRunning || otaInProgress) return;
  photoTaskRunning = true;
  xTaskCreate(
    [](void* param) {
      downloadAndShowPhoto();
      photoTaskRunning = false;
      vTaskDelete(NULL);
    },
    "photoTask", 16384, NULL, 1, NULL
  );
}

// ============================================================
// Background OTA update task (no core pin)
// ============================================================
static void otaUpdateTask(void* pv) {
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(5000));
  vTaskDelay(pdMS_TO_TICKS(30000));

  for (;;) {
    char     updateUrlSnap[256] = {};
    uint32_t intervalMinSnap    = DEFAULT_UPDATE_INTERVAL_MIN;
    strncpy(updateUrlSnap, cfg.updateUrl.c_str(), sizeof(updateUrlSnap) - 1);
    intervalMinSnap = cfg.updateIntervalMin;

    uint32_t intervalMs = intervalMinSnap * 60UL * 1000UL;
    if (intervalMs == 0) intervalMs = 3600000UL;

    if (updateUrlSnap[0] != '\0' && WiFi.status() == WL_CONNECTED && !otaInProgress) {
      logEvent("OTA", "checking %s compileId=%s", updateUrlSnap, compileIdStr);

      HTTPClient*       http  = new HTTPClient();
      WiFiClientSecure* sec   = nullptr;
      WiFiClient*       plain = nullptr;

      if (http) {
        http->setConnectTimeout(10000);
        http->setTimeout(60000);
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
          char macNaked[13] = {};
          int mi = 0;
          for (int i = 0; MAC_STR[i] && mi < 12; i++) {
            if (MAC_STR[i] != ':') macNaked[mi++] = MAC_STR[i];
          }
          char uptimeBuf[16];
          snprintf(uptimeBuf, sizeof(uptimeBuf), "%lu",
                   (unsigned long)((millis() - bootTimeMs) / 1000));
          http->addHeader("X-SF-Hostname", HOSTNAME);
          http->addHeader("X-SF-MAC",      macNaked);
          http->addHeader("X-SF-Compiled", compileIdStr);
          http->addHeader("X-SF-Uptime",   uptimeBuf);

          int code = http->GET();

          if (code == 204) {
            logEvent("OTA", "no update (204)");
            http->end(); delete http; delete sec; delete plain;
          } else if (code == HTTP_CODE_OK) {
            int32_t fwSize = http->getSize();
            if (fwSize <= 0) {
              logEvent("OTA", "200 but no Content-Length, aborting");
              http->end(); delete http; delete sec; delete plain;
            } else {
              WiFiClient* fwStream = http->getStreamPtr();
              if (!fwStream) {
                logEvent("OTA", "null fw stream");
                http->end(); delete http; delete sec; delete plain;
              } else {
                otaInProgress = true;
                logEvent("OTA", "starting flash size=%d", fwSize);
                bool flashed = false;
                if (xSemaphoreTake(drawMutex, portMAX_DELAY) == pdTRUE) {
                  if (Update.begin((size_t)fwSize)) {
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
                }
                http->end(); delete http; delete sec; delete plain;
                if (flashed) {
                  saveInstalledFw(compileIdStr, compileIdStr);
                  delay(1500);
                  ESP.restart();
                }
                otaInProgress = false;
              }
            }
          } else {
            logEvent("OTA", "unexpected HTTP code=%d", code);
            http->end(); delete http; delete sec; delete plain;
          }
        } else {
          logEvent("OTA", "http.begin failed");
          http->end(); delete http; delete sec; delete plain;
        }
      }
    }

    uint32_t elapsed = 0;
    while (elapsed < intervalMs) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      elapsed += 1000;
      if (cfg.updateIntervalMin * 60UL * 1000UL != intervalMs) break;
    }
  }
}

static void startOtaTask() {
  if (cfg.updateUrl.length() == 0) return;
  if (otaTaskHandle != nullptr) return;
  xTaskCreate(otaUpdateTask, "ota_check", 16384, nullptr, 1, &otaTaskHandle);
  logEvent("OTA", "task started interval=%umin url=%s compileId=%s",
           (unsigned)cfg.updateIntervalMin, cfg.updateUrl.c_str(), compileIdStr);
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

// MQTT reconnect task — runs entirely off the main loop so that a slow/failing
// TCP connect to an unreachable broker can never block server.handleClient().
static void mqttReconnectTask(void* pv) {
  // Take a snapshot of connection params so we don't read cfg mid-change
  char host[64]  = {};
  char user[64]  = {};
  char pass[64]  = {};
  char topic[64] = {};
  strncpy(host,  cfg.mqttHost.c_str(),  sizeof(host)  - 1);
  strncpy(user,  cfg.mqttUser.c_str(),  sizeof(user)  - 1);
  strncpy(pass,  cfg.mqttPass.c_str(),  sizeof(pass)  - 1);
  strncpy(topic, cfg.mqttTopic.c_str(), sizeof(topic) - 1);

  mqttNetPlain.setTimeout(2);
  mqttNetSecure.setTimeout(2);

  addLog(String("MQTT connecting to: ") + host + ":" + String(cfg.mqttPort));

  bool ok = (user[0] != '\0')
    ? mqtt.connect(HOSTNAME, user, pass)
    : mqtt.connect(HOSTNAME);

  if (ok) {
    mqttConnected = true;
    bool subOk = mqtt.subscribe(topic);
    addLog(String("MQTT connected! topic=") + topic + " sub=" + (subOk ? "ok" : "fail"));
    logEvent("MQTT", "connected sub=%s", subOk ? "ok" : "fail");
  } else {
    addLog(String("MQTT failed, rc=") + String(mqtt.state()));
    logEvent("MQTT", "connect failed rc=%d", mqtt.state());
  }

  mqttTaskRunning = false;
  vTaskDelete(NULL);
}

// Called from loop() — spawns a background task if it's time to try reconnecting.
// Returns immediately; never blocks.
static void mqttMaybeReconnect() {
  if (mqtt.connected()) { mqttConnected = true; return; }
  mqttConnected = false;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttemptMs < 15000) return;
  if (mqttTaskRunning) return;
  if (cfg.mqttHost.length() == 0) return;

  lastMqttAttemptMs = millis();
  mqttTaskRunning   = true;
  xTaskCreate(mqttReconnectTask, "mqttRecon", 8192, nullptr, 1, nullptr);
}

// ---------------------- Network services ----------------------
// Called once when WiFi first connects (or reconnects after portal).
// Idempotent - subsequent calls are no-ops.
static void startNetworkServicesOnce() {
  if (networkServicesStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  bool mdnsOk = MDNS.begin(HOSTNAME);
  ArduinoOTA.setHostname(HOSTNAME);
  if (ARDUINO_OTA_PASSWORD && strlen(ARDUINO_OTA_PASSWORD) > 0)
    ArduinoOTA.setPassword(ARDUINO_OTA_PASSWORD);
  ArduinoOTA.begin();

  // Start web server (guarded against double-start)
  if (!webServerStarted) {
    server.on("/",            HTTP_GET,  handleRoot);
    server.on("/config",      HTTP_GET,  handleConfigPage);
    server.on("/api/status",  HTTP_GET,  handleStatusJson);
    server.on("/api/log",     HTTP_GET,  handleLogJson);
    server.on("/api/config",  HTTP_POST, handlePostConfig);
    server.on("/api/refresh", HTTP_POST, handleActionRefresh);
    server.on("/img/current", HTTP_GET,  handleImgCurrent);
    server.on("/img/last",    HTTP_GET,  handleImgLast);
    server.begin();
    webServerStarted = true;
    logEvent("WEB", "server started");
  }

  mqttNetPlain.setTimeout(1);
  mqttNetSecure.setTimeout(1);
  mqttSetupClient();
  lastMqttAttemptMs = 0;

  networkServicesStarted = true;
  wifiEverConnected      = true;
  wifiAttemptCount       = 0;

  logEvent("WIFI", "connected ip=%s mac=%s", WiFi.localIP().toString().c_str(), MAC_STR);
  logEvent("NET",  "mdns=%s ota=on web=on", mdnsOk ? "on" : "off");

  String ipMac = "IP: " + WiFi.localIP().toString() + "   MAC: " + String(MAC_STR);
  board_draw_boot_status(ipMac.c_str());

  startOtaTask();

  // Kick off first photo download in a task - keeps main loop free
  spawnPhotoTask();
}

// ---------------------------------------------------------------------------
// ensureWifi - called every loop() iteration.
//
// State machine:
//   portalActive              -> run portal loop
//   WL_CONNECTED              -> startNetworkServicesOnce (idempotent)
//   not connected, cooldown   -> wait
//   not connected, cooldown elapsed, wifiEverConnected -> reconnect
//   not connected, cooldown elapsed, !wifiEverConnected, ssid set -> WiFi.begin()
//   too many failed attempts  -> start portal
//   no ssid                   -> start portal immediately
// ---------------------------------------------------------------------------
static void ensureWifi() {
  if (portalActive) { handlePortalLoop(); return; }
  if (WiFi.status() == WL_CONNECTED) return;  // services started in loop()
  if (lastWifiAttemptMs != 0 && (millis() - lastWifiAttemptMs) < 5000) return;

  lastWifiAttemptMs = millis();

  if (cfg.wifiSsid.length() == 0) {
    logEvent("WIFI", "no saved credentials");
    addLog("No saved credentials");
    startPortalMode();
    return;
  }

  if (wifiEverConnected) {
    logEvent("WIFI", "reconnect attempt");
    addLog("Reconnecting...");
    WiFi.reconnect();
    return;
  }

  // Count attempts; fall back to portal after WIFI_MAX_ATTEMPTS failures
  wifiAttemptCount++;
  if (wifiAttemptCount > WIFI_MAX_ATTEMPTS) {
    logEvent("WIFI", "giving up after %u attempts, starting portal", (unsigned)wifiAttemptCount);
    addLog("Could not connect. Starting Wi-Fi setup...");
    startPortalMode();
    return;
  }

  applyWifiDefaults();
  logEvent("WIFI", "connect attempt %u/%u ssid=%s",
           (unsigned)wifiAttemptCount, (unsigned)WIFI_MAX_ATTEMPTS, cfg.wifiSsid.c_str());
  addLog("Connecting to: " + cfg.wifiSsid + " (" + String(wifiAttemptCount) + "/" + String(WIFI_MAX_ATTEMPTS) + ")");
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
  j += "\"wifi\":";       j += (wifiOk ? "true" : "false"); j += ",";
  j += "\"wifiEverConnected\":"; j += (wifiEverConnected ? "true" : "false"); j += ",";
  j += "\"ip\":\""; appendJsonEscaped(j, wifiOk ? WiFi.localIP().toString() : String("")); j += "\",";
  j += "\"mdns\":";  j += (networkServicesStarted ? "true" : "false"); j += ",";
  j += "\"ota\":";   j += (networkServicesStarted ? "true" : "false"); j += ",";
  j += "\"mqtt\":";  j += (mqttConnected ? "true" : "false"); j += ",";
  j += "\"lastDownloadOk\":";  j += (lastDownloadOk ? "true" : "false"); j += ",";
  j += "\"lastDownloadErr\":\""; appendJsonEscaped(j, lastDownloadErr); j += "\",";
  j += "\"lastDownloadMs\":"; j += String(lastDownloadMs); j += ",";
  j += "\"lastMqttMsgMs\":";  j += String(lastMqttMsgMs); j += ",";
  j += "\"lastLogSeq\":";     j += String(logSeq); j += ",";
  j += "\"otaInProgress\":";  j += (otaInProgress ? "true" : "false"); j += ",";
  j += "\"installedFw\":\"";  appendJsonEscaped(j, installedFwFilename); j += "\",";
  j += "\"compiledId\":\"";   appendJsonEscaped(j, compileIdStr); j += "\",";
  j += "\"installedFwId\":\""; appendJsonEscaped(j, installedFwToken); j += "\",";
  j += "\"uptimeMs\":";       j += String(uptimeMs); j += ",";
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
  if (server.hasArg("mqttHost"))           cfg.mqttHost           = server.arg("mqttHost");
  if (server.hasArg("mqttTopic"))          cfg.mqttTopic          = server.arg("mqttTopic");
  if (server.hasArg("mqttUser"))           cfg.mqttUser           = server.arg("mqttUser");
  if (server.hasArg("mqttPort"))           cfg.mqttPort           = (uint16_t)server.arg("mqttPort").toInt();
  if (server.hasArg("updateUrl"))          cfg.updateUrl          = server.arg("updateUrl");
  if (server.hasArg("updateIntervalMin"))  cfg.updateIntervalMin  = (uint32_t)server.arg("updateIntervalMin").toInt();
  if (server.hasArg("webUser") && server.arg("webUser").length() > 0)
    cfg.webUser = server.arg("webUser");
  if (server.hasArg("httpPass") && isRealPassword(server.arg("httpPass"))) cfg.httpPass = server.arg("httpPass");
  if (server.hasArg("mqttPass") && isRealPassword(server.arg("mqttPass"))) cfg.mqttPass = server.arg("mqttPass");
  if (server.hasArg("webPass")  && isRealPassword(server.arg("webPass")))  cfg.webPass  = server.arg("webPass");
  if (server.hasArg("webPassClear") && server.arg("webPassClear") == "1")  cfg.webPass  = "";
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
  board_init();

  board_draw_jpeg(splash_logo, splash_logo_len);
  board_draw_boot_status("Connecting to Wi-Fi...");

  // Start WiFi; everything else happens in loop() once connected.
  applyWifiDefaults();
  if (cfg.wifiSsid.length() > 0) {
    logEvent("WIFI", "connect begin ssid=%s", cfg.wifiSsid.c_str());
    addLog("Connecting to: " + cfg.wifiSsid);
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    wifiAttemptCount = 1;  // this counts as attempt #1
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
      server.handleClient();

      if (!otaInProgress) {
        mqttMaybeReconnect();
        if (mqtt.connected()) mqtt.loop();
      }

      if (!otaInProgress && (mqttRefreshPending || webRefreshPending)) {
        mqttRefreshPending = false;
        webRefreshPending  = false;
        spawnPhotoTask();
      }
    }
  }

  board_loop();
  delay(10);
}
