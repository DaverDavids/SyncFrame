#pragma once

#include <Arduino.h>
#include <LittleFS.h>

// Use TJpg_Decoder (Bodmer) instead of JPEGDEC.
// JPEGDEC 1.8.4 has an internal MCU-batching bug that causes a cache/MMU
// crash on ESP32-S3 for images whose width produces >4096 pixels/MCU-row
// but is still in a certain size range (e.g. 270-350px wide). TJpg_Decoder
// uses TJpgD internally, has no such boundary, works fine with PSRAM
// buffers of any size, and is available in the Arduino Library Manager.
//
// Install: Arduino IDE -> Library Manager -> search "TJpg_Decoder" by Bodmer
#include <TJpg_Decoder.h>

// ---------------------------------------------------------------------------
// boardDrawActive
// Set true during board_draw_jpeg() to block board_loop() from triggering
// a re-entrant draw (showCurrentPhoto / showLastPhoto) while the panel DMA
// is being written. A concurrent full-screen write races the DMA scanner
// and produces a wrap-around line-shift artifact (always same height at
// bottom that should be at top) - this flag is the primary fix for that.
//
// The flag is set at entry and cleared at exit of board_draw_jpeg() on
// every code path (including early-return on bad dimensions). The caller
// (.ino) must NOT clear it after calling board_draw_jpeg(); that was the
// previous arrangement but it left a window where the flag stayed true
// between the function returning and the caller's boardDrawActive=false line.
// ---------------------------------------------------------------------------
volatile bool boardDrawActive = false;

// Target identification
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ARDUINO_ESP32C3_DEV)
  #include "config_c3.h"
#elif defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)
  #include "config_s3.h"
#else
  #warning "Board target not automatically identified. Defaulting to ESP32-S3 configuration."
  #include "config_s3.h"
#endif

// ---------------------------------------------------------------------------
// TJpg_Decoder callback - called for each 16x16 decoded MCU block.
// x/y are absolute screen coordinates (TJpgDec adds the drawJpg origin).
// ---------------------------------------------------------------------------
static bool jpegDrawCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* data) {
  if (x >= SCREEN_W || y >= SCREEN_H) return true;
  if (x < 0 || y < 0) return true;

  int clipW = ((int)x + (int)w > SCREEN_W) ? (SCREEN_W - (int)x) : (int)w;
  int clipH = ((int)y + (int)h > SCREEN_H) ? (SCREEN_H - (int)y) : (int)h;

  if (clipW == (int)w && clipH == (int)h) {
    gfx->draw16bitRGBBitmap(x, y, data, w, h);
  } else {
    for (int row = 0; row < clipH; row++) {
      gfx->draw16bitRGBBitmap(x, y + row, data + row * w, clipW, 1);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// board_draw_jpeg
// Decodes a JPEG from a RAM buffer and draws it centred on the display.
// Scaling is power-of-2 only (1x, 1/2, 1/4, 1/8).
//
// THE GLITCH FIX:
// We must NEVER call fillScreen() while the RGB panel DMA is scanning.
// The DMA runs continuously at 16 MHz pixel clock regardless of CPU activity.
// fillScreen writes 384,000 pixels sequentially; if the DMA read pointer
// laps the CPU write pointer (or vice versa) you get a scanline wrap:
// the bottom N rows appear at the top - always the same height because the
// DMA period is fixed. The fix is to only fill the letterbox bars (the
// regions the JPEG does NOT paint), which are small, fast, and don't
// race the scanner because they complete before the DMA reaches them.
// For a full 800x480 image, x==0 and y==0 so NO fillRect fires at all.
// ---------------------------------------------------------------------------
void board_draw_jpeg(const uint8_t* jpg, size_t len) {
  if (!jpg || !len) return;

  boardDrawActive = true;

  // ---- Step 1: read image dimensions without full decode -----------------
  uint16_t imgW = 0, imgH = 0;
  TJpgDec.getJpgSize(&imgW, &imgH, jpg, (uint32_t)len);
  if (imgW == 0 || imgH == 0) {
    boardDrawActive = false;
    return;
  }

  // ---- Step 2: choose best power-of-2 downscale --------------------------
  float aspectSrc = (float)imgW / (float)imgH;
  float aspectDst = (float)SCREEN_W / (float)SCREEN_H;

  int targetWidth, targetHeight;
  if (aspectSrc > aspectDst) {
    targetWidth  = SCREEN_W;
    targetHeight = (int)((float)SCREEN_W / aspectSrc);
  } else {
    targetHeight = SCREEN_H;
    targetWidth  = (int)((float)SCREEN_H * aspectSrc);
  }

  int scales[]  = {1, 2, 4, 8};
  int bestScale = 1;
  int bestDiff  = 99999;
  for (int i = 0; i < 4; i++) {
    int s       = scales[i];
    int scaledW = (int)imgW / s;
    int scaledH = (int)imgH / s;
    int diff    = abs(scaledW - targetWidth) + abs(scaledH - targetHeight);
    if (diff < bestDiff && scaledW <= SCREEN_W + 50 && scaledH <= SCREEN_H + 50) {
      bestScale = s;
      bestDiff  = diff;
    }
  }

  int scaledW = (int)imgW / bestScale;
  int scaledH = (int)imgH / bestScale;

  // ---- Step 3: compute centred origin ------------------------------------
  int x = (SCREEN_W - scaledW) / 2;  if (x < 0) x = 0;
  int y = (SCREEN_H - scaledH) / 2;  if (y < 0) y = 0;

  // ---- Step 4: fill ONLY the letterbox bars (not the whole screen) -------
  // Each fillRect covers a small strip; it completes before the DMA scanner
  // reaches that region, so there is no race. For full-frame images (y==0,
  // x==0) none of these fire.
  if (y > 0) {
    gfx->fillRect(0, 0,           SCREEN_W, y,                         0x0000); // top bar
    gfx->fillRect(0, y + scaledH, SCREEN_W, SCREEN_H - (y + scaledH),  0x0000); // bottom bar
  }
  if (x > 0) {
    gfx->fillRect(0,           y, x,                         scaledH, 0x0000); // left bar
    gfx->fillRect(x + scaledW, y, SCREEN_W - (x + scaledW), scaledH,  0x0000); // right bar
  }

  // ---- Step 5: configure decoder and draw --------------------------------
  TJpgDec.setJpgScale((uint8_t)bestScale);
  TJpgDec.setSwapBytes(JPEG_SWAP_BYTES);
  TJpgDec.setCallback(jpegDrawCallback);
  TJpgDec.drawJpg((int32_t)x, (int32_t)y, jpg, (uint32_t)len);

  // Clear flag here - ownership is inside this function, not the caller.
  boardDrawActive = false;
}

inline void board_draw_jpeg_from_stream(fs::File& f) {
  size_t len = f.size();
  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) return;
  f.read(buf, len);
  vTaskDelay(1);
  board_draw_jpeg(buf, len);
  free(buf);
}

// ---------------------------------------------------------------------------
// board_draw_boot_status
// Draws a status bar at the bottom of the screen during setup.
// ---------------------------------------------------------------------------
void board_draw_boot_status(const char* text) {
  if (boardDrawActive) return;
  gfx->setTextSize(2);

  int16_t  x1, y1;
  uint16_t tw, th;
  gfx->getTextBounds("Ag", 0, 0, &x1, &y1, &tw, &th);

  int padding = 4;
  int barH    = th + (padding * 2);
  int barY    = SCREEN_H - barH - 6;

  gfx->fillRect(0, barY, SCREEN_W, barH, 0x0000);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(10, barY + padding);
  gfx->print(text);
}
