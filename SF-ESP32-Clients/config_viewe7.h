#pragma once

// ---------------------------------------------------------------------------
// config_viewe7.h — VIEWESMART UEDX80480070ESP32-7inch-Touch-Display
//
// Constructor argument order MUST match config_s3.h exactly:
//   Arduino_ESP32RGBPanel(
//     r0,r1,r2,r3,          <- 4 R pins
//     g0,g1,g2,g3,g4,       <- 5 G pins (note: this lib uses 5-bit G in this ctor)
//     b0,b1,b2,b3,b4,       <- 5 B pins
//     hsync, vsync, de, pclk, unused,
//     hsync_pol, hsync_fp, hsync_pw, hsync_bp,
//     vsync_pol, vsync_fp, vsync_pw, vsync_bp,
//     pclk_active_neg, prefer_speed,
//     auto_flush, ?, ?, bounce_buf_pixels
//   )
//
// Pin source: VIEWESMART esp_panel_board_custom_conf.h
//   DATA0-4  = B0-B4 = 10,11,12,13,14
//   DATA5-10 = G0-G5 = 21,47,48,45,38,39
//   DATA11-15= R0-R4 = 40,41,42,2,1
//   HSYNC=46  VSYNC=3  DE=17  PCLK=9
//   BL=38 (active high)
// ---------------------------------------------------------------------------

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

#define GFX_BL 38

// Argument order mirrors config_s3.h exactly.
// S3 reference:  (40,41,39,42,  45,48,47,21,14,  5,6,7,15,16,4,  8,3,46,9,1,  ...)
// VIEWE7 pins:   R0-R3=40,41,42,2  G0-G4=21,47,48,45,38  B0-B4=10,11,12,13,14
//                HSYNC=46 VSYNC=3 DE=17 PCLK=9
// Note: ctor takes 4 R + 5 G + 5 B = 14 data pins then 5 control pins
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  // R0, R1, R2, R3
  40, 41, 42, 2,
  // G0, G1, G2, G3, G4
  21, 47, 48, 45, 38,
  // B0, B1, B2, B3, B4
  10, 11, 12, 13, 14,
  // HSYNC, VSYNC, DE, PCLK, (R4/unused=1)
  46, 3, 17, 9, 1,
  // hsync_pol, hsync_fp, hsync_pw, hsync_bp
  0, 10, 10, 10,
  // vsync_pol, vsync_fp, vsync_pw, vsync_bp
  0, 10, 10, 10,
  // pclk_active_neg, prefer_speed
  1, 16000000,
  // auto_flush, ?, ?, bounce_buf_pixels
  true, 0, 0, 800 * 20
);

Arduino_GFX *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel, 0, true);

// GT911: SDA=8 SCL=18 (shared with RGB G1/G2 — init BEFORE gfx->begin())
#define TOUCH_SDA   8
#define TOUCH_SCL  18
#define TOUCH_RST  (-1)
#define TOUCH_INT  (-1)
TAMC_GT911 ts(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, SCREEN_W, SCREEN_H);

extern void showCurrentPhoto();
extern void showLastPhoto();
extern bool showingLast;
extern bool hasLastPhoto();
extern volatile bool boardDrawActive;

void board_init() {
  // Touch FIRST — GPIO 8/18 free before panel DMA takes them as G1/G2
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  ts.begin();
  ts.setRotation(0);

  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  gfx->begin();
  gfx->fillScreen(0x0000);
  gfx->flush();
}

void board_loop(int peekPin) {
  (void)peekPin;
  if (boardDrawActive) return;

  ts.read();
  bool pressed = ts.isTouched;

  static bool lastPressed = false;
  if (pressed == lastPressed) return;
  lastPressed = pressed;

  if (pressed && !showingLast && hasLastPhoto()) {
    showLastPhoto();
  } else if (!pressed && showingLast) {
    showCurrentPhoto();
  }
}
