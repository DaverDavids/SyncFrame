#pragma once

// ---------------------------------------------------------------------------
// config_viewe7.h  —  VIEWESMART UEDX80480070ESP32-7inch-Touch-Display
//
<<<<<<< HEAD
// Root cause of blank display / vertical-lines artifacts:
//   The ST7701 LCD controller on this board requires a 3-wire SPI
//   initialization sequence BEFORE the RGB DMA panel is started.
//   Arduino_ESP32RGBPanel alone cannot do this; we must bit-bang the
//   SPI init registers first, then let GFX start the RGB DMA engine.
//
// Pin source: VIEWESMART reference example lcd_3wire_spi_rgb.ino
//   (examples/arduino/drivers/lcd/lcd_3wire_spi_rgb/lcd_3wire_spi_rgb.ino)
//
// Arduino_GFX constructor arg order:
//   Arduino_ESP32RGBPanel(de, vsync, hsync, pclk,
//                         r0,r1,r2,r3,r4,
//                         g0,g1,g2,g3,g4,g5,
//                         b0,b1,b2,b3,b4,
//                         hsync_pol, hsync_fp, hsync_pw, hsync_bp,
//                         vsync_pol, vsync_fp, vsync_pw, vsync_bp,
//                         pclk_active_neg, prefer_speed,
//                         useBigEndian, de_idle_high, pclk_idle_high,
//                         bounce_buffer_size_px)
//
// GPIO mapping (VIEWESMART reference, 16-bit RGB565):
//   DATA0 =  4  -> B0     DATA1 =  5  -> B1     DATA2 =  6  -> B2
//   DATA3 =  7  -> B3     DATA4 = 15  -> B4
//   DATA5 =  8  -> G0     DATA6 = 20  -> G1     DATA7 =  3  -> G2
//   DATA8 = 46  -> G3     DATA9 =  9  -> G4     DATA10= 10  -> G5
//   DATA11= 11  -> R0     DATA12= 12  -> R1     DATA13= 13  -> R2
//   DATA14= 14  -> R3     DATA15=  0  -> R4
//
//   VSYNC=17  HSYNC=16  DE=18  PCLK=21
//   SPI_CS=39  SPI_SCK=48  SPI_SDA=47
//   BL=38 (active high, dedicated GPIO — does NOT share with data bus)
//   Touch GT911: SDA=19, SCL=20, RST=-1, INT=-1
//
// Timing (from VIEWESMART reference):
//   HPW=10 HBP=10 HFP=20   VPW=10 VBP=10 VFP=10   PCLK=16MHz
=======
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
>>>>>>> parent of 1988ef1 (Merge branch 'main' of https://github.com/DaverDavids/SyncFrame)
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

<<<<<<< HEAD
// ---------------------------------------------------------------------------
// 3-wire SPI pins for ST7701 vendor init
// ---------------------------------------------------------------------------
#define VIEWE7_SPI_CS   39
#define VIEWE7_SPI_SCK  48
#define VIEWE7_SPI_SDA  47

#define GFX_BL 38  // active high, dedicated GPIO

// ---------------------------------------------------------------------------
// ST7701 3-wire SPI bit-bang helper
// Sends a 9-bit word: bit8=0 for command, bit8=1 for data.
// CS is active-low; data is clocked on SCK rising edge.
// ---------------------------------------------------------------------------
static void _viewe7_spi_write9(uint16_t word9) {
  for (int i = 8; i >= 0; i--) {
    digitalWrite(VIEWE7_SPI_SCK, LOW);
    digitalWrite(VIEWE7_SPI_SDA, (word9 >> i) & 1 ? HIGH : LOW);
    digitalWrite(VIEWE7_SPI_SCK, HIGH);
  }
}

static void _viewe7_write_cmd(uint8_t cmd) {
  digitalWrite(VIEWE7_SPI_CS, LOW);
  _viewe7_spi_write9((uint16_t)cmd);          // bit8=0 → command
  digitalWrite(VIEWE7_SPI_CS, HIGH);
}

static void _viewe7_write_data(uint8_t dat) {
  digitalWrite(VIEWE7_SPI_CS, LOW);
  _viewe7_spi_write9(0x100 | (uint16_t)dat);  // bit8=1 → data
  digitalWrite(VIEWE7_SPI_CS, HIGH);
}

// ---------------------------------------------------------------------------
// ST7701 vendor initialization sequence
// Source: VIEWESMART lcd_3wire_spi_rgb.ino reference example
// ---------------------------------------------------------------------------
static void _viewe7_st7701_init() {
  pinMode(VIEWE7_SPI_CS,  OUTPUT); digitalWrite(VIEWE7_SPI_CS,  HIGH);
  pinMode(VIEWE7_SPI_SCK, OUTPUT); digitalWrite(VIEWE7_SPI_SCK, HIGH);
  pinMode(VIEWE7_SPI_SDA, OUTPUT); digitalWrite(VIEWE7_SPI_SDA, HIGH);

  // CMD2 BK0 — page 0
  _viewe7_write_cmd(0xFF); _viewe7_write_data(0x77); _viewe7_write_data(0x01);
                            _viewe7_write_data(0x00); _viewe7_write_data(0x00); _viewe7_write_data(0x10);

  _viewe7_write_cmd(0xC0); _viewe7_write_data(0x3B); _viewe7_write_data(0x00);
  _viewe7_write_cmd(0xC1); _viewe7_write_data(0x0D); _viewe7_write_data(0x02);
  _viewe7_write_cmd(0xC2); _viewe7_write_data(0x31); _viewe7_write_data(0x05);
  _viewe7_write_cmd(0xCD); _viewe7_write_data(0x00);

  _viewe7_write_cmd(0xB0);
    _viewe7_write_data(0x00); _viewe7_write_data(0x11); _viewe7_write_data(0x18); _viewe7_write_data(0x0E);
    _viewe7_write_data(0x11); _viewe7_write_data(0x06); _viewe7_write_data(0x07); _viewe7_write_data(0x08);
    _viewe7_write_data(0x07); _viewe7_write_data(0x22); _viewe7_write_data(0x04); _viewe7_write_data(0x12);
    _viewe7_write_data(0x0F); _viewe7_write_data(0xAA); _viewe7_write_data(0x31); _viewe7_write_data(0x18);

  _viewe7_write_cmd(0xB1);
    _viewe7_write_data(0x00); _viewe7_write_data(0x11); _viewe7_write_data(0x19); _viewe7_write_data(0x0E);
    _viewe7_write_data(0x12); _viewe7_write_data(0x07); _viewe7_write_data(0x08); _viewe7_write_data(0x08);
    _viewe7_write_data(0x08); _viewe7_write_data(0x22); _viewe7_write_data(0x04); _viewe7_write_data(0x11);
    _viewe7_write_data(0x11); _viewe7_write_data(0xA9); _viewe7_write_data(0x32); _viewe7_write_data(0x18);

  // CMD2 BK1 — page 1
  _viewe7_write_cmd(0xFF); _viewe7_write_data(0x77); _viewe7_write_data(0x01);
                            _viewe7_write_data(0x00); _viewe7_write_data(0x00); _viewe7_write_data(0x11);

  _viewe7_write_cmd(0xB0); _viewe7_write_data(0x60);
  _viewe7_write_cmd(0xB1); _viewe7_write_data(0x32);
  _viewe7_write_cmd(0xB2); _viewe7_write_data(0x07);
  _viewe7_write_cmd(0xB3); _viewe7_write_data(0x80);
  _viewe7_write_cmd(0xB5); _viewe7_write_data(0x49);
  _viewe7_write_cmd(0xB7); _viewe7_write_data(0x85);
  _viewe7_write_cmd(0xB8); _viewe7_write_data(0x21);
  _viewe7_write_cmd(0xC1); _viewe7_write_data(0x78);
  _viewe7_write_cmd(0xC2); _viewe7_write_data(0x78);

  _viewe7_write_cmd(0xE0); _viewe7_write_data(0x00); _viewe7_write_data(0x1B); _viewe7_write_data(0x02);

  _viewe7_write_cmd(0xE1);
    _viewe7_write_data(0x08); _viewe7_write_data(0xA0); _viewe7_write_data(0x00); _viewe7_write_data(0x00);
    _viewe7_write_data(0x07); _viewe7_write_data(0xA0); _viewe7_write_data(0x00); _viewe7_write_data(0x00);
    _viewe7_write_data(0x00); _viewe7_write_data(0x44); _viewe7_write_data(0x44);

  _viewe7_write_cmd(0xE2);
    _viewe7_write_data(0x11); _viewe7_write_data(0x11); _viewe7_write_data(0x44); _viewe7_write_data(0x44);
    _viewe7_write_data(0xED); _viewe7_write_data(0xA0); _viewe7_write_data(0x00); _viewe7_write_data(0x00);
    _viewe7_write_data(0xEC); _viewe7_write_data(0xA0); _viewe7_write_data(0x00); _viewe7_write_data(0x00);

  _viewe7_write_cmd(0xE3);
    _viewe7_write_data(0x00); _viewe7_write_data(0x00); _viewe7_write_data(0x11); _viewe7_write_data(0x11);

  _viewe7_write_cmd(0xE4); _viewe7_write_data(0x44); _viewe7_write_data(0x44);

  _viewe7_write_cmd(0xE5);
    _viewe7_write_data(0x0A); _viewe7_write_data(0xE9); _viewe7_write_data(0xD8); _viewe7_write_data(0xA0);
    _viewe7_write_data(0x0C); _viewe7_write_data(0xEB); _viewe7_write_data(0xD8); _viewe7_write_data(0xA0);
    _viewe7_write_data(0x0E); _viewe7_write_data(0xED); _viewe7_write_data(0xD8); _viewe7_write_data(0xA0);
    _viewe7_write_data(0x10); _viewe7_write_data(0xEF); _viewe7_write_data(0xD8); _viewe7_write_data(0xA0);

  _viewe7_write_cmd(0xE6);
    _viewe7_write_data(0x00); _viewe7_write_data(0x00); _viewe7_write_data(0x11); _viewe7_write_data(0x11);

  _viewe7_write_cmd(0xE7); _viewe7_write_data(0x44); _viewe7_write_data(0x44);

  _viewe7_write_cmd(0xE8);
    _viewe7_write_data(0x09); _viewe7_write_data(0xE8); _viewe7_write_data(0xD8); _viewe7_write_data(0xA0);
    _viewe7_write_data(0x0B); _viewe7_write_data(0xEA); _viewe7_write_data(0xD8); _viewe7_write_data(0xA0);
    _viewe7_write_data(0x0D); _viewe7_write_data(0xEC); _viewe7_write_data(0xD8); _viewe7_write_data(0xA0);
    _viewe7_write_data(0x0F); _viewe7_write_data(0xEE); _viewe7_write_data(0xD8); _viewe7_write_data(0xA0);

  _viewe7_write_cmd(0xEB);
    _viewe7_write_data(0x02); _viewe7_write_data(0x00); _viewe7_write_data(0xE4); _viewe7_write_data(0xE4);
    _viewe7_write_data(0x88); _viewe7_write_data(0x00); _viewe7_write_data(0x40);

  _viewe7_write_cmd(0xEC); _viewe7_write_data(0x3C); _viewe7_write_data(0x00);

  _viewe7_write_cmd(0xED);
    _viewe7_write_data(0xAB); _viewe7_write_data(0x89); _viewe7_write_data(0x76); _viewe7_write_data(0x54);
    _viewe7_write_data(0x02); _viewe7_write_data(0xFF); _viewe7_write_data(0xFF); _viewe7_write_data(0xFF);
    _viewe7_write_data(0xFF); _viewe7_write_data(0xFF); _viewe7_write_data(0xFF); _viewe7_write_data(0x20);
    _viewe7_write_data(0x45); _viewe7_write_data(0x67); _viewe7_write_data(0x98); _viewe7_write_data(0xBA);

  // CMD2 BK3
  _viewe7_write_cmd(0xFF); _viewe7_write_data(0x77); _viewe7_write_data(0x01);
                            _viewe7_write_data(0x00); _viewe7_write_data(0x00); _viewe7_write_data(0x13);
  _viewe7_write_cmd(0xE5); _viewe7_write_data(0xE4);

  // CMD2 BK0 again — exit vendor page, send sleep-out
  _viewe7_write_cmd(0xFF); _viewe7_write_data(0x77); _viewe7_write_data(0x01);
                            _viewe7_write_data(0x00); _viewe7_write_data(0x00); _viewe7_write_data(0x00);
  _viewe7_write_cmd(0x11);  // Sleep Out
  delay(120);
  _viewe7_write_cmd(0x29);  // Display On
  delay(20);
}

// ---------------------------------------------------------------------------
// RGB panel — corrected GPIO mapping from VIEWESMART reference
// Arduino_ESP32RGBPanel arg order:
//   (de, vsync, hsync, pclk, r0..r4, g0..g5, b0..b4,
//    hsync_pol, hsync_fp, hsync_pw, hsync_bp,
//    vsync_pol, vsync_fp, vsync_pw, vsync_bp,
//    pclk_active_neg, prefer_speed, useBigEndian,
//    de_idle_high, pclk_idle_high, bounce_buffer_size_px)
// ---------------------------------------------------------------------------
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  // de,  vsync, hsync, pclk
  18,    17,    16,    21,
  // r0,  r1,  r2,  r3,  r4
  11,   12,   13,   14,   0,
  // g0,  g1,  g2,  g3,  g4,  g5
   8,   20,    3,   46,   9,  10,
  // b0,  b1,  b2,  b3,  b4
   4,    5,    6,    7,  15,
  // hsync_pol, hsync_front_porch, hsync_pulse_width, hsync_back_porch
  0,           20,                10,                10,
  // vsync_pol, vsync_front_porch, vsync_pulse_width, vsync_back_porch
  0,           10,                10,                10,
  // pclk_active_neg, prefer_speed, useBigEndian, de_idle_high, pclk_idle_high
  0,               16000000,      false,         0,            0,
  // bounce_buffer_size_px
  800 * 10
=======
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
>>>>>>> parent of 1988ef1 (Merge branch 'main' of https://github.com/DaverDavids/SyncFrame)
);

Arduino_GFX *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel, 0, true);

