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
    
    gfx->fillScreen(0x0000); 
    jpeg.decode(x, y, decodeOption);
    jpeg.close();

    // Flush is handled by the caller or double-buffered panels
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)
    gfx->flush();
#endif
  }
}

// Draw a small text overlay over the existing image without clearing the screen
void board_draw_boot_status(const char* text) {
  // Set up text properties
  gfx->setTextSize(2);
  
  // Calculate text width/height to draw a small background box
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  
  // Position in bottom left corner
  int padding = 4;
  int x = 10;
  int y = SCREEN_H - h - 10;
  
  // Draw solid black rectangle behind text so it's readable over the logo
  gfx->fillRect(x - padding, y - padding, w + (padding * 2), h + (padding * 2), 0x0000);
  
  // Draw white text
  gfx->setTextColor(0xFFFF); // White in RGB565
  gfx->setCursor(x, y);
  gfx->print(text);

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)
  gfx->flush();
#endif
}