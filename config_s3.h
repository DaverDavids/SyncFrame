#pragma once

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <TAMC_GT911.h>
#include <JPEGDEC.h>

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
  1, 15400000,
  false, 0, 0, 800*10
);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel);

#define TOUCH_SDA 19
#define TOUCH_SCL 20
#define TOUCH_INT 18
#define TOUCH_RST 38
TAMC_GT911 ts(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, SCREEN_W, SCREEN_H);

static JPEGDEC jpeg;

// Expose these from the main app
extern void showCurrentPhoto();
extern void showLastPhoto();
extern bool showingLast;
extern bool hasLastPhoto();

static int JPEGDraw(JPEGDRAW *pDraw) {
  gfx->draw16bitRGBBitmap(
    pDraw->x, pDraw->y,
    (uint16_t*)pDraw->pPixels,
    pDraw->iWidth, pDraw->iHeight
  );
  return 1;
}

void board_init() {
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  gfx->begin();
  gfx->fillScreen(gfx->color565(0, 0, 0));
  
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  ts.begin();
  ts.setRotation(0);
}

void board_draw_jpeg(const uint8_t* jpg, size_t len) {
  if (!jpg || !len) return;
  gfx->fillScreen(gfx->color565(0, 0, 0));
  
  if (jpeg.openRAM((uint8_t*)jpg, (int)len, JPEGDraw)) {
    int w = jpeg.getWidth();
    int h = jpeg.getHeight();
    int x = (SCREEN_W - w) / 2;
    int y = (SCREEN_H - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    jpeg.decode(x, y, 0);
    jpeg.close();
  }
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