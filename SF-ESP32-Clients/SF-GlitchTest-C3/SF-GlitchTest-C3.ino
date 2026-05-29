/*
 * SF-GlitchTest-C3.ino
 *
 * Targeted glitch tester for ESP32-C3 + ST7789 SPI TFT (280x240).
 * Investigates why TJpgDec stops calling jpegDrawCallback before the
 * image is fully drawn — suspected 256-column boundary / PSRAM / SPI
 * bus contention issues.
 *
 * Each of the 8 tests changes exactly ONE variable so results are
 * directly comparable. Every test reports:
 *   - how many 16x16 MCU callbacks TJpgDec fired
 *   - how many it should have fired (ceil(sw/16)*ceil(sh/16))
 *   - x/y/w/h of the LAST callback (i.e. where it stopped)
 *   - heap before/after
 *
 * TESTS
 *  T1  Baseline          PSRAM src, standard draw16bitRGBBitmap callback
 *  T2  InternalRAM       Copy JPEG to internal malloc() before decode
 *  T3  NoSPI             Count-only callback — no GFX writes at all
 *  T4  InternalRAM+NoSPI Both T2 and T3 combined
 *  T5  AlignedPSRAM      32-byte aligned PSRAM alloc
 *  T6  WritePixels       Use gfx->writeAddrWindow/writePixels (Arduino_TFT path)
 *  T7  Scale2            Force scale=2 (avoids 256-column boundary)
 *  T8  Combined          InternalRAM + WritePixels
 *
 * WEB UI  http://<ip>/
 *  POST /upload    multipart JPEG upload (field "jpg")
 *  POST /run?t=N   run test N (1-8), t=0 runs all
 *  GET  /result    JSON result of last test
 *
 * OTA hostname: sf-glitchtest-c3
 *
 * SECRETS: create Secrets.h alongside this file defining:
 *   const char* MYSSID = "...";
 *   const char* MYPSK  = "...";
 *   const char* ARDUINO_OTA_PASSWORD = ""; // empty = no password
 */

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <esp_heap_caps.h>
#include <Arduino_GFX_Library.h>
#include <TJpg_Decoder.h>

// ── Secrets ───────────────────────────────────────────────────────────────────
#if __has_include(<Secrets.h>)
  #include <Secrets.h>
#else
  static const char* MYSSID               = "";
  static const char* MYPSK                = "";
  static const char* ARDUINO_OTA_PASSWORD = "";
#endif

// ── Board: ESP32-C3 + ST7789 280x240 SPI ─────────────────────────────────────
#define TFT_SCLK 0
#define TFT_MOSI 1
#define TFT_DC   3
#define TFT_RST  2
#define TFT_CS   4

static const int SCREEN_W = 280;
static const int SCREEN_H = 240;

// Declare as Arduino_TFT* so writeAddrWindow/writePixels/startWrite are visible.
// Arduino_ST7789 extends Arduino_TFT which has those methods; Arduino_GFX does not.
static Arduino_DataBus* bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED);
static Arduino_TFT*     gfx = new Arduino_ST7789(bus, TFT_RST, 1, true, 240, 280, 0, 20, 0, 20);

// SPI transaction guards — keep WiFi off the bus during MCU writes
static SPISettings _spiCfg(80000000, MSBFIRST, SPI_MODE0);
#define SPI_BEGIN() SPI.beginTransaction(_spiCfg)
#define SPI_END()   SPI.endTransaction()

// ── Image buffer ──────────────────────────────────────────────────────────────
// C3 has NO PSRAM. Max JPEG = 280*240*2 = 131400 bytes. Allocate 140KB from
// internal heap. The WebServer upload buffer sits in internal RAM too, so we
// allocate at START and keep it for the lifetime of the test run.
#define MAX_JPG_BYTES 140000UL

static uint8_t* g_jpgBuf   = nullptr;
static size_t   g_jpgLen   = 0;
static bool     g_hasImage = false;

// ── Per-test MCU counters ─────────────────────────────────────────────────────
static int      s_cbCount = 0;
static int16_t  s_lastX = -1, s_lastY = -1;
static uint16_t s_lastW = 0,  s_lastH = 0;
static void resetCb() { s_cbCount=0; s_lastX=-1; s_lastY=-1; s_lastW=0; s_lastH=0; }

// ── Result ────────────────────────────────────────────────────────────────────
struct Result {
  int      t;
  bool     ran;
  int      got, expected, missing;
  int16_t  lx, ly;
  uint16_t lw, lh;
  uint32_t heapBefore, heapAfter;
  char     note[192];
};
static Result g_res;

