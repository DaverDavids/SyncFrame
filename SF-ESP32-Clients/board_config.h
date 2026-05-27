#pragma once

#include <Arduino.h>
// Use TJpg_Decoder (Bodmer) instead of JPEGDEC.
// JPEGDEC 1.8.4 has an internal MCU-batching bug that causes a cache/MMU
// crash on ESP32-S3 for images whose width produces >4096 pixels/MCU-row
// but is still in a certain size range (e.g. 270-350px wide). TJpgDec
// uses TJpgD internally, has no such boundary, works fine with PSRAM
// buffers of any size, and is available in the Arduino Library Manager.
//
// Install: Arduino IDE -> Library Manager -> search "TJpg_Decoder" by Bodmer
#include <TJpg_Decoder.h>

// ---------------------------------------------------------------------------
// boardDrawActive
// Set true during board_draw_jpeg() to block board_loop() from triggering
// a re-entrant draw (showCurrentPhoto / showLastPhoto) while a frame is
// being written to the back buffer.
//
// In double-buffer (S3) mode this guards the back buffer write;
// in single-buffer (C3) mode it guards the live SPI framebuffer write.
//
// The flag is set at entry and cleared at exit of board_draw_jpeg() on
// every code path (including early-return on bad dimensions).
// ---------------------------------------------------------------------------
volatile bool boardDrawActive = false;

// ---------------------------------------------------------------------------
// Target identification
// ---------------------------------------------------------------------------
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ARDUINO_ESP32C3_DEV)
  #include "config_c3.h"
  #define BOARD_IS_C3 1
  #define BOARD_IS_S3 0
#elif defined(BOARD_TARGET_VIEWE7)
  #include "config_viewe7.h"
  #define BOARD_IS_C3 0
  #define BOARD_IS_S3 1
#elif defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)
  #include "config_s3.h"
  #define BOARD_IS_C3 0
  #define BOARD_IS_S3 1
#else
  #warning "Board target not automatically identified. Defaulting to ESP32-S3 configuration."
  #include "config_s3.h"
  #define BOARD_IS_C3 0
  #define BOARD_IS_S3 1
#endif

// ---------------------------------------------------------------------------
// SPI transaction guard (ESP32-C3 only)
// ---------------------------------------------------------------------------
#if BOARD_IS_C3
  #include <SPI.h>
  static SPISettings _sfSpiSettings(80000000, MSBFIRST, SPI_MODE0);
  #define SF_SPI_BEGIN() SPI.beginTransaction(_sfSpiSettings)
  #define SF_SPI_END()   SPI.endTransaction()
#else
  #define SF_SPI_BEGIN() do {} while(0)
  #define SF_SPI_END()   do {} while(0)
#endif

// ---------------------------------------------------------------------------
// SF_GFX_FLUSH — atomic front/back buffer swap (S3 double-buffer only).
//
// Arduino_RGB_Display::flush() swaps the DMA pointer from the front buffer
// to the fully-rendered back buffer in one operation.  The DMA engine never
// sees a partially-drawn frame.  On C3 (SPI TFT, no back buffer) this is a
// no-op — draw16bitRGBBitmap writes directly to the display.
// ---------------------------------------------------------------------------
#if BOARD_IS_S3
  #define SF_GFX_FLUSH() gfx->flush()
#else
  #define SF_GFX_FLUSH() do {} while(0)
#endif

// ---------------------------------------------------------------------------
// TJpg_Decoder callback — called for each 16x16 decoded MCU block.
// Writes into the GFX back buffer (S3) or directly to display (C3).
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
//
// S3 double-buffer flow:
//   1. All MCU blocks are decoded and written into the BACK buffer via
//      jpegDrawCallback -> gfx->draw16bitRGBBitmap().
//   2. gfx->flush() (SF_GFX_FLUSH) atomically swaps back->front so the
//      DMA engine transitions from the previous complete frame to the new
//      complete frame in one operation.  No partial frames ever reach DMA.
//
// C3 SPI flow:
//   SF_SPI_BEGIN/END hold the Arduino SPI mutex for the full frame to block
//   WiFi from stealing the bus mid-MCU-block.  SF_GFX_FLUSH is a no-op.
// ---------------------------------------------------------------------------
void board_draw_jpeg(const uint8_t* jpg, size_t len) {
  if (!jpg || !len) return;

  boardDrawActive = true;

  // Step 1: read image dimensions
  uint16_t imgW = 0, imgH = 0;
  TJpgDec.getJpgSize(&imgW, &imgH, jpg, (uint32_t)len);
  if (imgW == 0 || imgH == 0) {
    boardDrawActive = false;
    return;
  }

  // Step 2: choose best power-of-2 downscale
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

  // Step 3: compute centred origin
  int x = (SCREEN_W - scaledW) / 2;  if (x < 0) x = 0;
  int y = (SCREEN_H - scaledH) / 2;  if (y < 0) y = 0;

  // Step 4: acquire SPI bus (C3) or no-op (S3)
  SF_SPI_BEGIN();
#if BOARD_IS_C3
  gfx->fillRect(0, 0, 1, 1, 0x0000);  // address resync
#endif

  // Step 5: fill letterbox bars into back buffer (S3) or display (C3)
  if (y > 0) {
    gfx->fillRect(0, 0,           SCREEN_W, y,                         0x0000);
    gfx->fillRect(0, y + scaledH, SCREEN_W, SCREEN_H - (y + scaledH),  0x0000);
  }
  if (x > 0) {
    gfx->fillRect(0,           y, x,                         scaledH, 0x0000);
    gfx->fillRect(x + scaledW, y, SCREEN_W - (x + scaledW), scaledH,  0x0000);
  }

  // Step 6: decode JPEG — MCU blocks land in back buffer (S3) or display (C3)
  TJpgDec.setJpgScale((uint8_t)bestScale);
  TJpgDec.setSwapBytes(JPEG_SWAP_BYTES);
  TJpgDec.setCallback(jpegDrawCallback);
  TJpgDec.drawJpg((int32_t)x, (int32_t)y, jpg, (uint32_t)len);

  // Step 7: atomic buffer swap (S3) — DMA now scans the fully-rendered frame.
  // This is the critical call that was previously missing.
  // C3: no-op (SPI display, no back buffer).
  SF_GFX_FLUSH();

  SF_SPI_END();

  boardDrawActive = false;
}

// ---------------------------------------------------------------------------
// board_draw_boot_status
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

  // Flush to front buffer so boot status text is visible immediately.
  // Safe here because boardDrawActive was false when we entered.
  SF_GFX_FLUSH();
}
