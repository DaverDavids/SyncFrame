#pragma once

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include "splash.h"

static const int SCREEN_W = 280;
static const int SCREEN_H = 240;

#define TFT_SCLK 0
#define TFT_MOSI 1
#define TFT_DC   3
#define TFT_RST  2
#define TFT_CS   4

#define JPEG_SWAP_BYTES false
#define splash_logo logo_240 
const size_t splash_logo_len = sizeof(logo_240);
#define MAX_JPG (280UL * 240UL * 2UL)
#define DEFAULT_PHOTO_FILE "photo.280x240.jpg"
#define APP_CORE 0

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_ST7789(bus, TFT_RST, 1 /* rotation */, true /* IPS */, 240, 280, 0, 20, 0, 20);

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