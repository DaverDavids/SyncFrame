#pragma once

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <TAMC_GT911.h>
#include "splash.h"

static const int SCREEN_W = 800;
static const int SCREEN_H = 480;

#define JPEG_SWAP_BYTES true
#define splash_logo logo_480
const size_t splash_logo_len = sizeof(logo_480);
#define MAX_JPG (800UL * 480UL * 2UL)
#define DEFAULT_PHOTO_FILE "photo.800x480.jpg"
#define APP_CORE 1

#define GFX_BL 2

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40, 41, 39, 42,
  45, 48, 47, 21, 14,
  5,  6,  7,  15, 16, 4,
  8,  3,  46, 9,  1,
  0, 8, 4, 24,
  0, 8, 4, 24,
  1, 16000000,
  true, 0, 0, 800*20  // 800*20 is correct; larger values cause boot failures
);

// useDataBuf = true enables Arduino_GFX double-buffering:
// jpegDrawCallback writes into the off-screen back buffer while the DMA
// engine continuously scans the front buffer.  board_draw_jpeg() then calls
// gfx->flush() to atomically swap front<->back once the full frame is ready.
// This is the ONLY correct way to draw on this RGB panel without tearing.
Arduino_GFX *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel, 0, true /* useDataBuf = double-buffer ENABLED */);

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

void board_init() {
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  // PSRAM debug: log free PSRAM before and after gfx->begin() to confirm
  // double-buffer actually allocates two full framebuffers.
  // Expected delta: ~1536KB (2 x 800*480*2 bytes).
  // If delta is only ~768KB, double-buffer silently fell back to single.
  uint32_t psramBefore = ESP.getFreePsram();
  Serial.printf("[BOARD] PSRAM before gfx->begin(): %u bytes\n", (unsigned)psramBefore);

  gfx->begin();

  uint32_t psramAfter = ESP.getFreePsram();
  Serial.printf("[BOARD] PSRAM after  gfx->begin(): %u bytes (delta: %u)\n",
                (unsigned)psramAfter, (unsigned)(psramBefore - psramAfter));
  Serial.printf("[BOARD] Double-buffer check: expected delta ~%u, got %u — %s\n",
                (unsigned)(SCREEN_W * SCREEN_H * 2 * 2),
                (unsigned)(psramBefore - psramAfter),
                (psramBefore - psramAfter >= (uint32_t)(SCREEN_W * SCREEN_H * 2 * 2) * 85 / 100)
                  ? "DOUBLE-BUFFER ACTIVE" : "WARNING: may be single-buffer");

  gfx->fillScreen(0x0000);

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  ts.begin();
  ts.setRotation(0);
}

void board_loop(int peekPin) {
  (void)peekPin;  // S3 uses touchscreen, not a GPIO button
  if (boardDrawActive) return;

  ts.read();
  bool pressed = ts.isTouched;

  // Only act on state CHANGES, not every loop tick (1ms)
  static bool lastPressed = false;
  if (pressed == lastPressed) return;
  lastPressed = pressed;

  if (pressed && !showingLast && hasLastPhoto()) {
    showLastPhoto();
  } else if (!pressed && showingLast) {
    showCurrentPhoto();
  }
}
