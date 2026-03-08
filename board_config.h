#pragma once

#include <Arduino.h>
#include <JPEGDEC.h>

// Target identification
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ARDUINO_ESP32C3_DEV)
  #include "config_c3.h"
#elif defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)
  #include "config_s3.h"
#else
  #warning "Board target not automatically identified. Defaulting to ESP32-S3 configuration."
  #include "config_s3.h"
#endif

// Shared unified JPEG drawing logic
static JPEGDEC jpeg;

static int JPEGDraw(JPEGDRAW *pDraw) {
  gfx->draw16bitRGBBitmap(
    pDraw->x, pDraw->y,
    (uint16_t*)pDraw->pPixels,
    pDraw->iWidth, pDraw->iHeight
  );
  return 1;
}

void board_draw_jpeg(const uint8_t* jpg, size_t len) {
  if (!jpg || !len) return;
  
  if (jpeg.openRAM((uint8_t*)jpg, (int)len, JPEGDraw)) {
    int w = jpeg.getWidth();
    int h = jpeg.getHeight();
    
    float aspectSrc = (float)w / h;
    float aspectDst = (float)SCREEN_W / SCREEN_H;
    
    uint16_t targetWidth, targetHeight;
    if (aspectSrc > aspectDst) {
      targetWidth = SCREEN_W;
      targetHeight = (uint16_t)((float)SCREEN_W / aspectSrc);
    } else {
      targetHeight = SCREEN_H;
      targetWidth = (uint16_t)((float)SCREEN_H * aspectSrc);
    }
    
    int bestScale = 1;
    int scales[] = {1, 2, 4, 8};
    int bestDiff = 99999;
    
    for (int i = 0; i < 4; i++) {
      int s = scales[i];
      int scaledW = w / s;
      int scaledH = h / s;
      int diff = abs(scaledW - targetWidth) + abs(scaledH - targetHeight);
      if (diff < bestDiff && scaledW <= SCREEN_W + 50 && scaledH <= SCREEN_H + 50) {
         bestScale = s;
         bestDiff = diff;
      }
    }
    
    int decodeOption = 0;
    if (bestScale == 2) decodeOption = JPEG_SCALE_HALF;
    else if (bestScale == 4) decodeOption = JPEG_SCALE_QUARTER;
    else if (bestScale == 8) decodeOption = JPEG_SCALE_EIGHTH;
    
    int scaledW = w / bestScale;
    int scaledH = h / bestScale;
    
    int x = (SCREEN_W - scaledW) / 2;
    int y = (SCREEN_H - scaledH) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    
    gfx->fillScreen(BLACK);
    jpeg.decode(x, y, decodeOption);
    jpeg.close();
  }
}