<<<<<<< HEAD
#define TOUCH_SDA  19
#define TOUCH_SCL  20
=======
// GT911: SDA=8 SCL=18 (shared with RGB G1/G2 — init BEFORE gfx->begin())
// RST=-1 and INT=-1 per VIEWESMART canonical config
#define TOUCH_SDA   8
#define TOUCH_SCL  18
>>>>>>> parent of 1988ef1 (Merge branch 'main' of https://github.com/DaverDavids/SyncFrame)
#define TOUCH_RST  (-1)
#define TOUCH_INT  (-1)
TAMC_GT911 ts(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, SCREEN_W, SCREEN_H);

extern void showCurrentPhoto();
extern void showLastPhoto();
extern bool showingLast;
extern bool hasLastPhoto();
extern volatile bool boardDrawActive;

void board_init() {
<<<<<<< HEAD
  // 1. ST7701 SPI init MUST happen before RGB DMA starts
  _viewe7_st7701_init();

  // 2. Backlight on
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  // 3. Touch — GT911 on I2C (SDA=19, SCL=20)
=======
  // Touch FIRST — GPIO 8/18 free before panel DMA takes them as data lines
>>>>>>> parent of 1988ef1 (Merge branch 'main' of https://github.com/DaverDavids/SyncFrame)
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  ts.begin();
  ts.setRotation(0);

<<<<<<< HEAD
  // 4. Start RGB DMA panel
=======
  // Backlight on before panel init
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  // Panel init — RGB DMA now owns GPIO 8/18 as G1/G2 data lines
>>>>>>> parent of 1988ef1 (Merge branch 'main' of https://github.com/DaverDavids/SyncFrame)
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
