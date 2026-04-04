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
  1, 16000000,
  true, 0, 0, 800*20  // bounce buffer: 800*20 is correct; larger values cause boot failures
);

// Single-buffer mode. Double-buffer was removed: flush() mid-JPEG-render
// caused partial frames, and reentrant GFX from Core 0 caused watchdog reboots.
Arduino_GFX *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel, 0, false);

#define TOUCH_SDA 19
#define TOUCH_SCL 20
#define TOUCH_INT 18
#define TOUCH_RST 38
TAMC_GT911 ts(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, SCREEN_W, SCREEN_H);

extern void showCurrentPhoto();
extern void showLastPhoto();
extern bool showingLast;
extern bool hasLastPhoto();
extern volatile bool boardDrawActive;
extern volatile bool networkBusy;

void board_init() {
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  gfx->begin();
  gfx->fillScreen(0x0000);  // safe here: DMA not scanning content yet

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  ts.begin();
  ts.setRotation(0);
}

void board_loop() {
  // Do not touch the display while a JPEG decode/draw is in progress.
  // boardDrawActive is set true at the top of board_draw_jpeg() and cleared
  // at the bottom - showCurrentPhoto/showLastPhoto both acquire drawMutex
  // internally, so no mutex access is needed here.
  if (boardDrawActive) return;
  if (networkBusy) return;

  ts.read();
  ts.read();
  bool pressed = ts.isTouched;

  if (pressed && !showingLast && hasLastPhoto()) {
    showLastPhoto();
  } else if (!pressed && showingLast) {
    showCurrentPhoto();
  }
}
