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
  if (boardDrawActive) return;
  if (networkBusy) return;

  ts.read();
  bool pressed = ts.isTouched;

  // Edge detection: only act on transitions (press-down, lift-up).
  //
  // CRITICAL: lastPressed must be updated to 'pressed' BEFORE calling
  // showLastPhoto() / showCurrentPhoto(), because those call board_draw_jpeg()
  // which sets boardDrawActive=true and blocks this function for ~300ms.
  // If lastPressed is updated AFTER the draw (the naive pattern), it is still
  // 'false' when board_loop() unblocks with the finger still held, so
  // pressStart fires again → infinite redraw loop while touching.
  static bool lastPressed = false;

  if (pressed && !lastPressed && hasLastPhoto() && !showingLast) {
    lastPressed = true;       // update BEFORE the draw blocks us
    showLastPhoto();
  } else if (!pressed && lastPressed && showingLast) {
    lastPressed = false;      // update BEFORE the draw blocks us
    showCurrentPhoto();
  } else {
    // No transition: just keep lastPressed in sync with reality.
    // This handles the case where boardDrawActive caused us to miss a
    // transition (e.g. a very quick tap) - we re-sync here so we don't
    // get permanently stuck.
    lastPressed = pressed;
  }
}
