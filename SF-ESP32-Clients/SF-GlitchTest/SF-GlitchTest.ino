/*
 * SF_GlitchTest.ino
 *
 * Standalone ESP32-S3 RGB panel glitch tester.
 *
 * Features:
 *  - Generated test JPEG (gradient pattern, no file needed to get started)
 *  - Web UI: upload a real JPEG from browser -> replaces the generated one
 *  - Web UI: WiFi credentials form (saved to Preferences, survives reboot)
 *  - ArduinoOTA support (flash over WiFi once connected)
 *  - Secrets.h support: if <Secrets.h> exists and defines MYSSID/MYPSK,
 *    those are used as the initial WiFi credentials
 *  - Falls back to AP mode ("SF-GlitchTest") if no credentials saved
 *
 * TESTS
 *  T1  BASELINE        loop() only, GFX stripe primitives, no JPEG, no tasks.
 *  T2  TASK_DRAW       JPEG drawn from a FreeRTOS task (prio 0).
 *  T3  WIFI_BURST      loop() draws while bgTask hammers UDP broadcasts.
 *  T4  PENDING_FLAG    bgTask sets volatile flag; loop() draws.
 *  T5  LITTLEFS_ROUND  bgTask writes JPEG to LittleFS; loop() reads+draws.
 *  T6  DOUBLE_BUF_ON   useDataBuf=true + single flush() after drawJpg().
 *  T7  DOUBLE_BUF_OFF  useDataBuf=false, no flush().
 *  T8  FLUSH_PER_MCU   flush() inside every jpegDrawCallback.
 *  T9  MUTEX_CONTEND   bgTask holds drawMutex 50 ms mid-frame.
 * T10  RECONNECT_SIM   bgTask creates/destroys WiFiClientSecure every 3 s.
 *
 * REQUIRED LIBRARIES
 *  - Arduino_GFX_Library (moononournation)
 *  - TJpg_Decoder (Bodmer)
 *  - ArduinoOTA  (built-in ESP32 core)
 *  - Preferences (built-in ESP32 core)
 */

// JpegEncoder.h must come first -- it defines HuffCode/BitWriter/makeGrayscaleJpeg
// before the Arduino IDE pre-processor hoists any .ino function prototypes.
#include "JpegEncoder.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Arduino_GFX_Library.h>
#include <TJpg_Decoder.h>

// ============================================================
// Secrets.h -- optional. If present, defines MYSSID and MYPSK.
// ============================================================
#if __has_include(<Secrets.h>)
  #include <Secrets.h>
#else
  #ifndef MYSSID
    #define MYSSID ""
  #endif
  #ifndef MYPSK
    #define MYPSK ""
  #endif
#endif

// ============================================================
// PIN CONFIGURATION -- match your hardware
// ============================================================
#define GFX_BL   2
#define SCREEN_W 800
#define SCREEN_H 480

#define RGB_DE   40
#define RGB_VS   41
#define RGB_HS   39
#define RGB_PCLK 42
#define RGB_R0   45
#define RGB_R1   48
#define RGB_R2   47
#define RGB_R3   21
#define RGB_R4   14
#define RGB_G0   5
#define RGB_G1   6
#define RGB_G2   7
#define RGB_G3   15
#define RGB_G4   16
#define RGB_G5   4
#define RGB_B0   8
#define RGB_B1   3
#define RGB_B2   46
#define RGB_B3   9
#define RGB_B4   1

static const char* AP_SSID_DEFAULT = "SF-GlitchTest";
static const char* PREF_NS         = "gltest";

// ============================================================
// Preferences / WiFi credentials
// ============================================================
static Preferences prefs;
static String savedSsid;
static String savedPass;

