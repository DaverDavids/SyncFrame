#pragma once

#include <Arduino_GFX_Library.h>
#include <Wire.h>

static const int SCREEN_W = 280;
static const int SCREEN_H = 240;

#define TFT_SCLK 4
#define TFT_MOSI 6
#define TFT_DC   8
#define TFT_RST  9
#define TFT_CS   21

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_ST7789(bus, TFT_RST, 1 /* rotation */, true /* IPS */, 240, 280);

extern void showCurrentPhoto();
extern void showLastPhoto();
extern bool showingLast;
extern bool hasLastPhoto();

void board_init() {
  gfx->begin();
  gfx->fillScreen(0x0000); // Replaced BLACK with 0x0000 (black in RGB565)
}

void board_loop() {
  // Empty loop now that RFID is removed
}