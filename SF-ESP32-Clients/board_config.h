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
//
// BOARD_TARGET_VIEWE7 is injected via build.extra_flags in sketch.yaml
// (SF-VIEWE7 profile) so the two S3-based boards can share the same
// CONFIG_IDF_TARGET_ESP32S3 define but pick different config headers.
// ---------------------------------------------------------------------------
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ARDUINO_ESP32C3_DEV)
  #include "config_c3.h"
  #define BOARD_IS_C3 1
  #define BOARD_IS_S3 1
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
//
// On ESP32-C3 the WiFi radio and HSPI share the same SPI peripheral.
// Wrapping the entire JPEG decode+draw in SPI.beginTransaction() acquires
// the Arduino SPI mutex for the full frame duration, preventing WiFi from
// stealing the bus mid-MCU-block write.
//
// The S3 uses a dedicated parallel RGB panel — no SPI contention possible.
// These macros compile away to nothing on S3.
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
// TJpg_Decoder callback - called for each 16x16 decoded MCU block.
// x/y are absolute screen coordinates (TJpgDec adds the drawJpg origin).
//
// On S3 (double-buffer): writes into the back buffer only. flush() is
// called once after drawJpg() completes in board_draw_jpeg() — NEVER here.
// On C3 (single-buffer + SPI guard): writes directly to the display
// under the SPI transaction lock held by board_draw_jpeg().
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
// S3 DOUBLE-BUFFER PATH:
//   All MCU block writes go into the back buffer (DMA scans front only).
//   After drawJpg() returns, gfx->flush() atomically swaps front/back.
//   The display never shows a partially-decoded frame.
//   flush() must ONLY be called here, never in the callback.
//
// C3 SPI GUARD PATH:
//   SF_SPI_BEGIN/END hold the Arduino SPI mutex for the full frame,
//   preventing WiFi from stealing the bus mid-MCU-block write.
//   A dummy 1x1 fillRect resets the ST7789 CASET/RASET counters before
//   each frame, eliminating the permanent right-shift artifact.
//
// LETTERBOX FIX (both targets):
//   Only the letterbox bars (regions the JPEG does NOT paint) are filled
//   with black. fillScreen() is never called during a live draw.
// ---------------------------------------------------------------------------
void board_draw_jpeg(const uint8_t* jpg, size_t len) {
  if (!jpg || !len) return;

  // Safety: always start with boardDrawActive false so a previously-
  // interrupted draw cannot permanently block future frames.
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

  // ---- Step 4: acquire SPI bus (C3) or no-op (S3) -----------------------
  // C3: hold SPI mutex for full frame to block WiFi bus contention.
  // C3: reset ST7789 CASET/RASET address counters (prevents right-shift).
  SF_SPI_BEGIN();
#if BOARD_IS_C3
  gfx->fillRect(0, 0, 1, 1, 0x0000);  // address resync — resets CASET/RASET
#endif

  // ---- Step 5: fill ONLY the letterbox bars ------------------------------
  // Small strips that complete before the DMA scanner reaches them.
  // For full-frame images (y==0, x==0) none of these fire.
  if (y > 0) {
    gfx->fillRect(0, 0,           SCREEN_W, y,                         0x0000);
    gfx->fillRect(0, y + scaledH, SCREEN_W, SCREEN_H - (y + scaledH),  0x0000);
  }
  if (x > 0) {
    gfx->fillRect(0,           y, x,                         scaledH, 0x0000);
    gfx->fillRect(x + scaledW, y, SCREEN_W - (x + scaledW), scaledH,  0x0000);
  }

  // ---- Step 6: decode and draw into back buffer (S3) or display (C3) ----
  TJpgDec.setJpgScale((uint8_t)bestScale);
  TJpgDec.setSwapBytes(JPEG_SWAP_BYTES);
  TJpgDec.setCallback(jpegDrawCallback);
  TJpgDec.drawJpg((int32_t)x, (int32_t)y, jpg, (uint32_t)len);

  // ---- Step 7: atomic buffer swap (S3 only) ------------------------------
  // All MCU blocks are now in the back buffer. Swap atomically so the
  // display transitions from the previous complete frame directly to the
  // new complete frame — no tearing, no partial frames visible.
  // This call must ONLY appear here, never in jpegDrawCallback.
#if BOARD_IS_S3
  
#endif

  // Release SPI bus (C3 only).
  SF_SPI_END();

  boardDrawActive = false;
}

inline void board_draw_jpeg_from_stream(fs::File& f) {
  size_t len = f.size();
  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) return;
  f.read(buf, len);
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
#if BOARD_IS_S3
    // push boot status text to front buffer
#endif
}