// ── Helpers ───────────────────────────────────────────────────────────────────

static int expectedMCUs(int w, int h) {
  return ((w + 15) / 16) * ((h + 15) / 16);
}

// Mirrors board_draw_jpeg scale/letterbox logic
static bool computeLayout(const uint8_t* jpg, size_t len,
                           int& scale, int& ox, int& oy, int& sw, int& sh) {
  uint16_t iw = 0, ih = 0;
  TJpgDec.getJpgSize(&iw, &ih, jpg, (uint32_t)len);
  if (!iw || !ih) return false;
  float as = (float)iw / ih, ad = (float)SCREEN_W / SCREEN_H;
  int tw, th;
  if (as > ad) { tw = SCREEN_W; th = (int)(SCREEN_W / as); }
  else         { th = SCREEN_H; tw = (int)(SCREEN_H * as); }
  int scales[] = {1, 2, 4, 8}, best = 1, bestD = 99999;
  for (int i = 0; i < 4; i++) {
    int s = scales[i];
    int d = abs((int)iw/s - tw) + abs((int)ih/s - th);
    if (d < bestD && (int)iw/s <= SCREEN_W+50 && (int)ih/s <= SCREEN_H+50) { best=s; bestD=d; }
  }
  scale = best;
  sw = (int)iw / best;
  sh = (int)ih / best;
  ox = (SCREEN_W - sw) / 2; if (ox < 0) ox = 0;
  oy = (SCREEN_H - sh) / 2; if (oy < 0) oy = 0;
  return true;
}

static void letterbox(int ox, int oy, int sw, int sh) {
  if (oy > 0) {
    gfx->fillRect(0, 0, SCREEN_W, oy, 0x0000);
    gfx->fillRect(0, oy+sh, SCREEN_W, SCREEN_H-(oy+sh), 0x0000);
  }
  if (ox > 0) {
    gfx->fillRect(0, oy, ox, sh, 0x0000);
    gfx->fillRect(ox+sw, oy, SCREEN_W-(ox+sw), sh, 0x0000);
  }
}

// ── Callbacks ─────────────────────────────────────────────────────────────────

static bool g_countOnly = false;

// T1 / T2 / T5 / T7 — draw16bitRGBBitmap path (same as production code)
static bool jpegCB_draw(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* data) {
  s_cbCount++;
  s_lastX = x; s_lastY = y; s_lastW = w; s_lastH = h;
  if (g_countOnly) return true;
  if (x >= SCREEN_W || y >= SCREEN_H || x < 0 || y < 0) return true;
  int cw = (x+w > SCREEN_W) ? SCREEN_W-x : (int)w;
  int ch = (y+h > SCREEN_H) ? SCREEN_H-y : (int)h;
  SPI_BEGIN();
  if (cw == (int)w && ch == (int)h) {
    gfx->draw16bitRGBBitmap(x, y, data, w, h);
  } else {
    for (int r = 0; r < ch; r++)
      gfx->draw16bitRGBBitmap(x, y+r, data + r*w, cw, 1);
  }
  SPI_END();
  return true;
}

// T6 / T8 — writeAddrWindow + writePixels path (Arduino_TFT members)
static bool jpegCB_wpx(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* data) {
  s_cbCount++;
  s_lastX = x; s_lastY = y; s_lastW = w; s_lastH = h;
  if (x >= SCREEN_W || y >= SCREEN_H || x < 0 || y < 0) return true;
  int cw = (x+w > SCREEN_W) ? SCREEN_W-x : (int)w;
  int ch = (y+h > SCREEN_H) ? SCREEN_H-y : (int)h;
  gfx->startWrite();
  for (int r = 0; r < ch; r++) {
    gfx->writeAddrWindow(x, y+r, cw, 1);
    gfx->writePixels(data + r*w, cw);
  }
  gfx->endWrite();
  return true;
}

