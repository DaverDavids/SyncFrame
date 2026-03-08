#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

static const int TFT_NATIVE_W = 240;
static const int TFT_NATIVE_H = 280;

#define TFT_SCLK 4
#define TFT_MOSI 6
#define TFT_DC   8
#define TFT_RST  9
#define TFT_CS   21  // If truly not connected, this can still be any GPIO (left high) or tied low in hardware

static Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

void board_init() {
  tft.init(TFT_NATIVE_W, TFT_NATIVE_H);
  tft.setRotation(1); // landscape
  tft.fillScreen(ST77XX_BLACK);
}

void board_loop() {
  // No interaction for C3 at the moment (RFID removed).
}

int board_screen_w() { return tft.width(); }
int board_screen_h() { return tft.height(); }

void board_fill_black() {
  tft.fillScreen(ST77XX_BLACK);
}

void board_draw_rgb565_block(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!pixels || w <= 0 || h <= 0) return;
  // JPEGDEC gives RGB565 blocks; Adafruit_GFX supports drawing 16-bit color bitmaps.
  tft.drawRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}
