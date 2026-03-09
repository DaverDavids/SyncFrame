//
// SyncFrame_JpegDebug.ino
// Standalone JPEG decode crash diagnostic sketch.
// Replaces all normal SyncFrame logic with a focused test:
//   1. Connects WiFi + downloads the target image from the SyncFrame server
//   2. Dumps every JPEGDraw callback: x, y, iWidth, iHeight, lastBlock
//   3. Prints heap/PSRAM before/after every stage
//   4. Tests jpeg.setMaxOutputSize(1) (single-MCU mode) vs default (batched) mode
//   5. Never calls gfx->draw16bitRGBBitmap — safe even if JPEGDEC misbehaves
//
// Does NOT include or compile any other SyncFrame .ino/.h files.
// Flash this sketch as its own Arduino project in the SyncFrame_JpegDebug/ folder.
//
// Change the four config constants below to match your environment.
//

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <Secrets.h>

// ---- USER CONFIG -------------------------------------------------------
const char* WIFI_SSID     = MYSSID;          // your SSID
const char* WIFI_PASS     = MYPSK;             // your password (empty = open)
const char* IMAGE_URL     = "https://david:rawson@192.168.6.202:8369/syncframe/photo.800x480.jpg";
// Change IMAGE_URL to the crashing image URL, e.g.:
// "https://192.168.6.202:8369/syncframe/photo.270x480.jpg"
// ------------------------------------------------------------------------

#define SCREEN_W 800
#define SCREEN_H 480

JPEGDEC jpeg;
static uint8_t* jpegBuf = nullptr;
static size_t   jpegLen = 0;

// Per-decode stats gathered inside JPEGDraw
static int      drawCallCount;
static int      drawFirstX, drawFirstY;
static int      drawLastX,  drawLastY;
static int      drawMaxW,   drawMaxH;
static int      drawMinX,   drawMaxXRight; // leftmost and rightmost pixel ever drawn
static int      drawMinY,   drawMaxYBottom;
static bool     drawGotBadBlock;           // any block with x/y/w/h that looks wrong

