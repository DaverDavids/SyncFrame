#pragma once

#include <Arduino.h>

// Use ESP-IDF's built-in JPEG decoder (esp_jpeg) instead of JPEGDEC.
// esp_jpeg is stable for all image sizes/aspect ratios, lives in the ESP32
// Arduino core, needs no external library, and supports PSRAM output buffers.
#include <esp_jpeg_dec.h>

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
// board_draw_jpeg
// Decodes a JPEG from RAM and draws it centred on the display.
// Scaling is limited to the four power-of-2 factors that JPEGDEC used to
// support (1x, 1/2, 1/4, 1/8) so that scaling behaviour is unchanged.
// Output pixels are RGB565 Little-Endian (same as JPEGDEC's default output).
// ---------------------------------------------------------------------------
void board_draw_jpeg(const uint8_t* jpg, size_t len) {
  if (!jpg || !len) return;

  // ---- Step 1: peek at image dimensions without full decode ---------------
  // esp_jpeg_get_image_info() parses just the JPEG header.
  esp_jpeg_image_cfg_t infoCfg = {};
  infoCfg.indata       = (uint8_t*)jpg;
  infoCfg.indata_size  = len;
  infoCfg.outbuf       = nullptr;   // nullptr = info-only, no decode
  infoCfg.outbuf_size  = 0;

  esp_jpeg_image_output_t info = {};
  esp_err_t infoErr = esp_jpeg_decode(&infoCfg, &info);
  // info-only call returns ESP_ERR_NO_MEM (no output buffer) but still fills
  // info.width / info.height — that is expected behaviour.
  if (info.width == 0 || info.height == 0) {
    // Couldn't even parse header — bail.
    return;
  }

  int w = (int)info.width;
  int h = (int)info.height;

  // ---- Step 2: choose the best power-of-2 downscale ----------------------
  float aspectSrc = (float)w / (float)h;
  float aspectDst = (float)SCREEN_W / (float)SCREEN_H;

  int targetWidth, targetHeight;
  if (aspectSrc > aspectDst) {
    targetWidth  = SCREEN_W;
    targetHeight = (int)((float)SCREEN_W / aspectSrc);
  } else {
    targetHeight = SCREEN_H;
    targetWidth  = (int)((float)SCREEN_H * aspectSrc);
  }

  // esp_jpeg supports scale factors 1, 2, 4, 8 via the scale_numerator field
  // (numerator/8: 8=1x, 4=1/2, 2=1/4, 1=1/8)
  int scales[]       = {1,  2,  4,  8};
  int numerators[]   = {8,  4,  2,  1}; // scale_numerator for each
  int bestIdx        = 0;
  int bestDiff       = 99999;

  for (int i = 0; i < 4; i++) {
    int s      = scales[i];
    int scaledW = w / s;
    int scaledH = h / s;
    int diff   = abs(scaledW - targetWidth) + abs(scaledH - targetHeight);
    if (diff < bestDiff && scaledW <= SCREEN_W + 50 && scaledH <= SCREEN_H + 50) {
      bestIdx  = i;
      bestDiff = diff;
    }
  }

  int bestScale    = scales[bestIdx];
  int scaledW      = w / bestScale;
  int scaledH      = h / bestScale;
  int scaleNum     = numerators[bestIdx]; // 8=1x, 4=half, 2=quarter, 1=eighth

  // ---- Step 3: allocate output buffer (RGB565, 2 bytes/pixel) in PSRAM ---
  size_t outBytes = (size_t)scaledW * (size_t)scaledH * 2;
  uint16_t* outBuf = (uint16_t*)heap_caps_malloc(outBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!outBuf) outBuf = (uint16_t*)malloc(outBytes);
  if (!outBuf) {
    // Out of memory — nothing we can do
    return;
  }

  // ---- Step 4: full decode ------------------------------------------------
  esp_jpeg_image_cfg_t decCfg = {};
  decCfg.indata           = (uint8_t*)jpg;
  decCfg.indata_size      = len;
  decCfg.outbuf           = (uint8_t*)outBuf;
  decCfg.outbuf_size      = outBytes;
  decCfg.out_format       = JPEG_IMAGE_FORMAT_RGB565;
  decCfg.out_scale.width  = scaledW;
  decCfg.out_scale.height = scaledH;
  // Ask the decoder to swap bytes to Little-Endian so GFX gets native RGB565
  decCfg.flags            = JPEG_FLAGS_SWAP_RB; // keeps R/B in correct order for RGB565

  esp_jpeg_image_output_t decOut = {};
  esp_err_t decErr = esp_jpeg_decode(&decCfg, &decOut);

  if (decErr != ESP_OK && decErr != ESP_ERR_NO_MEM) {
    // Decode failed
    free(outBuf);
    return;
  }

  // decOut.width / decOut.height reflect actual decoded size (may differ
  // slightly from requested if image dimensions aren't divisible by scale)
  int dw = (decOut.width  > 0) ? (int)decOut.width  : scaledW;
  int dh = (decOut.height > 0) ? (int)decOut.height : scaledH;

  // ---- Step 5: centre and draw -------------------------------------------
  int x = (SCREEN_W - dw) / 2;  if (x < 0) x = 0;
  int y = (SCREEN_H - dh) / 2;  if (y < 0) y = 0;

  // Clip to screen (in case decoded dimensions somehow exceeded SCREEN bounds)
  int drawW = (x + dw > SCREEN_W) ? (SCREEN_W - x) : dw;
  int drawH = (y + dh > SCREEN_H) ? (SCREEN_H - y) : dh;

  gfx->fillScreen(0x0000);

  if (drawW == dw) {
    // Full-width draw — one call per image height slice would work but
    // draw16bitRGBBitmap accepts the full block at once.
    gfx->draw16bitRGBBitmap(x, y, outBuf, drawW, drawH);
  } else {
    // Clipped width — draw row by row
    for (int row = 0; row < drawH; row++) {
      gfx->draw16bitRGBBitmap(x, y + row, outBuf + row * dw, drawW, 1);
    }
  }

  free(outBuf);

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)
  gfx->flush();
#endif
}

// ---------------------------------------------------------------------------
// board_draw_boot_status
// Draws a status bar at the bottom of the screen.
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

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)
  gfx->flush();
#endif
}
