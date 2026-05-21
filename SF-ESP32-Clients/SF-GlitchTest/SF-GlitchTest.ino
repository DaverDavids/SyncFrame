/*
 * SF_GlitchTest.ino
 *
 * Standalone ESP32-S3 RGB panel glitch tester.
 * No server, no stream, no LittleFS required (LittleFS used only for T5).
 *
 * A small web UI lets you trigger each test scenario and watch the display.
 * Each test repeatedly draws a reference image while applying the condition
 * under test, so you can observe whether the line-shift artifact appears.
 *
 * -----------------------------------------------------------------------
 * TESTS
 * -----------------------------------------------------------------------
 *  T1  BASELINE        – loop() only, GFX stripe primitives, no JPEG, no tasks.
 *                        Should NEVER glitch. Confirms the display itself is fine.
 *  T2  TASK_DRAW       – JPEG decoded and drawn from a FreeRTOS task (prio 0).
 *                        Tests whether drawing off-loop causes a DMA race.
 *  T3  WIFI_BURST      – loop() draws while bgTask hammers 512-byte UDP broadcasts.
 *                        Tests whether WiFi TX activity alone causes DMA contention.
 *  T4  PENDING_FLAG    – bgTask sets a volatile flag; loop() draws on that flag.
 *                        Exact simulation of the pendingDraw handoff in main code.
 *  T5  LITTLEFS_ROUND  – bgTask writes JPEG to LittleFS; loop() reads+draws it.
 *                        Tests whether LittleFS in the draw path is the trigger.
 *  T6  DOUBLE_BUF_ON   – Re-inits display with useDataBuf=true; draws with
 *                        a single gfx->flush() after drawJpg() completes.
 *  T7  DOUBLE_BUF_OFF  – Re-inits display with useDataBuf=false; draws with
 *                        no flush(). Direct single-buffer comparison to T6.
 *  T8  FLUSH_PER_MCU   – Calls gfx->flush() inside EVERY jpegDrawCallback.
 *                        Tests if per-MCU flushing causes partial-frame swaps.
 *  T9  MUTEX_CONTEND   – bgTask acquires drawMutex for 50 ms mid-frame.
 *                        Tests if mutex starvation during draw causes corruption.
 * T10  RECONNECT_SIM   – bgTask rapidly creates/destroys WiFiClientSecure every
 *                        3 s while loop() draws. Simulates stream reconnect.
 *
 * -----------------------------------------------------------------------
 * JPEG NOTE
 * -----------------------------------------------------------------------
 * For realistic testing, supply an 800x480 JPEG.
 * Two options (pick one):
 *   A) Upload photo.jpg to LittleFS via the Arduino LittleFS uploader tool
 *      and the sketch will auto-load it on boot.
 *   B) Embed it as a PROGMEM array and point testJpegBuf/testJpegLen at it.
 *
 * If no JPEG is found, T1 draws colour stripes with GFX primitives and all
 * JPEG-path tests (T2–T10) draw a solid blue rectangle instead.
 *
 * -----------------------------------------------------------------------
 * REQUIRED LIBRARIES (Arduino Library Manager)
 * -----------------------------------------------------------------------
 *   - Arduino_GFX_Library (moononournation)
 *   - TJpg_Decoder (Bodmer)
 *
 * BOARD: ESP32-S3  (your RGB panel pin mapping is in PIN CONFIGURATION below)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Arduino_GFX_Library.h>
#include <TJpg_Decoder.h>

// ============================================================
// PIN CONFIGURATION — match your hardware
// ============================================================
#define GFX_BL   2
#define SCREEN_W 800
#define SCREEN_H 480

// RGB panel constructor args (same as your main sketch)
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

// ============================================================
// WIFI — fill in or leave SSID empty to run as AP
// ============================================================
const char* WIFI_SSID = "";          // leave empty → AP mode
const char* WIFI_PASS = "";
const char* AP_SSID   = "SF-GlitchTest";

// ============================================================
// GFX — pointer so we can re-init for T6/T7
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

  Serial.printf("[GFX] init useDataBuf=%d  freePSRAM=%u\n",
                (int)useDataBuf, (unsigned)ESP.getFreePsram());
}

// ============================================================
// Test JPEG buffer
// ============================================================
static uint8_t* testJpegBuf = nullptr;
static size_t   testJpegLen = 0;

// Fallback — draws coloured stripes using GFX primitives (no JPEG)
static void drawStripes() {
  static const uint16_t PAL[] = {
    0xF800, 0x07E0, 0x001F, 0xFFE0,
    0xF81F, 0x07FF, 0xFFFF, 0x8410
  };
  int sh = SCREEN_H / 16;
  for (int i = 0; i < 16; i++)
    gfx->fillRect(0, i * sh, SCREEN_W, sh, PAL[i & 7]);
}

// Fallback when no JPEG loaded — solid blue rectangle
static void drawFallback() {
  gfx->fillRect(0, 0, SCREEN_W, SCREEN_H, 0x001F);
  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(2);
  gfx->setCursor(20, SCREEN_H / 2 - 10);
  gfx->print("No JPEG loaded (see sketch notes)");
}

// ============================================================
// TJpg_Decoder callback
// ============================================================
static volatile bool useFlushPerMCU = false;

static bool jpegCB(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* data) {
  if (x >= SCREEN_W || y >= SCREEN_H || x < 0 || y < 0) return true;
  int cw = ((int)x + w > SCREEN_W) ? SCREEN_W - x : (int)w;
  int ch = ((int)y + h > SCREEN_H) ? SCREEN_H - y : (int)h;
  if (cw == (int)w && ch == (int)h) {
    gfx->draw16bitRGBBitmap(x, y, data, w, h);
  } else {
    for (int r = 0; r < ch; r++)
      gfx->draw16bitRGBBitmap(x, y + r, data + r * w, cw, 1);
  }
  if (useFlushPerMCU) gfx->flush();
  return true;
}

// Draw JPEG from RAM buffer; optionally flush after full frame
static void decodeAndDraw(const uint8_t* jpg, size_t len, bool flushAfter) {
  if (!jpg || !len) { drawFallback(); return; }
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(jpegCB);
  TJpgDec.drawJpg(0, 0, jpg, (uint32_t)len);
  if (flushAfter) gfx->flush();
}

// ============================================================
// Shared state
// ============================================================
static volatile bool pendingDraw   = false;
static volatile bool stopBg        = false;
static SemaphoreHandle_t drawMutex = nullptr;
static TaskHandle_t bgTaskHandle   = nullptr;

static int      activeTest   = 0;
static uint32_t drawCount    = 0;
static char     statusMsg[160] = "Idle — open web UI";
static unsigned long statusPrintMs = 0;

// ============================================================
// Background task
// ============================================================
static void bgTask(void* pv) {
  int id = (int)(intptr_t)pv;

  while (!stopBg) {
    switch (id) {

      case 2: // T2: draw JPEG from this task
        decodeAndDraw(testJpegBuf, testJpegLen, false);
        drawCount++;
        vTaskDelay(pdMS_TO_TICKS(33));
        break;

      case 3: // T3: UDP broadcast burst (WiFi TX stress)
        {
          WiFiUDP udp;
          uint8_t buf[512];
          memset(buf, 0xAA, sizeof(buf));
          for (int i = 0; i < 30 && !stopBg; i++) {
            udp.beginPacket("255.255.255.255", 9999);
            udp.write(buf, sizeof(buf));
            udp.endPacket();
            vTaskDelay(pdMS_TO_TICKS(3));
          }
          vTaskDelay(pdMS_TO_TICKS(50));
        }
        break;

      case 4: // T4: set pendingDraw; loop() will draw
        vTaskDelay(pdMS_TO_TICKS(200));
        pendingDraw = true;
        break;

      case 5: // T5: write JPEG to LittleFS; loop() reads+draws
        if (testJpegBuf && testJpegLen) {
          File f = LittleFS.open("/gltest.jpg", "w", true);
          if (f) { f.write(testJpegBuf, testJpegLen); f.close(); }
        }
        pendingDraw = true;
        vTaskDelay(pdMS_TO_TICKS(500));
        break;

      case 9: // T9: acquire drawMutex for 50 ms mid-frame
        vTaskDelay(pdMS_TO_TICKS(10));  // let loop() start decoding first
        if (xSemaphoreTake(drawMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          vTaskDelay(pdMS_TO_TICKS(50));
          xSemaphoreGive(drawMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        break;

      case 10: // T10: create/connect/destroy WiFiClientSecure every 3 s
        {
          WiFiClientSecure* c = new WiFiClientSecure();
          c->setInsecure();
          c->setTimeout(2);
          c->connect("192.0.2.1", 443);  // RFC 5737 test addr — unreachable, times out fast
          c->stop();
          delete c;
          vTaskDelay(pdMS_TO_TICKS(3000));
        }
        break;

      default:
        vTaskDelay(pdMS_TO_TICKS(100));
        break;
    }
  }

  bgTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static void stopBgTask() {
  if (!bgTaskHandle) return;
  stopBg = true;
  unsigned long t0 = millis();
  while (bgTaskHandle != nullptr && millis() - t0 < 3000) delay(10);
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
body{font-family:monospace;background:#111;color:#eee;padding:1em 2em;}
h2{color:#4af;margin-top:0;}
p.note{color:#aaa;font-size:12px;}
table{border-collapse:collapse;width:100%;max-width:900px;}
td,th{border:1px solid #333;padding:7px 12px;vertical-align:top;}
th{background:#1a2a3a;color:#8cf;}
td:first-child{white-space:nowrap;font-weight:bold;color:#ffd;}
td:nth-child(2){font-size:12px;color:#bbb;}
button{padding:6px 14px;background:#1a3a5a;color:#fff;border:1px solid #4af;
       border-radius:5px;cursor:pointer;font-size:13px;}
button:hover{background:#2a5a8a;}
button.stop{background:#5a1a1a;border-color:#f44;}
button.stop:hover{background:#8a2a2a;}
#status{margin-top:1.5em;background:#1a2030;padding:10px 14px;border-radius:8px;
        border:1px solid #334;font-size:14px;max-width:900px;}
</style>
</head><body>
<h2>&#128247; SyncFrame — RGB Panel Glitch Tester</h2>
<p class="note">
  Start a test, watch the display. A line-shift/wrap artifact = the glitch reproduced for that theory.<br>
  T1 should <em>never</em> glitch and is your sanity-check baseline.
  For JPEG tests (T2–T10), upload a <code>photo.jpg</code> to LittleFS first.
</p>

<table>
<tr><th>Test</th><th>Theory being tested</th><th></th></tr>
<tr><td>T1 Baseline</td>
    <td>No JPEG, no tasks — GFX stripe primitives in loop() only. Should never glitch.</td>
    <td><button onclick="run(1)">Start T1</button></td></tr>
<tr><td>T2 Task Draw</td>
    <td>Drawing from a FreeRTOS task (prio 0) causes RGB DMA write race.</td>
    <td><button onclick="run(2)">Start T2</button></td></tr>
<tr><td>T3 WiFi Burst</td>
    <td>WiFi UDP TX bursts while loop() draws cause DMA bus contention.</td>
    <td><button onclick="run(3)">Start T3</button></td></tr>
<tr><td>T4 Pending Flag</td>
    <td>pendingDraw handoff timing races the DMA scanner.</td>
    <td><button onclick="run(4)">Start T4</button></td></tr>
<tr><td>T5 LittleFS Round-trip</td>
    <td>LittleFS write→read in the draw path introduces a timing gap that triggers glitch.</td>
    <td><button onclick="run(5)">Start T5</button></td></tr>
<tr><td>T6 Double-buf ON</td>
    <td>useDataBuf=true + flush() after full frame eliminates glitch.</td>
    <td><button onclick="run(6)">Start T6</button></td></tr>
<tr><td>T7 Double-buf OFF</td>
    <td>useDataBuf=false (single-buffer) with no flush() is glitch-free.</td>
    <td><button onclick="run(7)">Start T7</button></td></tr>
<tr><td>T8 Flush Per MCU</td>
    <td>flush() inside every jpegDrawCallback causes partial-frame buffer swaps.</td>
    <td><button onclick="run(8)">Start T8</button></td></tr>
<tr><td>T9 Mutex Contention</td>
    <td>drawMutex held by a second task 50 ms mid-frame corrupts the draw.</td>
    <td><button onclick="run(9)">Start T9</button></td></tr>
<tr><td>T10 Reconnect Sim</td>
    <td>WiFiClientSecure create/destroy every 3 s while drawing causes glitch.</td>
    <td><button onclick="run(10)">Start T10</button></td></tr>
<tr><td colspan="2"></td>
    <td><button class="stop" onclick="run(0)">&#9632; Stop</button></td></tr>
</table>

<div id="status">Status: connecting...</div>

<script>
function run(n){
  fetch('/test?id='+n).then(r=>r.json()).then(d=>{
    document.getElementById('status').textContent='Requested T'+n+': '+d.status;
  }).catch(e=>{ document.getElementById('status').textContent='Error: '+e; });
}
setInterval(function(){
  fetch('/status').then(r=>r.json()).then(d=>{
    document.getElementById('status').innerHTML =
      '<b>Active: T'+d.test+'</b> &emsp; Draws: '+d.draws+
      ' &emsp; '+d.msg;
  }).catch(()=>{});
},1200);
</script>
</body></html>
)rawhtml";

static void handleRoot() {
  webServer.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus() {
  char j[256];
  snprintf(j, sizeof(j),
    "{\"test\":%d,\"draws\":%lu,\"msg\":\"%s\"}",
    activeTest, (unsigned long)drawCount, statusMsg);
  webServer.send(200, "application/json", j);
}

static void handleTestReq() {
  int id = 0;
  if (webServer.hasArg("id")) id = webServer.arg("id").toInt();

  stopBgTask();
  activeTest = 0;
  drawCount  = 0;
  useFlushPerMCU = false;
  pendingDraw    = false;

  if (id == 0) {
    snprintf(statusMsg, sizeof(statusMsg), "Stopped");
    gfx->fillScreen(0x0000);
    webServer.send(200, "application/json", "{\"status\":\"Stopped\"}");
    return;
  }

  // Re-init GFX for buffer-mode tests
  if      (id == 6) { buildGfx(true);  }
  else if (id == 7) { buildGfx(false); }

  useFlushPerMCU = (id == 8);
  activeTest = id;
  snprintf(statusMsg, sizeof(statusMsg), "T%d starting...", id);

  // Spawn background task for tests that need one
  if (id == 2 || id == 3 || id == 4 || id == 5 || id == 9 || id == 10) {
    xTaskCreatePinnedToCore(bgTask, "bgTask", 8192,
      (void*)(intptr_t)id, 0, &bgTaskHandle, 0);
  }

  char j[128];
  snprintf(j, sizeof(j), "{\"status\":\"T%d running\"}", id);
  webServer.send(200, "application/json", j);
}

// ============================================================
// setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[SF-GlitchTest] boot");

  drawMutex = xSemaphoreCreateBinary();
  xSemaphoreGive(drawMutex);

  buildGfx(false);

  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(3);
  gfx->setCursor(20, 20);
  gfx->print("SF Glitch Tester");
  gfx->setTextSize(2);
  gfx->setCursor(20, 70);
  gfx->print("Starting...");

  // LittleFS — needed for T5 and optional JPEG auto-load
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed");
  } else {
    // Auto-load JPEG if present
    File f = LittleFS.open("/photo.jpg", "r");
    if (f) {
      testJpegLen = f.size();
      testJpegBuf = (uint8_t*)ps_malloc(testJpegLen);
      if (!testJpegBuf) testJpegBuf = (uint8_t*)malloc(testJpegLen);
      if (testJpegBuf) {
        f.read(testJpegBuf, testJpegLen);
        Serial.printf("[JPEG] loaded /photo.jpg  %u bytes\n", (unsigned)testJpegLen);
      } else {
        testJpegLen = 0;
        Serial.println("[JPEG] alloc failed");
      }
      f.close();
    } else {
      Serial.println("[JPEG] /photo.jpg not found — JPEG tests will use fallback");
    }
  }

  // WiFi
  gfx->setCursor(20, 100);
  gfx->print("WiFi...");
  if (strlen(WIFI_SSID) > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    Serial.printf("[WiFi] AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
    gfx->setCursor(20, 130);
    gfx->printf("AP: %s", AP_SSID);
    gfx->setCursor(20, 160);
    gfx->printf("http://%s", WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
    gfx->setCursor(20, 130);
    gfx->printf("http://%s", WiFi.localIP().toString().c_str());
  }

  gfx->setCursor(20, 200);
  gfx->print(testJpegBuf ? "JPEG: loaded" : "JPEG: not found (T1 still works)");

  webServer.on("/",       HTTP_GET, handleRoot);
  webServer.on("/status", HTTP_GET, handleStatus);
  webServer.on("/test",   HTTP_GET, handleTestReq);
  webServer.begin();

  snprintf(statusMsg, sizeof(statusMsg), "Ready — open web UI");
  Serial.println("[SF-GlitchTest] ready");
}

// ============================================================
// loop
// ============================================================
void loop() {
  webServer.handleClient();

  switch (activeTest) {

    case 0:
      delay(10);
      break;

    case 1: // Baseline — GFX stripes, no JPEG, no tasks
      drawStripes();
      drawCount++;
      delay(33);
      break;

    case 2: // bgTask draws; loop() idles
      delay(10);
      break;

    case 3: // bgTask sends UDP; loop() draws JPEG
      decodeAndDraw(testJpegBuf, testJpegLen, false);
      drawCount++;
      delay(33);
      break;

    case 4: // pendingDraw handoff
      if (pendingDraw) {
        pendingDraw = false;
        decodeAndDraw(testJpegBuf, testJpegLen, false);
        drawCount++;
      }
      delay(5);
      break;

    case 5: // bgTask writes LittleFS; loop() reads+draws
      if (pendingDraw) {
        pendingDraw = false;
        File f = LittleFS.open("/gltest.jpg", "r");
        if (f) {
          size_t len = f.size();
          uint8_t* buf = (uint8_t*)malloc(len);
          if (buf) {
            f.read(buf, len);
            f.close();
            decodeAndDraw(buf, len, false);
            free(buf);
            drawCount++;
          } else { f.close(); }
        }
      }
      delay(5);
      break;

    case 6: // Double-buf ON + flush after full frame
      decodeAndDraw(testJpegBuf, testJpegLen, true);
      drawCount++;
      delay(33);
      break;

    case 7: // Double-buf OFF, no flush
      decodeAndDraw(testJpegBuf, testJpegLen, false);
      drawCount++;
      delay(33);
      break;

    case 8: // useFlushPerMCU=true (set in handleTestReq)
      decodeAndDraw(testJpegBuf, testJpegLen, false);
      drawCount++;
      delay(33);
      break;

    case 9: // Mutex contention; bgTask steals mutex mid-frame
      if (xSemaphoreTake(drawMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        decodeAndDraw(testJpegBuf, testJpegLen, false);
        xSemaphoreGive(drawMutex);
        drawCount++;
      }
      delay(33);
      break;

    case 10: // bgTask hammers WiFiClientSecure; loop() draws
      decodeAndDraw(testJpegBuf, testJpegLen, false);
      drawCount++;
      delay(33);
      break;

    default:
      delay(10);
      break;
  }

  // Periodic serial status
  if (millis() - statusPrintMs > 5000 && activeTest != 0) {
    statusPrintMs = millis();
    snprintf(statusMsg, sizeof(statusMsg),
      "T%d active | draws=%lu | heap=%u | psram=%u",
      activeTest, (unsigned long)drawCount,
      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    Serial.printf("[STATUS] %s\n", statusMsg);
  }
}
