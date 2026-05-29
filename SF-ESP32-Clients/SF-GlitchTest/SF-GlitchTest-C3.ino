/*
 * SF-GlitchTest-C3.ino
 *
 * Targeted glitch tester for ESP32-C3 + ST7789 SPI TFT (280x240).
 * Investigates why TJpgDec stops calling jpegDrawCallback before x=256
 * even though SCREEN_W=280.
 *
 * Each theory is isolated so results can be compared directly.
 *
 * THEORIES
 *  T1  Baseline          PSRAM src, standard draw16bitRGBBitmap callback
 *  T2  InternalRAM       Copy JPEG to internal heap before TJpgDec.drawJpg
 *  T3  NoSPI             Count-only callback (no SPI during decode)
 *  T4  InternalRAM+NoSPI Internal heap copy + count-only (isolates which axis)
 *  T5  AlignedPSRAM      32-byte aligned PSRAM alloc before decode
 *  T6  WritePixels        writeAddrWindow+writePixels instead of draw16bitRGBBitmap
 *  T7  Scale2            Force scale=2 (decode to 140x120, no 256-boundary issue)
 *  T8  InternalRAM+WritePixels  Both T2 and T6 combined
 *
 * WEB UI
 *  GET  /          Control page
 *  POST /upload    Upload JPEG (multipart, field "jpg")
 *  POST /run?t=N   Run test N (1-8), or t=0 for all
 *  GET  /result    JSON result of last completed test
 *
 * OTA: ArduinoOTA hostname "sf-glitchtest-c3"
 *
 * DEPENDS ON: config_c3.h (defines gfx, SCREEN_W, SCREEN_H, JPEG_SWAP_BYTES,
 *             board_init, SF_SPI_BEGIN, SF_SPI_END, SF_GFX_FLUSH)
 *             These are already in your SF-ESP32-Clients folder.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <esp_heap_caps.h>

// Pull board config (gfx, SCREEN_W, SCREEN_H, SF_SPI_BEGIN/END, etc.)
// config_c3.h lives one folder up — adjust path if compiling standalone.
#include "../config_c3.h"

// TJpg_Decoder — intentionally included AFTER board config so any
// compile-time defines (e.g. JD_FASTDECODE) set above take effect.
#include <TJpg_Decoder.h>

#if __has_include(<Secrets.h>)
  #include <Secrets.h>
#else
  const char* MYSSID               = "";
  const char* MYPSK                = "";
  const char* ARDUINO_OTA_PASSWORD = "";
#endif

// ── Uploaded image storage (PSRAM preferred)
static uint8_t* g_jpgBuf   = nullptr;
static size_t   g_jpgLen   = 0;
static bool     g_hasImage = false;

// ── Per-test MCU tracking (reset before every test)
static int      s_cbCount = 0;
static int16_t  s_lastX   = -1, s_lastY  = -1;
static uint16_t s_lastW   = 0,  s_lastH  = 0;

static void resetCb() { s_cbCount=0; s_lastX=-1; s_lastY=-1; s_lastW=0; s_lastH=0; }

// ── Result struct
struct Result {
  int      t;
  bool     ran;
  int      got, expected, missing;
  int16_t  lx, ly; uint16_t lw, lh;
  uint32_t heapBefore, heapAfter;
  char     note[160];
};
static Result g_res = {};

// ── Helpers ────────────────────────────────────────────────

static int expectedMCUs(int w, int h) { return ((w+15)/16)*((h+15)/16); }

// Mirror of board_draw_jpeg scale/origin logic
static bool computeLayout(const uint8_t* jpg, size_t len,
                           int& scale, int& ox, int& oy, int& sw, int& sh) {
  uint16_t iw=0, ih=0;
  TJpgDec.getJpgSize(&iw, &ih, jpg, (uint32_t)len);
  if (!iw||!ih) return false;
  float as=(float)iw/ih, ad=(float)SCREEN_W/SCREEN_H;
  int tw,th;
  if (as>ad){tw=SCREEN_W;th=(int)(SCREEN_W/as);}
  else      {th=SCREEN_H;tw=(int)(SCREEN_H*as);}
  int scales[]={1,2,4,8},best=1,bestD=99999;
  for(int i=0;i<4;i++){
    int s=scales[i];
    int d=abs((int)iw/s-tw)+abs((int)ih/s-th);
    if(d<bestD&&(int)iw/s<=SCREEN_W+50&&(int)ih/s<=SCREEN_H+50){best=s;bestD=d;}
  }
  scale=best; sw=(int)iw/best; sh=(int)ih/best;
  ox=(SCREEN_W-sw)/2; if(ox<0)ox=0;
  oy=(SCREEN_H-sh)/2; if(oy<0)oy=0;
  return true;
}

static void letterbox(int ox,int oy,int sw,int sh){
  if(oy>0){gfx->fillRect(0,0,SCREEN_W,oy,0);gfx->fillRect(0,oy+sh,SCREEN_W,SCREEN_H-(oy+sh),0);}
  if(ox>0){gfx->fillRect(0,oy,ox,sh,0);gfx->fillRect(ox+sw,oy,SCREEN_W-(ox+sw),sh,0);}
}

// ── Callbacks ──────────────────────────────────────────────

static bool g_countOnly = false;

static bool jpegCB(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* data) {
  s_cbCount++;
  s_lastX=x; s_lastY=y; s_lastW=w; s_lastH=h;
  if(g_countOnly) return true;
  if(x>=SCREEN_W||y>=SCREEN_H||x<0||y<0) return true;
  int cw=(x+w>SCREEN_W)?SCREEN_W-x:(int)w;
  int ch=(y+h>SCREEN_H)?SCREEN_H-y:(int)h;
  SF_SPI_BEGIN();
  if(cw==(int)w&&ch==(int)h){
    gfx->draw16bitRGBBitmap(x,y,data,w,h);
  } else {
    for(int r=0;r<ch;r++) gfx->draw16bitRGBBitmap(x,y+r,data+r*w,cw,1);
  }
  SF_SPI_END();
  return true;
}

static bool jpegCB_writePixels(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* data) {
  s_cbCount++;
  s_lastX=x; s_lastY=y; s_lastW=w; s_lastH=h;
  if(x>=SCREEN_W||y>=SCREEN_H||x<0||y<0) return true;
  int cw=(x+w>SCREEN_W)?SCREEN_W-x:(int)w;
  int ch=(y+h>SCREEN_H)?SCREEN_H-y:(int)h;
  gfx->startWrite();
  for(int r=0;r<ch;r++){
    gfx->writeAddrWindow(x,y+r,cw,1);
    gfx->writePixels(data+r*w, cw, true);
  }
  gfx->endWrite();
  return true;
}

static void doDecode(int scale, const uint8_t* src, size_t len, int ox, int oy, bool wpx) {
  TJpgDec.setJpgScale((uint8_t)scale);
  TJpgDec.setSwapBytes(JPEG_SWAP_BYTES);
  TJpgDec.setCallback(wpx ? jpegCB_writePixels : jpegCB);
  TJpgDec.drawJpg(ox, oy, src, (uint32_t)len);
  SF_GFX_FLUSH();
}

// ── Tests ──────────────────────────────────────────────────

static void runTest(int t) {
  if(!g_hasImage){
    g_res={t,false,0,0,0,-1,-1,0,0,0,0,{}};
    snprintf(g_res.note,sizeof(g_res.note),"no image");
    return;
  }
  resetCb();
  g_countOnly=false;

  int scale,ox,oy,sw,sh;
  if(!computeLayout(g_jpgBuf,g_jpgLen,scale,ox,oy,sw,sh)){
    g_res={t,false,0,0,0,-1,-1,0,0,0,0,{}};
    snprintf(g_res.note,sizeof(g_res.note),"getJpgSize failed");
    return;
  }

  uint32_t hBefore = ESP.getFreeHeap();
  const uint8_t* src = g_jpgBuf;
  uint8_t* tmpBuf    = nullptr;
  char noteLocal[160] = {};

  switch(t) {
    case 1: // Baseline
      letterbox(ox,oy,sw,sh);
      doDecode(scale, src, g_jpgLen, ox, oy, false);
      snprintf(noteLocal,sizeof(noteLocal),
        "PSRAM src draw16bitRGBBitmap scale=%d sw=%d sh=%d ox=%d oy=%d",scale,sw,sh,ox,oy);
      break;

    case 2: // Internal RAM copy
      tmpBuf=(uint8_t*)malloc(g_jpgLen);
      if(tmpBuf){ memcpy(tmpBuf,g_jpgBuf,g_jpgLen); src=tmpBuf; }
      letterbox(ox,oy,sw,sh);
      doDecode(scale, src, g_jpgLen, ox, oy, false);
      snprintf(noteLocal,sizeof(noteLocal),
        "internalRAM copy=%s scale=%d sw=%d sh=%d", tmpBuf?"YES":"FAILED(PSRAM fallback)",scale,sw,sh);
      break;

    case 3: // Count-only, PSRAM src
      g_countOnly=true;
      doDecode(scale, src, g_jpgLen, ox, oy, false);
      snprintf(noteLocal,sizeof(noteLocal),
        "count-only noSPI PSRAM src scale=%d sw=%d sh=%d",scale,sw,sh);
      break;

    case 4: // Internal RAM + count-only
      g_countOnly=true;
      tmpBuf=(uint8_t*)malloc(g_jpgLen);
      if(tmpBuf){ memcpy(tmpBuf,g_jpgBuf,g_jpgLen); src=tmpBuf; }
      doDecode(scale, src, g_jpgLen, ox, oy, false);
      snprintf(noteLocal,sizeof(noteLocal),
        "internalRAM+count-only copy=%s scale=%d", tmpBuf?"YES":"FAILED",scale);
      break;

    case 5: // 32-byte aligned PSRAM
      tmpBuf=(uint8_t*)heap_caps_aligned_alloc(32,g_jpgLen,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
      if(tmpBuf){ memcpy(tmpBuf,g_jpgBuf,g_jpgLen); src=tmpBuf; }
      letterbox(ox,oy,sw,sh);
      doDecode(scale, src, g_jpgLen, ox, oy, false);
      snprintf(noteLocal,sizeof(noteLocal),
        "32B-aligned PSRAM alloc=%s scale=%d", tmpBuf?"YES":"FAILED(orig)",scale);
      break;

    case 6: // writePixels callback
      letterbox(ox,oy,sw,sh);
      doDecode(scale, src, g_jpgLen, ox, oy, true);
      snprintf(noteLocal,sizeof(noteLocal),
        "writePixels path PSRAM src scale=%d sw=%d sh=%d",scale,sw,sh);
      break;

    case 7: // Force scale=2
      {
        uint16_t iw=0,ih=0;
        TJpgDec.getJpgSize(&iw,&ih,g_jpgBuf,(uint32_t)g_jpgLen);
        scale=2; sw=(int)iw/2; sh=(int)ih/2;
        ox=(SCREEN_W-sw)/2; if(ox<0)ox=0;
        oy=(SCREEN_H-sh)/2; if(oy<0)oy=0;
      }
      letterbox(ox,oy,sw,sh);
      doDecode(2, src, g_jpgLen, ox, oy, false);
      snprintf(noteLocal,sizeof(noteLocal),
        "FORCED scale=2 sw=%d sh=%d (bypasses 256-col boundary)",sw,sh);
      break;

    case 8: // Internal RAM + writePixels combined
      tmpBuf=(uint8_t*)malloc(g_jpgLen);
      if(tmpBuf){ memcpy(tmpBuf,g_jpgBuf,g_jpgLen); src=tmpBuf; }
      letterbox(ox,oy,sw,sh);
      doDecode(scale, src, g_jpgLen, ox, oy, true);
      snprintf(noteLocal,sizeof(noteLocal),
        "internalRAM+writePixels copy=%s scale=%d", tmpBuf?"YES":"FAILED",scale);
      break;

    default:
      snprintf(noteLocal,sizeof(noteLocal),"unknown test %d",t);
      break;
  }

  if(tmpBuf) free(tmpBuf);

  int exp=expectedMCUs(sw,sh);
  g_res.t          = t;
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
  strncpy(g_res.note, noteLocal, sizeof(g_res.note)-1);

  Serial.printf("[T%d] got=%d expected=%d missing=%d lastX=%d lastY=%d | %s\n",
    t, g_res.got, g_res.expected, g_res.missing, g_res.lx, g_res.ly, g_res.note);
}

// ── Web server ─────────────────────────────────────────────

WebServer server(80);

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>SF Glitch C3</title>
<style>
  body{font-family:monospace;background:#111;color:#ddd;max-width:640px;margin:2em auto;padding:0 1em}
  h2{color:#4af;margin-bottom:.3em}
  hr{border-color:#333}
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
<button class='all' onclick='runAll()'>&#9654;&#9654; Run All</button>
<br><br>
<button onclick='run(1)'>T1</button><span class='desc'>Baseline &mdash; PSRAM src, draw16bitRGBBitmap</span><br>
<button onclick='run(2)'>T2</button><span class='desc'>Internal RAM copy before decode</span><br>
<button onclick='run(3)'>T3</button><span class='desc'>Count-only callback (zero SPI during decode)</span><br>
<button onclick='run(4)'>T4</button><span class='desc'>Internal RAM + count-only (both isolated)</span><br>
<button onclick='run(5)'>T5</button><span class='desc'>32-byte aligned PSRAM alloc</span><br>
<button onclick='run(6)'>T6</button><span class='desc'>writeAddrWindow + writePixels callback</span><br>
<button onclick='run(7)'>T7</button><span class='desc'>Force scale=2 (bypasses 256-col boundary)</span><br>
<button onclick='run(8)'>T8</button><span class='desc'>Internal RAM + writePixels (combined)</span><br>
</fieldset>

<div id='status'></div>
<div id='result'>No test run yet.</div>

<script>
const $ = id => document.getElementById(id);
const delay = ms => new Promise(r => setTimeout(r, ms));

async function upload() {
  const file = $('f').files[0];
  if (!file) { alert('Select a JPEG'); return; }
  $('upMsg').textContent = ' uploading...';
  const fd = new FormData(); fd.append('jpg', file);
  const r = await fetch('/upload', { method: 'POST', body: fd });
  const j = await r.json();
  $('upMsg').textContent = j.ok ? ' \u2713 ' + j.msg : ' \u2717 ' + j.err;
}

async function run(n) {
  $('status').textContent = 'Running T' + n + '...';
  await fetch('/run?t=' + n, { method: 'POST' });
  await delay(700);
  const r = await fetch('/result');
  const j = await r.json();
  $('result').innerHTML = fmt(j);
  $('status').textContent = '';
}

async function runAll() {
  let out = '';
  for (let i = 1; i <= 8; i++) {
    $('status').textContent = 'Running T' + i + '...';
    await fetch('/run?t=' + i, { method: 'POST' });
    await delay(900);
    const r = await fetch('/result');
    const j = await r.json();
    out += fmt(j) + '\n';
    $('result').innerHTML = out;
  }
  $('status').textContent = 'Done.';
}

function fmt(j) {
  if (!j.ran) return '<span class="warn">T' + j.t + ': ' + (j.note || 'not run') + '</span>';
  const ok = j.missing === 0;
  const cls = ok ? 'ok' : 'bad';
  return '<b>T' + j.t + '</b>  expected=' + j.expected +
         '  got=<b class="' + cls + '">' + j.got + '</b>' +
         (ok ? '  <span class="ok">\u2713 ALL MCUs</span>'
             : '  <span class="bad">MISSING ' + j.missing + '</span>') +
         '\n  lastMCU x=' + j.lx + ' y=' + j.ly + ' w=' + j.lw + ' h=' + j.lh +
         '\n  heap: ' + j.hBefore + ' \u2192 ' + j.hAfter +
         '\n  <span style="color:#888">' + j.note + '</span>';
}
</script>
</body></html>
)HTML";

static void handleIndex() { server.send_P(200,"text/html",INDEX_HTML); }

static void handleRun() {
  int t = server.hasArg("t") ? server.arg("t").toInt() : 0;
  if (t==0) { for(int i=1;i<=8;i++) runTest(i); }
  else runTest(t);
  server.send(200,"application/json","{\"ok\":true}");
}

static void handleResult() {
  String j="{";
  j+="\"t\":"+String(g_res.t)+",";
  j+="\"ran\":"+(g_res.ran?"true":"false")+",";
  j+="\"got\":"+String(g_res.got)+",";
  j+="\"expected\":"+String(g_res.expected)+",";
  j+="\"missing\":"+String(g_res.missing)+",";
  j+="\"lx\":"+String(g_res.lx)+",";
  j+="\"ly\":"+String(g_res.ly)+",";
  j+="\"lw\":"+String(g_res.lw)+",";
  j+="\"lh\":"+String(g_res.lh)+",";
  j+="\"hBefore\":"+String(g_res.heapBefore)+",";
  j+="\"hAfter\":"+String(g_res.heapAfter)+",";
  j+="\"note\":\"";
  for(const char*p=g_res.note;*p;p++){
    if(*p=='"')j+="\\\""; else if(*p=='\\')j+="\\\\"; else j+=*p;
  }
  j+="\"}";
  server.send(200,"application/json",j);
}

static size_t s_upLen=0;

static void handleUploadBody() {
  HTTPUpload& u=server.upload();
  if(u.status==UPLOAD_FILE_START){
    s_upLen=0;
    if(g_jpgBuf){heap_caps_free(g_jpgBuf);g_jpgBuf=nullptr;g_jpgLen=0;g_hasImage=false;}
    g_jpgBuf=(uint8_t*)heap_caps_malloc(256*1024,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!g_jpgBuf) g_jpgBuf=(uint8_t*)malloc(256*1024);
  } else if(u.status==UPLOAD_FILE_WRITE){
    if(g_jpgBuf && s_upLen+u.currentSize<=256*1024){
      memcpy(g_jpgBuf+s_upLen,u.buf,u.currentSize);
      s_upLen+=u.currentSize;
    }
  } else if(u.status==UPLOAD_FILE_END){
    if(g_jpgBuf && s_upLen>0){g_jpgLen=s_upLen;g_hasImage=true;}
  }
}

static void handleUploadDone() {
  server.send(200,"application/json",
    g_hasImage
      ? ("{\"ok\":true,\"msg\":\""+String(g_jpgLen)+" bytes\"}")
      : "{\"ok\":false,\"err\":\"empty or alloc failed\"}");
}

// ── setup / loop ───────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  board_init();
  gfx->fillScreen(0x0000);
  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(2);
  gfx->setCursor(4,4);
  gfx->print("GlitchTest C3");
  SF_GFX_FLUSH();

  WiFi.mode(WIFI_STA);
  WiFi.begin(MYSSID, MYPSK);
  Serial.print("WiFi");
  for(int i=0;i<30&&WiFi.status()!=WL_CONNECTED;i++){delay(500);Serial.print('.');}
  Serial.println();
  Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  gfx->setCursor(4,28);
  gfx->print(WiFi.localIP().toString());
  SF_GFX_FLUSH();

  ArduinoOTA.setHostname("sf-glitchtest-c3");
  if(ARDUINO_OTA_PASSWORD&&strlen(ARDUINO_OTA_PASSWORD)>0)
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
