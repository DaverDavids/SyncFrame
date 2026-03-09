#pragma once

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <TAMC_GT911.h>

static const int SCREEN_W = 800;
static const int SCREEN_H = 480;

#define GFX_BL 2

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40, 41, 39, 42,
  45, 48, 47, 21, 14,
  5,  6,  7,  15, 16, 4,
  8,  3,  46, 9,  1,
  0, 8, 4, 24,
  0, 8, 4, 16,
  1, 12000000,           // Lowered from 15.4MHz to 12MHz to prevent PSRAM DMA starvation during WiFi spikes
  false, 0, 0, 800*20    // Increased bounce buffer from 10 lines to 20 lines to survive WiFi latency
);

Arduino_GFX *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel, 0, true);

#define TOUCH_SDA 19
#define TOUCH_SCL 20
#define TOUCH_INT 18
#define TOUCH_RST 38
TAMC_GT911 ts(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, SCREEN_W, SCREEN_H);

extern void showCurrentPhoto();
extern void showLastPhoto();
extern bool showingLast;
extern bool hasLastPhoto();

void board_init() {
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  gfx->begin();
  gfx->fillScreen(0x0000); 
  gfx->flush();
  
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  ts.begin();
  ts.setRotation(0);
}

void board_loop() {
  ts.read();
  bool pressed = ts.isTouched;
  
  if (pressed && !showingLast && hasLastPhoto()) {
    showLastPhoto();
  } else if (!pressed && showingLast) {
    showCurrentPhoto();
  }
}