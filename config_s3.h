#pragma once

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <TAMC_GT911.h>

static const int SCREEN_W = 800;
static const int SCREEN_H = 480;

#define GFX_BL 2

static Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40, 41, 39, 42,         // DE, VSYNC, HSYNC, PCLK
  45, 48, 47, 21, 14,     // R0-R4
  5,  6,  7,  15, 16, 4,  // G0-G5
  8,  3,  46, 9,  1,      // B0-B4
  0, 8, 4, 24,            // HSYNC timing
  0, 8, 4, 16,            // VSYNC timing
  1, 15400000,
  false, 0, 0, 800 * 10
);

static Arduino_RGB_Display *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel);

#define TOUCH_SDA 19
#define TOUCH_SCL 20
#define TOUCH_INT 18
#define TOUCH_RST 38
static TAMC_GT911 ts(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, SCREEN_W, SCREEN_H);

extern void showCurrentPhoto();
extern void showLastPhoto();
extern bool showingLast;
extern bool hasLastPhoto();

void board_init() {
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  gfx->begin();
  gfx->fillScreen(gfx->color565(0, 0, 0));

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

int board_screen_w() { return gfx->width(); }
int board_screen_h() { return gfx->height(); }

void board_fill_black() {
  gfx->fillScreen(gfx->color565(0, 0, 0));
}

void board_draw_rgb565_block(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!pixels || w <= 0 || h <= 0) return;
  gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}