static void loadWifiPrefs() {
  prefs.begin(PREF_NS, true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();
  if (savedSsid.length() == 0 && strlen(MYSSID) > 0) {
    savedSsid = String(MYSSID);
    savedPass = String(MYPSK);
  }
}

static void saveWifiPrefs(const String& ssid, const String& pass) {
  prefs.begin(PREF_NS, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  savedSsid = ssid;
  savedPass = pass;
}

// ============================================================
// GFX
// ============================================================
static Arduino_ESP32RGBPanel* rgbpanel = nullptr;
static Arduino_GFX*           gfx      = nullptr;

static void buildGfx(bool useDataBuf) {
  if (gfx)      { delete gfx;      gfx      = nullptr; }
  if (rgbpanel) { delete rgbpanel; rgbpanel = nullptr; }
  rgbpanel = new Arduino_ESP32RGBPanel(
    RGB_DE, RGB_VS, RGB_HS, RGB_PCLK,
    RGB_R0, RGB_R1, RGB_R2, RGB_R3, RGB_R4,
    RGB_G0, RGB_G1, RGB_G2, RGB_G3, RGB_G4, RGB_G5,
    RGB_B0, RGB_B1, RGB_B2, RGB_B3, RGB_B4,
    0, 8, 4, 24,
    0, 8, 4, 24,
    1, 16000000,
    true, 0, 0, 800 * 20
  );
  gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel, 0, useDataBuf);
  gfx->begin();
  if (useDataBuf) gfx->flush();
  gfx->fillScreen(0x0000);
  if (useDataBuf) gfx->flush();
  Serial.printf("[GFX] useDataBuf=%d  freePSRAM=%u\n",
                (int)useDataBuf, (unsigned)ESP.getFreePsram());
}

// ============================================================
// Test JPEG buffer
// ============================================================
static uint8_t* testJpegBuf     = nullptr;
static size_t   testJpegLen     = 0;
static bool     jpegIsGenerated = false;

static void makeGeneratedJpeg() {
  if (testJpegBuf) { free(testJpegBuf); testJpegBuf = nullptr; }
  testJpegLen = 0; jpegIsGenerated = false;
  size_t len = 0;
  uint8_t* buf = makeGrayscaleJpeg(160, 120, &len);
  if (buf && len > 0) {
    testJpegBuf = buf; testJpegLen = len; jpegIsGenerated = true;
    Serial.printf("[JPEG] generated 160x120, %u bytes\n", (unsigned)len);
  } else {
    Serial.println("[JPEG] generation failed");
    if (buf) free(buf);
  }
}

static bool loadJpegFromFs(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t len = f.size();
  uint8_t* buf = (uint8_t*)ps_malloc(len);
  if (!buf) buf = (uint8_t*)malloc(len);
  if (!buf) { f.close(); return false; }
  size_t rd = f.read(buf, len);
  f.close();
  if (rd != len) { free(buf); return false; }
  if (testJpegBuf) free(testJpegBuf);
  testJpegBuf = buf; testJpegLen = len; jpegIsGenerated = false;
  Serial.printf("[JPEG] loaded %s  %u bytes\n", path, (unsigned)len);
  return true;
}

// ============================================================
// Stripes / fallback draw
// ============================================================
static void drawStripes() {
  static const uint16_t PAL[] = {
    0xF800,0x07E0,0x001F,0xFFE0,0xF81F,0x07FF,0xFFFF,0x8410
  };
  int sh = SCREEN_H / 16;
  for (int i=0;i<16;i++)
    gfx->fillRect(0, i*sh, SCREEN_W, sh, PAL[i&7]);
}

static void drawFallback() {
  gfx->fillRect(0,0,SCREEN_W,SCREEN_H,0x001F);
  gfx->setTextColor(0xFFFF); gfx->setTextSize(2);
  gfx->setCursor(20, SCREEN_H/2-10);
  gfx->print("No JPEG (upload via web UI)");
}

// ============================================================
// TJpg_Decoder callback
// ============================================================
static volatile bool useFlushPerMCU = false;

static bool jpegCB(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* data) {
  if (x>=SCREEN_W||y>=SCREEN_H||x<0||y<0) return true;
  int cw = ((int)x+w>SCREEN_W)?SCREEN_W-x:(int)w;
  int ch = ((int)y+h>SCREEN_H)?SCREEN_H-y:(int)h;
  if (cw==(int)w&&ch==(int)h)
    gfx->draw16bitRGBBitmap(x,y,data,w,h);
  else
    for (int r=0;r<ch;r++) gfx->draw16bitRGBBitmap(x,y+r,data+r*w,cw,1);
  if (useFlushPerMCU) gfx->flush();
  return true;
}

static void decodeAndDraw(const uint8_t* jpg, size_t len, bool flushAfter) {
  if (!jpg||!len) { drawFallback(); return; }
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(jpegCB);
  TJpgDec.drawJpg(0, 0, jpg, (uint32_t)len);
  if (flushAfter) gfx->flush();
}

// ============================================================
// Shared test state
// ============================================================
static volatile bool     pendingDraw     = false;
static volatile bool     stopBg          = false;
static SemaphoreHandle_t drawMutex       = nullptr;
static TaskHandle_t      bgTaskHandle    = nullptr;
static int               activeTest      = 0;
static uint32_t          drawCount       = 0;
static char              statusMsg[160]  = "Idle";
static unsigned long     statusPrintMs   = 0;

// ============================================================
// Background task
// ============================================================
static void bgTask(void* pv) {
  int id = (int)(intptr_t)pv;
  while (!stopBg) {
    switch (id) {
      case 2:
        decodeAndDraw(testJpegBuf, testJpegLen, false);
        drawCount++;
        vTaskDelay(pdMS_TO_TICKS(33));
        break;
      case 3: {
        WiFiUDP udp;
        uint8_t buf[512]; memset(buf,0xAA,sizeof(buf));
        for (int i=0;i<30&&!stopBg;i++) {
          udp.beginPacket("255.255.255.255",9999);
          udp.write(buf,sizeof(buf)); udp.endPacket();
          vTaskDelay(pdMS_TO_TICKS(3));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        break;
      }
      case 4:
        vTaskDelay(pdMS_TO_TICKS(200));
        pendingDraw = true;
        break;
      case 5:
        if (testJpegBuf && testJpegLen) {
          File f = LittleFS.open("/gltest.jpg","w",true);
          if (f) { f.write(testJpegBuf,testJpegLen); f.close(); }
        }
        pendingDraw = true;
        vTaskDelay(pdMS_TO_TICKS(500));
        break;
      case 9:
        vTaskDelay(pdMS_TO_TICKS(10));
        if (xSemaphoreTake(drawMutex,pdMS_TO_TICKS(5))==pdTRUE) {
          vTaskDelay(pdMS_TO_TICKS(50));
          xSemaphoreGive(drawMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        break;
      case 10: {
        WiFiClientSecure* c = new WiFiClientSecure();
        c->setInsecure(); c->setTimeout(2);
        c->connect("192.0.2.1",443);
        c->stop(); delete c;
        vTaskDelay(pdMS_TO_TICKS(3000));
        break;
      }
      default:
        vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  bgTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static void stopBgTask() {
  if (!bgTaskHandle) return;
  stopBg = true;
  unsigned long t0 = millis();
  while (bgTaskHandle && millis()-t0 < 3000) delay(10);
  stopBg = false;
}

// ============================================================
// Web server
// ============================================================
static WebServer webServer(80);

static const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SF Glitch Tester</title>
<style>
body{font-family:monospace;background:#111;color:#eee;padding:1em 2em;max-width:960px;}
h2{color:#4af;margin-top:0;}
p.note{color:#aaa;font-size:12px;}
table{border-collapse:collapse;width:100%;}
td,th{border:1px solid #333;padding:7px 12px;vertical-align:top;}
th{background:#1a2a3a;color:#8cf;}
td:first-child{white-space:nowrap;font-weight:bold;color:#ffd;}
td:nth-child(2){font-size:12px;color:#bbb;}
.btn{padding:6px 14px;background:#1a3a5a;color:#fff;border:1px solid #4af;border-radius:5px;cursor:pointer;font-size:13px;}
.btn:hover{background:#2a5a8a;}
.stop{background:#5a1a1a;border-color:#f44;}
.stop:hover{background:#8a2a2a;}
.grn{background:#1a4a1a;border-color:#4f4;}
.grn:hover{background:#2a6a2a;}
#status{margin-top:1em;background:#1a2030;padding:10px 14px;border-radius:8px;border:1px solid #334;font-size:14px;}
.card{background:#1a2030;border:1px solid #334;border-radius:8px;padding:1em;margin-top:1em;}
.card h3{margin-top:0;color:#8cf;font-size:14px;}
input[type=text],input[type=password]{background:#222;color:#eee;border:1px solid #555;border-radius:4px;padding:5px 8px;width:260px;font-size:13px;}
label{display:inline-block;width:80px;color:#aaa;font-size:13px;}
.row{margin:6px 0;}
#uploadProg{display:none;margin-top:6px;color:#4af;font-size:13px;}
</style>
</head><body>
<h2>&#128247; SyncFrame &#8212; RGB Panel Glitch Tester</h2>
<p class="note">Each test runs continuously. Watch the display for a line-shift/wrap artifact.<br>
T1 should <em>never</em> glitch. JPEG-path tests (T2&#8211;T10) use the generated JPEG until you upload a real one.</p>
<table>
<tr><th>Test</th><th>Theory</th><th></th></tr>
<tr><td>T1 Baseline</td><td>GFX stripes in loop() only &#8212; no JPEG, no tasks. Reference.</td><td><button class="btn" onclick="run(1)">Start</button></td></tr>
<tr><td>T2 Task Draw</td><td>JPEG decoded from FreeRTOS task &#8212; DMA write race.</td><td><button class="btn" onclick="run(2)">Start</button></td></tr>
<tr><td>T3 WiFi Burst</td><td>UDP TX bursts while loop() draws &#8212; PSRAM bus contention.</td><td><button class="btn" onclick="run(3)">Start</button></td></tr>
<tr><td>T4 Pending Flag</td><td>pendingDraw handoff timing races DMA scanner.</td><td><button class="btn" onclick="run(4)">Start</button></td></tr>
<tr><td>T5 LittleFS</td><td>Write&#8594;read via LittleFS in draw path.</td><td><button class="btn" onclick="run(5)">Start</button></td></tr>
<tr><td>T6 DblBuf ON</td><td>useDataBuf=true + flush() after full frame.</td><td><button class="btn" onclick="run(6)">Start</button></td></tr>
<tr><td>T7 DblBuf OFF</td><td>useDataBuf=false, no flush().</td><td><button class="btn" onclick="run(7)">Start</button></td></tr>
<tr><td>T8 Flush/MCU</td><td>flush() inside every jpegDrawCallback.</td><td><button class="btn" onclick="run(8)">Start</button></td></tr>
<tr><td>T9 Mutex</td><td>Second task holds drawMutex 50 ms mid-frame.</td><td><button class="btn" onclick="run(9)">Start</button></td></tr>
<tr><td>T10 Reconnect</td><td>WiFiClientSecure create/destroy every 3 s while drawing.</td><td><button class="btn" onclick="run(10)">Start</button></td></tr>
<tr><td colspan="2"></td><td><button class="btn stop" onclick="run(0)">&#9632; Stop</button></td></tr>
</table>
<div id="status">Loading...</div>
<div class="card">
  <h3>&#128444; JPEG Image</h3>
  <p style="font-size:12px;color:#aaa;margin:0 0 8px">Upload an 800x480 JPEG for realistic testing. Saved to LittleFS as /photo.jpg.</p>
  <form id="upForm" onsubmit="uploadJpeg(event)">
    <input type="file" id="jpegFile" accept="image/jpeg" required style="font-size:13px;color:#ccc;">
    <button type="submit" class="btn grn" style="margin-left:8px;">Upload</button>
  </form>
  <div id="uploadProg">Uploading...</div>
</div>
<div class="card">
  <h3>&#128246; WiFi Credentials</h3>
  <div class="row"><label>SSID</label><input type="text" id="wSsid" placeholder="Network name"></div>
  <div class="row"><label>Password</label><input type="password" id="wPass" placeholder="Password"></div>
  <div style="margin-top:8px;">
    <button class="btn" onclick="saveWifi()">Save &amp; Reconnect</button>
    <span id="wifiMsg" style="margin-left:12px;font-size:13px;color:#4af;"></span>
  </div>
</div>
<div class="card">
  <h3>&#9889; OTA Firmware Update</h3>
  <p style="font-size:12px;color:#aaa;margin:0 0 8px">Flash via ArduinoOTA on port 3232, or Arduino IDE OTA target.</p>
  <button class="btn" onclick="reboot()">Reboot Device</button>
  <span id="rebootMsg" style="margin-left:12px;font-size:13px;color:#fa4;"></span>
</div>
<script>
function run(n){fetch('/test?id='+n).then(r=>r.json()).then(d=>{document.getElementById('status').textContent='T'+n+': '+d.status;}).catch(e=>document.getElementById('status').textContent='Error: '+e);}
setInterval(function(){fetch('/status').then(r=>r.json()).then(d=>{document.getElementById('status').innerHTML='<b>Active: T'+d.test+'</b> &emsp; Draws: '+d.draws+' &emsp; '+d.msg;}).catch(()=>{});},1200);
function uploadJpeg(e){e.preventDefault();var f=document.getElementById('jpegFile').files[0];if(!f)return;var fd=new FormData();fd.append('jpeg',f);var p=document.getElementById('uploadProg');p.style.display='block';p.textContent='Uploading '+f.name+' ('+f.size+' bytes)...';fetch('/upload',{method:'POST',body:fd}).then(r=>r.json()).then(d=>{p.textContent=d.ok?('Uploaded '+d.bytes+' bytes.'):('Error: '+d.err);if(d.ok)setTimeout(()=>p.style.display='none',3000);}).catch(e=>{p.textContent='Upload error: '+e;});}
function saveWifi(){var s=document.getElementById('wSsid').value;var p=document.getElementById('wPass').value;fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)}).then(r=>r.json()).then(d=>{document.getElementById('wifiMsg').textContent=d.ok?'Saved! Reconnecting...':'Error: '+d.err;}).catch(e=>document.getElementById('wifiMsg').textContent='Error: '+e);}
function reboot(){fetch('/reboot',{method:'POST'}).then(()=>{document.getElementById('rebootMsg').textContent='Rebooting...';});}
</script>
</body></html>
)rawhtml";

static void handleRoot()   { webServer.send_P(200, "text/html", INDEX_HTML); }

static void handleStatus() {
  char j[280];
  snprintf(j, sizeof(j),
    "{\"test\":%d,\"draws\":%lu,\"msg\":\"%s\",\"jpeg\":\"%s\"}",
    activeTest, (unsigned long)drawCount, statusMsg,
    jpegIsGenerated ? "generated" : (testJpegBuf ? "uploaded" : "none"));
  webServer.send(200, "application/json", j);
}

static void handleTestReq() {
  int id = webServer.hasArg("id") ? webServer.arg("id").toInt() : 0;
  stopBgTask();
  activeTest = 0; drawCount = 0; useFlushPerMCU = false; pendingDraw = false;
  if (id == 0) {
    snprintf(statusMsg, sizeof(statusMsg), "Stopped");
    gfx->fillScreen(0x0000);
    webServer.send(200, "application/json", "{\"status\":\"Stopped\"}");
    return;
  }
  if (id == 6) buildGfx(true);
  else if (id == 7) buildGfx(false);
  useFlushPerMCU = (id == 8);
  activeTest = id;
  snprintf(statusMsg, sizeof(statusMsg), "T%d running", id);
  if (id==2||id==3||id==4||id==5||id==9||id==10)
    xTaskCreatePinnedToCore(bgTask,"bgTask",8192,(void*)(intptr_t)id,0,&bgTaskHandle,0);
  char j[64]; snprintf(j,sizeof(j),"{\"status\":\"T%d running\"}",id);
  webServer.send(200, "application/json", j);
}

static File uploadFile;
static void handleUploadBody() {
  HTTPUpload& upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    LittleFS.remove("/photo.jpg");
    uploadFile = LittleFS.open("/photo.jpg", "w", true);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    bool ok = loadJpegFromFs("/photo.jpg");
    if (!ok) makeGeneratedJpeg();
    char j[80];
    snprintf(j, sizeof(j), "{\"ok\":%s,\"bytes\":%u}",
             ok?"true":"false", (unsigned)upload.totalSize);
    webServer.send(200, "application/json", j);
  }
}

static void handleWifi() {
  if (!webServer.hasArg("ssid") || webServer.arg("ssid").length()==0) {
    webServer.send(400,"application/json","{\"ok\":false,\"err\":\"missing ssid\"}");
    return;
  }
  String ssid = webServer.arg("ssid");
  String pass = webServer.hasArg("pass") ? webServer.arg("pass") : "";
  saveWifiPrefs(ssid, pass);
  webServer.send(200,"application/json","{\"ok\":true}");
  delay(300); WiFi.disconnect(true); delay(200);
  WiFi.begin(ssid.c_str(), pass.c_str());
}

static void handleReboot() {
  webServer.send(200,"application/json","{\"ok\":true}");
  delay(300); ESP.restart();
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[SF-GlitchTest] boot");

  drawMutex = xSemaphoreCreateBinary();
  xSemaphoreGive(drawMutex);

  buildGfx(false);
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  gfx->setTextColor(0xFFFF); gfx->setTextSize(3);
  gfx->setCursor(20,20); gfx->print("SF Glitch Tester");
  gfx->setTextSize(2);

  if (!LittleFS.begin(true))
    Serial.println("[FS] LittleFS mount failed");

  if (!loadJpegFromFs("/photo.jpg"))
    makeGeneratedJpeg();

  loadWifiPrefs();

  gfx->setCursor(20,70); gfx->print("WiFi...");

  if (savedSsid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status()!=WL_CONNECTED && millis()-t0<12000) delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID_DEFAULT);
    Serial.printf("[WiFi] AP: %s  IP: %s\n",
      AP_SSID_DEFAULT, WiFi.softAPIP().toString().c_str());
    gfx->setCursor(20,100); gfx->printf("AP: %s", AP_SSID_DEFAULT);
    gfx->setCursor(20,130); gfx->printf("http://%s", WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
    gfx->setCursor(20,100); gfx->printf("IP: %s", WiFi.localIP().toString().c_str());
  }

  ArduinoOTA.setHostname("SF-GlitchTest");
  ArduinoOTA.onStart([]() {
    stopBgTask();
    Serial.printf("[OTA] start %s\n",
      ArduinoOTA.getCommand()==U_FLASH ? "sketch" : "filesystem");
    gfx->fillScreen(0x0000);
    gfx->setTextSize(2); gfx->setTextColor(0xFFFF);
    gfx->setCursor(20,20); gfx->print("OTA Update...");
  });
  ArduinoOTA.onProgress([](unsigned int prog, unsigned int total) {
    int pct = prog*100/total;
    static int last=-1;
    if (pct!=last) {
      last=pct;
      char buf[12]; snprintf(buf,sizeof(buf),"%3d%%",pct);
      gfx->fillRect(20,55,200,40,0x0000);
      gfx->setCursor(20,60); gfx->setTextSize(3); gfx->print(buf);
    }
  });
  ArduinoOTA.onEnd([]()  { Serial.println("[OTA] done"); });
  ArduinoOTA.onError([](ota_error_t e){ Serial.printf("[OTA] error %u\n",e); });
  ArduinoOTA.begin();
  Serial.println("[OTA] ready");

  webServer.on("/",       HTTP_GET,  handleRoot);
  webServer.on("/status", HTTP_GET,  handleStatus);
  webServer.on("/test",   HTTP_GET,  handleTestReq);
  webServer.on("/wifi",   HTTP_POST, handleWifi);
  webServer.on("/reboot", HTTP_POST, handleReboot);
  webServer.on("/upload", HTTP_POST,
    [](){},
    handleUploadBody
  );
  webServer.begin();

  gfx->setCursor(20,170);
  gfx->printf("JPEG: %s", jpegIsGenerated?"generated":"loaded from FS");
  gfx->setCursor(20,200); gfx->print("Ready");
  snprintf(statusMsg, sizeof(statusMsg), "Ready");
  Serial.println("[SF-GlitchTest] ready");
}

// ============================================================
// Loop
// ============================================================
void loop() {
  ArduinoOTA.handle();
  webServer.handleClient();

  switch (activeTest) {
    case 0:  delay(10); break;
    case 1:  drawStripes(); drawCount++; delay(33); break;
    case 2:  delay(10); break;
    case 3:  decodeAndDraw(testJpegBuf,testJpegLen,false); drawCount++; delay(33); break;
    case 4:
      if (pendingDraw) { pendingDraw=false; decodeAndDraw(testJpegBuf,testJpegLen,false); drawCount++; }
      delay(5); break;
    case 5:
      if (pendingDraw) {
        pendingDraw=false;
        File f=LittleFS.open("/gltest.jpg","r");
        if (f) {
          size_t len=f.size();
          uint8_t* b=(uint8_t*)malloc(len);
          if (b){f.read(b,len);f.close();decodeAndDraw(b,len,false);free(b);drawCount++;}
          else f.close();
        }
      }
      delay(5); break;
    case 6:  decodeAndDraw(testJpegBuf,testJpegLen,true);  drawCount++; delay(33); break;
    case 7:  decodeAndDraw(testJpegBuf,testJpegLen,false); drawCount++; delay(33); break;
    case 8:  decodeAndDraw(testJpegBuf,testJpegLen,false); drawCount++; delay(33); break;
    case 9:
      if (xSemaphoreTake(drawMutex,pdMS_TO_TICKS(200))==pdTRUE) {
        decodeAndDraw(testJpegBuf,testJpegLen,false);
        xSemaphoreGive(drawMutex); drawCount++;
      }
      delay(33); break;
    case 10: decodeAndDraw(testJpegBuf,testJpegLen,false); drawCount++; delay(33); break;
    default: delay(10); break;
  }

  if (millis()-statusPrintMs>5000 && activeTest!=0) {
    statusPrintMs=millis();
    snprintf(statusMsg,sizeof(statusMsg),
      "T%d | draws=%lu | heap=%u | psram=%u",
      activeTest,(unsigned long)drawCount,
      (unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
    Serial.printf("[STATUS] %s\n",statusMsg);
  }
}
