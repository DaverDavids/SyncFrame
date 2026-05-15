#pragma once

// ---------------------------------------------------------------------------
// config_viewe7.h — VIEWESMART UEDX80480070ESP32-7inch-Touch-Display
//
// ESP32-S3, 800x480 16-bit RGB565 parallel panel, GT911 capacitive touch.
//
// Pin assignments sourced from VIEWESMART's own driver examples:
//   lcd_single_rgb.ino  → RGB panel GPIOs and timing
//   touch_i2c.ino       → GT911 SDA/SCL/RST/INT
//
// KEY HARDWARE QUIRK — shared GPIOs:
//   GPIO 8  = RGB G1 data line  AND  GT911 SDA
//   GPIO 18 = RGB G2 data line  AND  GT911 SCL
//   GPIO 3  = RGB VSYNC         AND  GT911 INT (cannot use INT after panel init)
//   GPIO 48 = GT911 RST (used only during address-config sequence at boot)
//
// The GT911 uses RST+INT toggling at startup to latch its I2C address.
// Touch MUST be initialized before gfx->begin() so those GPIOs are free
// for the address-config pulse sequence. After that, the RGB panel DMA
// takes over GPIO 8/18 as data lines and the GT911 responds via I2C at
// its latched address. Touch is polled (INT=3 conflicts with VSYNC).
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

// Backlight — GPIO 38, active high
// NOTE: GPIO 38 is also RGB G4 data line. The panel drives it as a data
// pin during normal operation; setting it HIGH before gfx->begin() is
// enough to turn the backlight on (the panel controller takes it over
// after init and the backlight remains on via the data activity).
#define GFX_BL 38

// ---------------------------------------------------------------------------
// RGB panel — 16-bit RGB565, 800x480 @ 16 MHz
//
// Arduino_ESP32RGBPanel constructor arg order (matches config_s3.h pattern):
//   (de, vsync, hsync, pclk,
//    r0,r1,r2,r3,r4,
//    g0,g1,g2,g3,g4,g5,
//    b0,b1,b2,b3,b4,
//    hsync_pol, hsync_fp, hsync_pw, hsync_bp,
//    vsync_pol, vsync_fp, vsync_pw, vsync_bp,
//    pclk_active_neg, prefer_speed,
//    auto_flush, ?, ?, bounce_buf_pixels)
//
// Timing from lcd_single_rgb.ino: HPW=40 HBP=40 HFP=48 VPW=23 VBP=32 VFP=13
//
// DATA pin mapping (DATA0..15 = B0..B4, G0..G5, R0..R4):
//   B0=10 B1=11 B2=12 B3=13 B4=14
//   G0=21 G1=8  G2=18 G3=45 G4=38 G5=39
//   R0=40 R1=41 R2=42 R3=2  R4=1
//   DE=17 VSYNC=3 HSYNC=46 PCLK=9
// ---------------------------------------------------------------------------
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  // DE,  VSYNC, HSYNC, PCLK
  17,    3,     46,    9,
  // R0, R1, R2, R3, R4
  40, 41, 42,  2,  1,
  // G0, G1, G2, G3, G4, G5
  21,  8, 18, 45, 38, 39,
  // B0, B1, B2, B3, B4
  10, 11, 12, 13, 14,
  // hsync_pol, hsync_fp, hsync_pw, hsync_bp
  0, 48, 40, 40,
  // vsync_pol, vsync_fp, vsync_pw, vsync_bp
  0, 13, 23, 32,
  // pclk_active_neg, prefer_speed
  0, 16000000,
  // auto_flush, unused, unused, bounce_buf_pixels (match s3: 800*20)
  true, 0, 0, 800 * 20
);

// Double-buffer: DMA scans front buffer; JPEG writes to back; flush() swaps atomically.
Arduino_GFX *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel, 0, true);

// ---------------------------------------------------------------------------
// GT911 touch
//   SDA=8, SCL=18 (shared with RGB G1/G2 — init BEFORE gfx->begin())
//   RST=48 — used to latch I2C address during GT911 startup sequence
//   INT=3  — conflicts with VSYNC; pass -1 so TAMC_GT911 polls via I2C
// ---------------------------------------------------------------------------
#define TOUCH_SDA   8
#define TOUCH_SCL  18
#define TOUCH_RST  48
#define TOUCH_INT  (-1)
TAMC_GT911 ts(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, SCREEN_W, SCREEN_H);

extern void showCurrentPhoto();
extern void showLastPhoto();
extern bool showingLast;
extern bool hasLastPhoto();
extern volatile bool boardDrawActive;

void board_init() {
  // --- Touch FIRST (GPIO 8/18 free before panel DMA takes them) ---
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  ts.begin();
  ts.setRotation(0);

  // --- Backlight on before panel init so screen lights up immediately ---
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  // --- Panel init — RGB DMA now owns GPIO 8/18 as data lines ---
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
