#pragma once

// ---------------------------------------------------------------------------
// config_viewe7.h — VIEWESMART UEDX80480070ESP32-7inch-Touch-Display
//
// ESP32-S3, 800x480 16-bit RGB565 parallel panel, GT911 capacitive touch.
//
// Pin assignments sourced directly from VIEWESMART's own board example:
// https://github.com/VIEWESMART/UEDX80480070ESP32-7inch-Touch-Display
//   esp_panel_board_custom_conf.h (RGB section, 16-bit data width)
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

// Backlight — GPIO 38 (active high, per board config)
#define GFX_BL 38

// ---------------------------------------------------------------------------
// RGB panel — 16-bit RGB565, 800x480 @ 16 MHz
// Pin mapping (DATA0..15 = B0..B4, G0..G5, R0..R4 in RGB565 order):
//   DATA0  (B0) = 10    DATA8  (G3) = 45
//   DATA1  (B1) = 11    DATA9  (G4) = 38   <-- also used as BL; board shares
//   DATA2  (B2) = 12    DATA10 (G5) = 39
//   DATA3  (B3) = 13    DATA11 (R0) = 40
//   DATA4  (B4) = 14    DATA12 (R1) = 41
//   DATA5  (G0) = 21    DATA13 (R2) = 42
//   DATA6  (G1) = 8    DATA14 (R3) = 2
//   DATA7  (G2) = 18    DATA15 (R4) = 1
// ---------------------------------------------------------------------------
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  // DE,  VSYNC, HSYNC, PCLK
  17,    3,     46,    9,
  // R4, R3, R2, R1, R0
  1,  2,  42, 41, 40,
  // G5, G4, G3, G2, G1, G0
  39, 38, 45, 18, 8, 21,
  // B4, B3, B2, B1, B0
  14, 13, 12, 11, 10,
  // hsync_polarity, hsync_front_porch, hsync_pulse_width, hsync_back_porch
  0, 48, 40, 40,
  // vsync_polarity, vsync_front_porch, vsync_pulse_width, vsync_back_porch
  0, 13, 23, 32,
  // pclk_active_neg, prefer_speed
  0, 16000000,
  // auto_flush, bounce_buffer_size (800*10 avoids drift on S3)
  true, 0, 0, 800 * 10
);

// Double-buffer: DMA always scans the front buffer; JPEG decode writes into
// the back buffer and gfx->flush() swaps atomically — no tearing.
Arduino_GFX *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel, 0, true);

// ---------------------------------------------------------------------------
// GT911 touch — I2C on SCL=18, SDA=8; INT and RST unused (-1)
// ---------------------------------------------------------------------------
#define TOUCH_SDA  8
#define TOUCH_SCL  18
#define TOUCH_INT  (-1)
#define TOUCH_RST  (-1)
TAMC_GT911 ts(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, SCREEN_W, SCREEN_H);

extern void showCurrentPhoto();
extern void showLastPhoto();
extern bool showingLast;
extern bool hasLastPhoto();
extern volatile bool boardDrawActive;

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
