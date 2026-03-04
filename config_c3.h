#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <TJpg_Decoder.h>

static const int SCREEN_W = 280;
static const int SCREEN_H = 240;

#define TFT_SCLK 4
#define TFT_MOSI 6
#define TFT_DC   8
#define TFT_RST  9
#define TFT_CS   21

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

#define RFID_DATA_PIN 3
#define RFID_BIT_PERIOD 256
#define RFID_DISPLAY_TIME 3000

volatile uint64_t rfidData = 0;
volatile uint8_t rfidBitCount = 0;
volatile bool rfidDataReady = false;
volatile unsigned long lastEdgeTime = 0;
volatile bool rfidEnabled = false;

// Expose these from the main app
extern void triggerPhotoDownload();
extern void showCurrentPhoto();

void IRAM_ATTR rfidISR() {
  if (!rfidEnabled) return;
  unsigned long currentTime = micros();
  unsigned long pulseWidth = currentTime - lastEdgeTime;
  
  if (lastEdgeTime == 0) {
    lastEdgeTime = currentTime;
    return;
  }
  
  lastEdgeTime = currentTime;
  if (pulseWidth < 100 || pulseWidth > 1000) return;
  
  if (pulseWidth > RFID_BIT_PERIOD / 2 && pulseWidth < RFID_BIT_PERIOD * 1.5) {
    int bitValue = digitalRead(RFID_DATA_PIN);
    if (rfidBitCount < 64) {
      rfidData = (rfidData << 1) | bitValue;
      rfidBitCount++;
      if (rfidBitCount >= 64) {
        rfidDataReady = true;
      }
    }
  } else if (pulseWidth > RFID_BIT_PERIOD * 1.5) {
    rfidData = 0;
    rfidBitCount = 0;
  }
}

uint64_t readRFID() {
  if (rfidDataReady) {
    noInterrupts();
    rfidDataReady = false;
    uint64_t tempData = rfidData;
    rfidData = 0;
    rfidBitCount = 0;
    interrupts();
    return tempData;
  }
  return 0;
}

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return 0;
  tft.startWrite();
  tft.setAddrWindow(x, y, w, h);
  for (uint16_t i = 0; i < w * h; i++) {
    tft.pushColor(bitmap[i]);
  }
  tft.endWrite();
  return 1;
}

void board_init() {
  tft.init(240, 280);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(tft_output);

  pinMode(RFID_DATA_PIN, INPUT);
  delay(100);
  rfidEnabled = true;
  attachInterrupt(digitalPinToInterrupt(RFID_DATA_PIN), rfidISR, CHANGE);
}

void board_draw_jpeg(const uint8_t* jpg, size_t len) {
  if (!jpg || !len) return;
  
  uint16_t jpegWidth, jpegHeight;
  TJpgDec.getJpgSize(&jpegWidth, &jpegHeight, jpg, len);
  
  float aspectSrc = (float)jpegWidth / jpegHeight;
  float aspectDst = (float)SCREEN_W / SCREEN_H;
  
  uint16_t targetWidth, targetHeight;
  if (aspectSrc > aspectDst) {
    targetWidth = SCREEN_W;
    targetHeight = (uint16_t)((float)SCREEN_W / aspectSrc);
  } else {
    targetHeight = SCREEN_H;
    targetWidth = (uint16_t)((float)SCREEN_H * aspectSrc);
  }
  
  uint8_t bestScale = 1;
  int bestDiff = 99999;
  for (uint8_t scale = 1; scale <= 8; scale *= 2) {
    uint16_t scaledW = jpegWidth / scale;
    uint16_t scaledH = jpegHeight / scale;
    int totalDiff = abs((int)scaledW - (int)targetWidth) + abs((int)scaledH - (int)targetHeight);
    if (totalDiff < bestDiff && scaledW <= SCREEN_W + 50 && scaledH <= SCREEN_H + 50) {
      bestScale = scale;
      bestDiff = totalDiff;
    }
  }
  
  uint16_t displayWidth = jpegWidth / bestScale;
  uint16_t displayHeight = jpegHeight / bestScale;
  int16_t xOffset = (SCREEN_W - displayWidth) / 2;
  int16_t yOffset = (SCREEN_H - displayHeight) / 2;
  if (xOffset < 0) xOffset = 0;
  if (yOffset < 0) yOffset = 0;
  
  tft.fillScreen(ST77XX_BLACK);
  TJpgDec.setJpgScale(bestScale);
  TJpgDec.drawJpg(xOffset, yOffset, jpg, len);
}

void board_loop() {
  uint64_t cardID = readRFID();
  if (cardID != 0) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_GREEN);
    tft.setTextSize(3);
    tft.setCursor(50, 80);
    tft.println("RFID:");
    tft.setCursor(20, 130);
    tft.println((unsigned long)cardID);
    
    delay(RFID_DISPLAY_TIME);
    triggerPhotoDownload(); // Re-trigger a photo update/download
  }
}