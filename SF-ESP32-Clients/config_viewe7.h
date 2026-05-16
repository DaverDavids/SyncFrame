#pragma once

// ---------------------------------------------------------------------------
// config_viewe7.h — VIEWESMART UEDX80480070ESP32-7inch-Touch-Display
//
// All pin assignments and timing taken verbatim from VIEWESMART's own
// esp_panel_board_custom_conf.h (examples/arduino/board/board_static_config/)
//
// RGB DATA bus mapping (DATA0..15, i.e. B0..B4 G0..G5 R0..R4):
//   DATA0  = 10  (B0)    DATA8  = 45  (G3)
//   DATA1  = 11  (B1)    DATA9  = 38  (G4)  ← also BACKLIGHT GPIO
//   DATA2  = 12  (B2)    DATA10 = 39  (G5)
//   DATA3  = 13  (B3)    DATA11 = 40  (R0)
//   DATA4  = 14  (B4)    DATA12 = 41  (R1)
//   DATA5  = 21  (G0)    DATA13 = 42  (R2)
//   DATA6  = 47  (G1)    DATA14 =  2  (R3)
//   DATA7  = 48  (G2)    DATA15 =  1  (R4)
//
// Control:
//   HSYNC=46  VSYNC=3  DE=17  PCLK=9
//   BL=38 (active high PWM/GPIO — same pin as G4 data line;
//          set HIGH before gfx->begin(), panel DMA takes it over after)
//
// Touch GT911: SDA=8, SCL=18
//   RST=-1 (not connected per VIEWESMART conf)  INT=-1 (3=VSYNC conflict)
//
// Timing (from esp_panel_board_custom_conf.h RGB section):
//   HPW=10  HBP=10  HFP=20  VPW=10  VBP=10  VFP=10
//   PCLK=16MHz  PCLK_ACTIVE_NEG=0
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

#define GFX_BL 38  // active high

// Arduino_ESP32RGBPanel arg order:
// (de, vsync, hsync, pclk,
//  r0,r1,r2,r3,r4,  g0,g1,g2,g3,g4,g5,  b0,b1,b2,b3,b4,
//  hsync_pol, hsync_fp, hsync_pw, hsync_bp,
//  vsync_pol, vsync_fp, vsync_pw, vsync_bp,
//  pclk_active_neg, prefer_speed, auto_flush, ?, ?, bounce_buf_pixels)
//
// Arduino_GFX DATA pin order: B0..B4, G0..G5, R0..R4
// which maps to DATA0..DATA15: 10,11,12,13,14, 21,47,48,45,38,39, 40,41,42,2,1
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  // DE,  VSYNC, HSYNC, PCLK
  17,    3,     46,    9,
  // R0, R1, R2, R3, R4  (DATA11..DATA15)
  40, 41, 42,  2,  1,
  // G0, G1, G2, G3, G4, G5  (DATA5..DATA10)
  21, 47, 48, 45, 38, 39,
  // B0, B1, B2, B3, B4  (DATA0..DATA4)
  10, 11, 12, 13, 14,
  // hsync_pol, hsync_fp, hsync_pw, hsync_bp
  0, 20, 10, 10,
  // vsync_pol, vsync_fp, vsync_pw, vsync_bp
  0, 10, 10, 10,
  // pclk_active_neg, prefer_speed
  0, 16000000,
  // auto_flush, unused, unused, bounce_buf_pixels
  true, 0, 0, 800 * 20
);

Arduino_GFX *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel, 0, true);

// GT911: SDA=8 SCL=18 (shared with RGB G1/G2 — init BEFORE gfx->begin())
// RST=-1 and INT=-1 per VIEWESMART canonical config
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
  // Touch FIRST — GPIO 8/18 free before panel DMA takes them as data lines
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  ts.begin();
  ts.setRotation(0);

  // Backlight on before panel init
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  // Panel init — RGB DMA now owns GPIO 8/18 as G1/G2 data lines
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