// -----------------------------------------------------------------------
// Helper: print heap/PSRAM state
// -----------------------------------------------------------------------
static void printMemory(const char* label) {
  Serial.printf("[MEM] %s\n", label);
  Serial.printf("      heap free    : %7lu bytes  (min ever: %7lu)\n",
    (unsigned long)ESP.getFreeHeap(),
    (unsigned long)ESP.getMinFreeHeap());
  Serial.printf("      internal free: %7lu bytes\n",
    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  Serial.printf("      PSRAM free   : %7lu bytes\n",
    (unsigned long)ESP.getFreePsram());
}

// -----------------------------------------------------------------------
// JPEGDraw callback — called by JPEGDEC for every batch of MCUs
// We intentionally do NOT draw to the display here.
// -----------------------------------------------------------------------
static int jpegDrawDebug(JPEGDRAW* pDraw) {
  drawCallCount++;

  int x = pDraw->x;
  int y = pDraw->y;
  int w = pDraw->iWidth;
  int h = pDraw->iHeight;
  int xRight  = x + w;
  int yBottom = y + h;

  // Track first/last call
  if (drawCallCount == 1) {
    drawFirstX = x; drawFirstY = y;
  }
  drawLastX = x; drawLastY = y;

  // Track extents
  if (x < drawMinX)          drawMinX       = x;
  if (xRight  > drawMaxXRight)  drawMaxXRight  = xRight;
  if (y < drawMinY)          drawMinY       = y;
  if (yBottom > drawMaxYBottom) drawMaxYBottom = yBottom;
  if (w > drawMaxW)          drawMaxW = w;
  if (h > drawMaxH)          drawMaxH = h;

  // Flag anything suspicious
  bool bad = false;
  if (x < 0 || y < 0)            bad = true;
  if (w <= 0 || h <= 0)          bad = true;
  if (xRight  > SCREEN_W * 2)    bad = true; // gross overrun
  if (yBottom > SCREEN_H * 2)    bad = true;
  if (pDraw->pPixels == nullptr)  bad = true;
  if (bad) drawGotBadBlock = true;

  // Print every call (verbose — scroll up after crash)
  Serial.printf("  draw #%4d  x=%-5d y=%-5d w=%-5d h=%-5d  xR=%-5d yB=%-5d  last=%d%s\n",
    drawCallCount, x, y, w, h, xRight, yBottom,
    pDraw->iWidthUsed,   // iWidthUsed = lastBlock flag in JPEGDEC
    bad ? "  <<< BAD" : "");

  return 1; // returning 0 aborts decode
}

// -----------------------------------------------------------------------
// Run one decode pass and report results
// -----------------------------------------------------------------------
static void runDecodePass(const char* label, int maxMCUs) {
  Serial.println();
  Serial.printf("============================================================\n");
  Serial.printf(" DECODE PASS: %s  (setMaxOutputSize=%d)\n", label, maxMCUs);
  Serial.printf("============================================================\n");

  // Reset stats
  drawCallCount  = 0;
  drawFirstX = drawFirstY = 0;
  drawLastX  = drawLastY  = 0;
  drawMaxW   = drawMaxH   = 0;
  drawMinX   = 99999;  drawMaxXRight  = -99999;
  drawMinY   = 99999;  drawMaxYBottom = -99999;
  drawGotBadBlock = false;

  printMemory("before openRAM");

  if (!jpeg.openRAM(jpegBuf, (int)jpegLen, jpegDrawDebug)) {
    Serial.println("[JPEG] openRAM FAILED");
    return;
  }

  int imgW = jpeg.getWidth();
  int imgH = jpeg.getHeight();
  Serial.printf("[JPEG] image size: %d x %d\n", imgW, imgH);
  Serial.printf("[JPEG] subSample : 0x%02X\n", jpeg.getSubSample());
  Serial.printf("[JPEG] BPP       : %d\n",     jpeg.getBpp());

  // Compute centered x/y offset
  int xOff = (SCREEN_W - imgW) / 2;  if (xOff < 0) xOff = 0;
  int yOff = (SCREEN_H - imgH) / 2;  if (yOff < 0) yOff = 0;
  Serial.printf("[JPEG] decode offset: x=%d  y=%d\n", xOff, yOff);

  jpeg.setMaxOutputSize(maxMCUs);

  printMemory("after openRAM, before decode");

  unsigned long t0 = millis();
  int rc = jpeg.decode(xOff, yOff, 0);
  unsigned long elapsed = millis() - t0;

  printMemory("after decode");

  Serial.printf("[JPEG] decode rc=%d  elapsed=%lums\n", rc, elapsed);
  Serial.printf("[JPEG] error code: %d\n", jpeg.getLastError());
  Serial.printf("[DRAW] total callbacks : %d\n",   drawCallCount);
  Serial.printf("[DRAW] first block     : x=%d y=%d\n",   drawFirstX, drawFirstY);
  Serial.printf("[DRAW] last  block     : x=%d y=%d\n",   drawLastX,  drawLastY);
  Serial.printf("[DRAW] max block size  : %dx%d\n", drawMaxW, drawMaxH);
  Serial.printf("[DRAW] pixel extents X : %d .. %d  (screen width=%d)\n",
    drawMinX, drawMaxXRight, SCREEN_W);
  Serial.printf("[DRAW] pixel extents Y : %d .. %d  (screen height=%d)\n",
    drawMinY, drawMaxYBottom, SCREEN_H);
  if (drawGotBadBlock)
    Serial.println("[DRAW] *** BAD BLOCK DETECTED — see <<< BAD lines above ***");
  else
    Serial.println("[DRAW] all blocks look sane");

  jpeg.close();
}

// -----------------------------------------------------------------------
// Download image into heap-allocated buffer
// Returns true on success
// -----------------------------------------------------------------------
static bool downloadImage(const char* url) {
  Serial.printf("[HTTP] GET %s\n", url);
  printMemory("before HTTP");

  WiFiClientSecure client;
  client.setInsecure(); // skip cert verification (same as main firmware)

  HTTPClient http;
  http.begin(client, url);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[HTTP] error: %d\n", code);
    http.end();
    return false;
  }

  int contentLen = http.getSize();
  Serial.printf("[HTTP] Content-Length: %d\n", contentLen);

  // Allocate in PSRAM if available, else heap
  if (jpegBuf) { free(jpegBuf); jpegBuf = nullptr; }
  size_t allocSize = (contentLen > 0) ? contentLen : 200000;
  
  // To this (internal RAM first, PSRAM fallback):
  jpegBuf = (uint8_t*)heap_caps_malloc(allocSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!jpegBuf)
      jpegBuf = (uint8_t*)heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!jpegBuf) {
    jpegBuf = (uint8_t*)malloc(allocSize);
  }
  if (!jpegBuf) {
    Serial.println("[HTTP] malloc failed");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  jpegLen = 0;
  int retries = 0;
  while ((http.connected() || stream->available()) && (contentLen < 0 || jpegLen < (size_t)contentLen)) {
    int avail = stream->available();
    if (avail > 0) {
      int toRead = min(avail, (int)(allocSize - jpegLen));
      if (toRead <= 0) break;
      int got = stream->readBytes(jpegBuf + jpegLen, toRead);
      jpegLen += got;
      retries = 0;
    } else {
      delay(1);
      if (++retries > 2000) break;
    }
  }
  http.end();
  Serial.printf("[HTTP] downloaded %u bytes\n", (unsigned)jpegLen);
  printMemory("after HTTP");
  return jpegLen > 0;
}

// -----------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("========================================");
  Serial.println(" SyncFrame JPEG Debug Sketch");
  Serial.println("========================================");

  // WiFi
  Serial.printf("[WIFI] connecting to %s ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500); Serial.print("."); tries++;
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] FAILED — halting");
    while (1) delay(1000);
  }
  Serial.printf("[WIFI] connected, IP=%s\n", WiFi.localIP().toString().c_str());

  // Download
  if (!downloadImage(IMAGE_URL)) {
    Serial.println("[HTTP] download failed — halting");
    while (1) delay(1000);
  }

  // --- Pass 1: default batched mode (iMaxMCUs=1000, same as main firmware) ---
  runDecodePass("DEFAULT (iMaxMCUs=1000, batched)", 1000);

  // Small delay so serial buffer flushes before possible crash on pass 2
  delay(500);
  Serial.flush();

  // --- Pass 2: single-MCU mode (forces one draw call per 16x16 block) ---
  runDecodePass("SINGLE-MCU (iMaxMCUs=1)", 1);

  delay(500);
  Serial.flush();

  // --- Pass 3: 8-MCU batches ---
  runDecodePass("8-MCU BATCHES (iMaxMCUs=8)", 8);

  Serial.println();
  Serial.println("[DONE] All passes completed without crash.");
  Serial.println("       If you never see this line, the crash happened during one of the passes above.");
  Serial.println("       Scroll up to see which pass and which draw# was the last before the crash.");
}

void loop() {
  delay(5000);
  Serial.println("[LOOP] idle — reflash to rerun");
}
