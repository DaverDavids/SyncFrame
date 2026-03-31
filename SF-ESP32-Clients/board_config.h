#pragma once

#include <Arduino.h>

// Use TJpg_Decoder (Bodmer) instead of JPEGDEC.
// JPEGDEC 1.8.4 has an internal MCU-batching bug that causes a cache/MMU
// crash on ESP32-S3 for images whose width produces >4096 pixels/MCU-row
// but is still in a certain size range (e.g. 270-350px wide). TJpg_Decoder
// uses TJpgD internally, has no such boundary, works fine with PSRAM
// buffers of any size, and is available in the Arduino Library Manager.
//
// Install: Arduino IDE -> Library Manager -> search "TJpg_Decoder" by Bodmer
#include <TJpg_Decoder.h>

// Flag to guard board_loop() during JPEG decode
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
// TJpg_Decoder callback - called for each decoded block.
// x/y are already absolute screen coordinates (TJpgDec adds the drawJpg
// origin offset automatically). We just pass the block straight to GFX.
// Clipping guards are retained in case a block overhangs the screen edge.
// ---------------------------------------------------------------------------
static bool jpegDrawCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* data) {
  if (x >= SCREEN_W || y >= SCREEN_H) return true;  // fully off-screen
  if (x < 0 || y < 0) return true;                  // shouldn't happen

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
// No flush() needed - single-buffer mode writes directly to panel DMA FIFO.
// ---------------------------------------------------------------------------
void board_draw_jpeg(const uint8_t* jpg, size_t len) {
  if (!jpg || !len) return;

  // ---- Step 1: read image dimensions without full decode -----------------
  uint16_t imgW = 0, imgH = 0;
  TJpgDec.getJpgSize(&imgW, &imgH, jpg, (uint32_t)len);
  if (imgW == 0 || imgH == 0) return;

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

  // ---- Step 4: configure decoder and draw --------------------------------
  TJpgDec.setJpgScale((uint8_t)bestScale);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(jpegDrawCallback);

  // Guard board_loop() during decode
  boardDrawActive = true;
  TJpgDec.drawJpg((int32_t)x, (int32_t)y, jpg, (uint32_t)len);
  boardDrawActive = false;
}

// ---------------------------------------------------------------------------
// board_draw_boot_status
// Draws a status bar at the bottom of the screen.
// No flush() needed - single-buffer mode.
// ---------------------------------------------------------------------------
void board_draw_boot_status(const char* text) {
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