static void doDecode(int scale, const uint8_t* src, size_t len, int ox, int oy, bool wpx) {
  TJpgDec.setJpgScale((uint8_t)scale);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(wpx ? jpegCB_wpx : jpegCB_draw);
  TJpgDec.drawJpg(ox, oy, src, (uint32_t)len);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

static void runTest(int t) {
  memset(&g_res, 0, sizeof(g_res));
  g_res.t = t;

  if (!g_hasImage) {
    strncpy(g_res.note, "no image uploaded", sizeof(g_res.note)-1);
    return;
  }

  resetCb();
  g_countOnly = false;

  int scale, ox, oy, sw, sh;
  if (!computeLayout(g_jpgBuf, g_jpgLen, scale, ox, oy, sw, sh)) {
    strncpy(g_res.note, "getJpgSize failed", sizeof(g_res.note)-1);
    return;
  }

  uint32_t hBefore = ESP.getFreeHeap();
  const uint8_t* src = g_jpgBuf;
  uint8_t* tmp = nullptr;

  switch (t) {

    case 1: // Baseline — exactly like production
      letterbox(ox, oy, sw, sh);
      doDecode(scale, src, g_jpgLen, ox, oy, false);
      snprintf(g_res.note, sizeof(g_res.note),
        "internal-heap src draw16bitRGBBitmap scale=%d sw=%d sh=%d ox=%d oy=%d",
        scale, sw, sh, ox, oy);
      break;

    case 2: // Second internal RAM copy (fresh malloc)
      tmp = (uint8_t*)malloc(g_jpgLen);
      if (tmp) { memcpy(tmp, g_jpgBuf, g_jpgLen); src = tmp; }
      letterbox(ox, oy, sw, sh);
      doDecode(scale, src, g_jpgLen, ox, oy, false);
      snprintf(g_res.note, sizeof(g_res.note),
        "fresh-malloc copy=%s scale=%d sw=%d sh=%d",
        tmp ? "OK" : "FAIL(heap full)", scale, sw, sh);
      break;

    case 3: // Count-only — no GFX, no SPI at all
      g_countOnly = true;
      doDecode(scale, src, g_jpgLen, ox, oy, false);
      snprintf(g_res.note, sizeof(g_res.note),
        "count-only noSPI scale=%d sw=%d sh=%d", scale, sw, sh);
      break;

    case 4: // Fresh malloc + count-only
      g_countOnly = true;
      tmp = (uint8_t*)malloc(g_jpgLen);
      if (tmp) { memcpy(tmp, g_jpgBuf, g_jpgLen); src = tmp; }
      doDecode(scale, src, g_jpgLen, ox, oy, false);
      snprintf(g_res.note, sizeof(g_res.note),
        "fresh-malloc+count-only copy=%s scale=%d",
        tmp ? "OK" : "FAIL", scale);
      break;

    case 5: // 32-byte aligned alloc (tests alignment sensitivity)
      tmp = (uint8_t*)heap_caps_aligned_alloc(32, g_jpgLen, MALLOC_CAP_DEFAULT);
      if (tmp) { memcpy(tmp, g_jpgBuf, g_jpgLen); src = tmp; }
      letterbox(ox, oy, sw, sh);
      doDecode(scale, src, g_jpgLen, ox, oy, false);
      snprintf(g_res.note, sizeof(g_res.note),
        "32B-aligned alloc=%s scale=%d",
        tmp ? "OK" : "FAIL(orig)", scale);
      break;

    case 6: // gfx->writeAddrWindow + writePixels (Arduino_TFT path)
      letterbox(ox, oy, sw, sh);
      doDecode(scale, src, g_jpgLen, ox, oy, true);
      snprintf(g_res.note, sizeof(g_res.note),
        "writeAddrWindow+writePixels scale=%d sw=%d sh=%d", scale, sw, sh);
      break;

    case 7: // Force scale=2 — avoids 256-col boundary
      {
        uint16_t iw2 = 0, ih2 = 0;
        TJpgDec.getJpgSize(&iw2, &ih2, g_jpgBuf, (uint32_t)g_jpgLen);
        scale = 2; sw = (int)iw2/2; sh = (int)ih2/2;
        ox = (SCREEN_W - sw) / 2; if (ox < 0) ox = 0;
        oy = (SCREEN_H - sh) / 2; if (oy < 0) oy = 0;
      }
      letterbox(ox, oy, sw, sh);
      doDecode(2, src, g_jpgLen, ox, oy, false);
      snprintf(g_res.note, sizeof(g_res.note),
        "FORCED scale=2 sw=%d sh=%d (bypasses 256-col boundary)", sw, sh);
      break;

    case 8: // Fresh malloc + writePixels combined
      tmp = (uint8_t*)malloc(g_jpgLen);
      if (tmp) { memcpy(tmp, g_jpgBuf, g_jpgLen); src = tmp; }
      letterbox(ox, oy, sw, sh);
      doDecode(scale, src, g_jpgLen, ox, oy, true);
      snprintf(g_res.note, sizeof(g_res.note),
        "fresh-malloc+writePixels copy=%s scale=%d", tmp ? "OK" : "FAIL", scale);
      break;

    default:
      snprintf(g_res.note, sizeof(g_res.note), "unknown test %d", t);
      break;
  }

  if (tmp) free(tmp);

  int exp = expectedMCUs(sw, sh);
  g_res.ran        = true;
  g_res.got        = s_cbCount;
  g_res.expected   = exp;
  g_res.missing    = exp - s_cbCount;
  g_res.lx         = s_lastX;
  g_res.ly         = s_lastY;
  g_res.lw         = s_lastW;
  g_res.lh         = s_lastH;
  g_res.heapBefore = hBefore;
  g_res.heapAfter  = ESP.getFreeHeap();

  Serial.printf("[T%d] got=%d expected=%d missing=%d lastX=%d lastY=%d | %s\n",
    t, g_res.got, g_res.expected, g_res.missing,
    (int)g_res.lx, (int)g_res.ly, g_res.note);
}

// ── Web server ────────────────────────────────────────────────────────────────

WebServer server(80);

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>SF Glitch C3</title>
<style>
body{font-family:monospace;background:#111;color:#ddd;max-width:640px;margin:2em auto;padding:0 1em}
h2{color:#4af;margin-bottom:.3em}
button{background:#1a6;color:#fff;border:none;border-radius:4px;padding:.35em .9em;margin:.2em;cursor:pointer;font-size:.95em}
button:hover{background:#0d5}
button.all{background:#37a}button.all:hover{background:#26c}
.desc{color:#777;font-size:.82em;margin-left:.3em}
#status{color:#fa4;margin:.4em 0;min-height:1.2em}
#result{background:#1a1a1a;border:1px solid #333;border-radius:5px;padding:.8em;margin-top:1em;
        white-space:pre-wrap;font-size:.88em;min-height:3em}
.ok{color:#4f4}.bad{color:#f55}.warn{color:#fa4}
input[type=file]{color:#ccc}
fieldset{border:1px solid #333;border-radius:4px;padding:.6em;margin-bottom:.8em}
legend{color:#4af;font-size:.9em}
</style></head><body>
<h2>&#128247; SF Glitch Test &mdash; C3</h2>
<fieldset>
<legend>Image</legend>
<input type='file' id='f' accept='image/jpeg'>
<button onclick='upload()'>&#8679; Upload</button>
<span id='upMsg'></span>
</fieldset>
<fieldset>
<legend>Tests</legend>
<button class='all' onclick='runAll()'>&#9654;&#9654; Run All</button><br><br>
<button onclick='run(1)'>T1</button><span class='desc'>Baseline &mdash; internal heap src, draw16bitRGBBitmap</span><br>
<button onclick='run(2)'>T2</button><span class='desc'>Fresh malloc copy before decode</span><br>
<button onclick='run(3)'>T3</button><span class='desc'>Count-only callback (zero GFX/SPI)</span><br>
<button onclick='run(4)'>T4</button><span class='desc'>Fresh malloc + count-only</span><br>
<button onclick='run(5)'>T5</button><span class='desc'>32-byte aligned alloc</span><br>
<button onclick='run(6)'>T6</button><span class='desc'>writeAddrWindow + writePixels (Arduino_TFT path)</span><br>
<button onclick='run(7)'>T7</button><span class='desc'>Force scale=2 (bypasses 256-col boundary)</span><br>
<button onclick='run(8)'>T8</button><span class='desc'>Fresh malloc + writePixels combined</span><br>
</fieldset>
<div id='status'></div>
<div id='result'>No test run yet.</div>
<script>
const $=id=>document.getElementById(id);
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function upload(){
  const file=$('f').files[0];
  if(!file){alert('Select a JPEG');return;}
  $('upMsg').textContent=' uploading...';
  const fd=new FormData();fd.append('jpg',file);
  const r=await fetch('/upload',{method:'POST',body:fd});
  const j=await r.json();
  $('upMsg').textContent=j.ok?' \u2713 '+j.msg:' \u2717 '+j.err;
}
async function run(n){
  $('status').textContent='Running T'+n+'...';
  await fetch('/run?t='+n,{method:'POST'});
  await delay(800);
  const j=await(await fetch('/result')).json();
  $('result').innerHTML=fmt(j);
  $('status').textContent='';
}
async function runAll(){
  let out='';
  for(let i=1;i<=8;i++){
    $('status').textContent='Running T'+i+'...';
    await fetch('/run?t='+i,{method:'POST'});
    await delay(900);
    const j=await(await fetch('/result')).json();
    out+=fmt(j)+'\n';
    $('result').innerHTML=out;
  }
  $('status').textContent='Done.';
}
function fmt(j){
  if(!j.ran)return'<span class="warn">T'+j.t+': '+(j.note||'not run')+'</span>';
  const ok=j.missing===0,cls=ok?'ok':'bad';
  return'<b>T'+j.t+'</b>  expected='+j.expected+
    '  got=<b class="'+cls+'">'+j.got+'</b>'+
    (ok?'  <span class="ok">\u2713 ALL MCUs</span>':'  <span class="bad">MISSING '+j.missing+'</span>')+
    '\n  lastMCU x='+j.lx+' y='+j.ly+' w='+j.lw+' h='+j.lh+
    '\n  heap: '+j.hBefore+' \u2192 '+j.hAfter+
    '\n  <span style="color:#888">'+j.note+'</span>';
}
</script>
</body></html>
)HTML";

static void handleIndex() {
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleRun() {
  int t = server.hasArg("t") ? server.arg("t").toInt() : 0;
  if (t == 0) { for (int i = 1; i <= 8; i++) runTest(i); }
  else runTest(t);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleResult() {
  char noteSafe[200] = {};
  int ni = 0;
  for (const char* p = g_res.note; *p && ni < (int)sizeof(noteSafe)-2; p++) {
    if (*p == '"' || *p == '\\') noteSafe[ni++] = '\\';
    noteSafe[ni++] = *p;
  }
  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"t\":%d,\"ran\":%s,\"got\":%d,\"expected\":%d,\"missing\":%d,"
    "\"lx\":%d,\"ly\":%d,\"lw\":%d,\"lh\":%d,"
    "\"hBefore\":%lu,\"hAfter\":%lu,\"note\":\"%s\"}",
    g_res.t,
    g_res.ran ? "true" : "false",
    g_res.got, g_res.expected, g_res.missing,
    (int)g_res.lx, (int)g_res.ly, (int)g_res.lw, (int)g_res.lh,
    (unsigned long)g_res.heapBefore,
    (unsigned long)g_res.heapAfter,
    noteSafe);
  server.send(200, "application/json", buf);
}

static size_t s_upLen = 0;

static void handleUploadBody() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    s_upLen = 0;
    if (g_jpgBuf) { free(g_jpgBuf); g_jpgBuf = nullptr; g_jpgLen = 0; g_hasImage = false; }
    // C3 has no PSRAM; malloc from internal heap. Max needed = 280*240*2 = 131400 bytes.
    g_jpgBuf = (uint8_t*)malloc(MAX_JPG_BYTES);
    Serial.printf("[upload] alloc %lu bytes: %s  freeHeap=%u\n",
      MAX_JPG_BYTES, g_jpgBuf ? "OK" : "FAIL", ESP.getFreeHeap());
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (g_jpgBuf && s_upLen + u.currentSize <= MAX_JPG_BYTES) {
      memcpy(g_jpgBuf + s_upLen, u.buf, u.currentSize);
      s_upLen += u.currentSize;
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (g_jpgBuf && s_upLen > 0) { g_jpgLen = s_upLen; g_hasImage = true; }
    Serial.printf("[upload] end len=%u hasImage=%d\n", (unsigned)s_upLen, (int)g_hasImage);
  }
}

static void handleUploadDone() {
  char buf[120];
  if (g_hasImage) {
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"msg\":\"%u bytes\"}", (unsigned)g_jpgLen);
  } else {
    snprintf(buf, sizeof(buf),
      "{\"ok\":false,\"err\":\"alloc failed or empty (freeHeap=%u)\"}",
      ESP.getFreeHeap());
  }
  server.send(200, "application/json", buf);
}

// ── setup / loop ──────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  gfx->begin();
  gfx->fillScreen(0x0000);
  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(2);
  gfx->setCursor(4, 4);
  gfx->print("GlitchTest C3");

  WiFi.mode(WIFI_STA);
  WiFi.begin(MYSSID, MYPSK);
  Serial.print("WiFi");
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print('.');
  }
  Serial.println();
  Serial.printf("IP: %s  freeHeap: %u\n", WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
  gfx->setCursor(4, 28);
  gfx->print(WiFi.localIP().toString());

  ArduinoOTA.setHostname("sf-glitchtest-c3");
  if (ARDUINO_OTA_PASSWORD && strlen(ARDUINO_OTA_PASSWORD) > 0)
    ArduinoOTA.setPassword(ARDUINO_OTA_PASSWORD);
  ArduinoOTA.begin();

  server.on("/",       HTTP_GET,  handleIndex);
  server.on("/run",    HTTP_POST, handleRun);
  server.on("/result", HTTP_GET,  handleResult);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadBody);
  server.begin();

  Serial.println("Ready.");
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
}
