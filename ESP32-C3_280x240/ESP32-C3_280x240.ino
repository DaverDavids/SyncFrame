// ============================================
// CONFIGURATION VARIABLES
// ============================================
#define DEBUG 1  // Set to 1 to enable serial debugging, 0 to disable

#define HOSTNAME "rfid"
#define IMAGE_URL "https://192.168.6.202:8369/syncframe/photo.800x480.jpg"  // Your image URL
#define IMAGE_USER "david"                                        // HTTP Basic Auth username
#define IMAGE_PASS test_http_password                           // HTTP Basic Auth password (added quotes)
#define IMAGE_UPDATE_INTERVAL 0                               // Image refresh interval (ms)
#define RFID_DISPLAY_TIME 3000                                    // Time to show RFID on screen (ms)

// ============================================
// DEBUG MACRO
// ============================================
#if DEBUG
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(x, y) Serial.println(x, y)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(x, y)
#endif

// ============================================
// PIN DEFINITIONS (ESP32-C3 SuperMini)
// ============================================
#define TFT_SCLK 4   // SPI Clock
#define TFT_MOSI 6   // SPI MOSI
#define TFT_DC   8   // Data/Command
#define TFT_RST  9   // Reset
#define TFT_CS   21  // CS not connected

#define RFID_DATA_PIN 3  // EM4095 data pin

// ============================================
// EM4095 RFID READING (Manchester Decoding)
// ============================================
#define RFID_BIT_PERIOD 256  // Microseconds per bit (adjust for your tag frequency)

volatile uint64_t rfidData = 0;
volatile uint8_t rfidBitCount = 0;
volatile bool rfidDataReady = false;
volatile unsigned long lastEdgeTime = 0;
volatile bool rfidEnabled = false;

void IRAM_ATTR rfidISR() {
  if (!rfidEnabled) return;  // Don't process if not ready
  
  unsigned long currentTime = micros();
  unsigned long pulseWidth = currentTime - lastEdgeTime;
  
  // Ignore first edge (no previous time reference)
  if (lastEdgeTime == 0) {
    lastEdgeTime = currentTime;
    return;
  }
  
  lastEdgeTime = currentTime;
  
  // Ignore noise and startup glitches
  if (pulseWidth < 100 || pulseWidth > 1000) return;
  
  // Manchester decoding: short pulse = bit, long pulse = no bit
  if (pulseWidth > RFID_BIT_PERIOD / 2 && pulseWidth < RFID_BIT_PERIOD * 1.5) {
    // Short pulse - this is a bit transition
    int bitValue = digitalRead(RFID_DATA_PIN);
    
    if (rfidBitCount < 64) {
      rfidData = (rfidData << 1) | bitValue;
      rfidBitCount++;
      
      // Standard EM4100 tags send 64 bits
      if (rfidBitCount >= 64) {
        rfidDataReady = true;
      }
    }
  } else if (pulseWidth > RFID_BIT_PERIOD * 1.5) {
    // Long pulse - reset and start over
    rfidData = 0;
    rfidBitCount = 0;
  }
}

void setupRFID() {
  pinMode(RFID_DATA_PIN, INPUT);
  delay(100);  // Let pin stabilize
  rfidEnabled = true;  // Enable ISR processing
  attachInterrupt(digitalPinToInterrupt(RFID_DATA_PIN), rfidISR, CHANGE);
  DEBUG_PRINTLN("RFID reader initialized on GPIO3");
}

uint64_t readRFID() {
  if (rfidDataReady) {
    noInterrupts();  // Disable interrupts while reading
    rfidDataReady = false;
    uint64_t tempData = rfidData;
    rfidData = 0;
    rfidBitCount = 0;
    interrupts();  // Re-enable interrupts
    return tempData;
  }
  return 0;
}

// ============================================
// LIBRARIES
// ============================================
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <TJpg_Decoder.h>
#include <Secrets.h>
#include "html.h"

// ============================================
// GLOBAL OBJECTS
// ============================================
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
WiFiManager wifiManager;
HTTPClient http;

// ============================================
// GLOBAL VARIABLES (SIMPLIFIED)
// ============================================
unsigned long lastImageUpdate = 0;
int imageWidth = 280;   // Landscape width
int imageHeight = 240;  // Landscape height
bool imageLoaded = false;

// Store calculated offset for redraw
int16_t lastXOffset = 0;
int16_t lastYOffset = 0;
uint8_t lastScale = 1;

// ============================================
// FUNCTION PROTOTYPES
// ============================================
void setupWiFi();
void setupOTA();
void setupMDNS();
void downloadAndDisplayImage();
void displayRFID(uint64_t cardID);
void checkRFID();

// ============================================
// SIMPLE BILINEAR IMAGE SCALING
// ============================================
void scaleAndDrawImage(uint16_t* srcBuffer, uint16_t srcW, uint16_t srcH, 
                       int16_t dstX, int16_t dstY, uint16_t dstW, uint16_t dstH) {
  float xRatio = (float)srcW / dstW;
  float yRatio = (float)srcH / dstH;
  
  tft.startWrite();
  for (uint16_t y = 0; y < dstH; y++) {
    for (uint16_t x = 0; x < dstW; x++) {
      uint16_t srcX = (uint16_t)(x * xRatio);
      uint16_t srcY = (uint16_t)(y * yRatio);
      uint16_t color = srcBuffer[srcY * srcW + srcX];
      tft.writePixel(dstX + x, dstY + y, color);
    }
  }
  tft.endWrite();
}

// Buffer for decoded image
uint16_t* decodedImageBuffer = nullptr;
uint16_t decodedWidth = 0;
uint16_t decodedHeight = 0;
uint16_t targetDisplayWidth = 0;
uint16_t targetDisplayHeight = 0;

// Callback to decode JPEG into buffer
bool jpegDecodeToBuffer(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  // Copy decoded block into our buffer
  for (uint16_t row = 0; row < h; row++) {
    if (y + row < decodedHeight) {
      memcpy(&decodedImageBuffer[(y + row) * decodedWidth + x], &bitmap[row * w], w * 2);
    }
  }
  return 1;
}

// ============================================
// JPEG DECODER SETUP
// ============================================
#include <TJpg_Decoder.h>

// Callback to draw directly to display
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

void setupJpegDecoder() {
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);  // You said this fixed colors
  TJpgDec.setCallback(tft_output);
}

// ============================================
// SETUP
// ============================================
void setup() {
  #if DEBUG
  Serial.begin(115200);
  delay(1000);
  DEBUG_PRINTLN("\n=== RFID Display Starting ===");
  #endif
  
  // Initialize ST7789
  DEBUG_PRINTLN("Initializing display...");
  tft.init(240, 280);  // Width, height
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 120);
  setupJpegDecoder();

  delay(500);
  
  // Setup WiFi with captive portal fallback
  DEBUG_PRINTLN("Setting up WiFi...");
  setupWiFi();
  
  // Setup mDNS
  setupMDNS();
  
  // Setup OTA
  setupOTA();
  
  // Initialize RFID LAST (after everything else is stable)
  DEBUG_PRINTLN("Initializing RFID...");
  setupRFID();
  
  // Initial image download
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(10, 120);
  tft.println("Loading...");
  delay(1000);
  downloadAndDisplayImage();
  
  DEBUG_PRINTLN("Setup complete!");
}

// ============================================
// MAIN LOOP
// ============================================
void loop() {
  ArduinoOTA.handle();
  
  // Check for RFID tags
  checkRFID();
  
  if (IMAGE_UPDATE_INTERVAL > 0 && (millis() - lastImageUpdate) > IMAGE_UPDATE_INTERVAL) {
    downloadAndDisplayImage();
  }

  delay(100);
}

// ============================================
// WiFi SETUP WITH CAPTIVE PORTAL
// ============================================
void setupWiFi() {
  DEBUG_PRINTLN("Setting up WiFi...");
  
  // FIRST: Try hardcoded credentials from Secrets.h
  DEBUG_PRINT("Attempting connection to: ");
  DEBUG_PRINTLN(MYSSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(MYSSID, MYPSK);
  WiFi.setTxPower(WIFI_POWER_11dBm);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {  // 15 seconds
    delay(500);
    DEBUG_PRINT(".");
    attempts++;
  }
  DEBUG_PRINTLN();
  
  // If connected, we're done
  if (WiFi.status() == WL_CONNECTED) {
    DEBUG_PRINTLN("WiFi connected!");
    DEBUG_PRINT("IP: ");
    DEBUG_PRINTLN(WiFi.localIP());
    return;
  }
  
  // SECOND: Hardcoded credentials failed, try WiFiManager saved credentials
  DEBUG_PRINTLN("Hardcoded credentials failed, checking saved credentials...");
  
  wifiManager.setConfigPortalTimeout(180);  // 3 minute timeout
  wifiManager.setConnectTimeout(20);  // 20 second connect timeout
  
  wifiManager.setAPCallback([](WiFiManager *myWiFiManager) {
    DEBUG_PRINT("Captive portal started: ");
    DEBUG_PRINTLN(myWiFiManager->getConfigPortalSSID());
    
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(10, 80);
    tft.setTextColor(ST77XX_YELLOW);
    tft.setTextSize(2);
    tft.println("WiFi Setup");
    tft.println();
    tft.setTextColor(ST77XX_WHITE);
    tft.println("Connect to:");
    tft.setTextColor(ST77XX_GREEN);
    tft.println(HOSTNAME);
    tft.setTextColor(ST77XX_WHITE);
    tft.println("on your phone");
  });
  
  // Try to connect with saved credentials or start captive portal
  if (!wifiManager.autoConnect(HOSTNAME)) {
    DEBUG_PRINTLN("Failed to connect and timeout reached");
    DEBUG_PRINTLN("Restarting...");
    
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(10, 120);
    tft.setTextColor(ST77XX_RED);
    tft.println("WiFi Failed");
    tft.println("Restarting...");
    delay(2000);
    
    ESP.restart();
  }
  
  // Connected!
  DEBUG_PRINTLN("WiFi connected!");
  DEBUG_PRINT("IP: ");
  DEBUG_PRINTLN(WiFi.localIP());
}

// ============================================
// mDNS SETUP
// ============================================
void setupMDNS() {
  if (MDNS.begin(HOSTNAME)) {
    DEBUG_PRINT("mDNS started: ");
    DEBUG_PRINT(HOSTNAME);
    DEBUG_PRINTLN(".local");
  } else {
    DEBUG_PRINTLN("mDNS failed to start");
  }
}

// ============================================
// OTA SETUP
// ============================================
void setupOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  
  ArduinoOTA.onStart([]() {
    DEBUG_PRINTLN("OTA Start");
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(10, 120);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.println("OTA Update...");
  });
  
  ArduinoOTA.onEnd([]() {
    DEBUG_PRINTLN("\nOTA End");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    DEBUG_PRINT("Progress: ");
    DEBUG_PRINTLN((progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    DEBUG_PRINT("OTA Error: ");
    DEBUG_PRINTLN(error);
  });
  
  ArduinoOTA.begin();
  DEBUG_PRINTLN("OTA initialized");
}

// ============================================
// DOWNLOAD AND DISPLAY IMAGE FROM URL
// ============================================
void downloadAndDisplayImage() {
  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_PRINTLN("WiFi not connected");
    return;
  }
  
  DEBUG_PRINT("Downloading: ");
  DEBUG_PRINTLN(IMAGE_URL);
  
  http.begin(IMAGE_URL);
  http.setAuthorization(IMAGE_USER, IMAGE_PASS);
  
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    int size = http.getSize();
    DEBUG_PRINT("Size: ");
    DEBUG_PRINTLN(size);
    
    // Allocate temporary buffer for download only
    uint8_t* jpegBuffer = (uint8_t*)malloc(size);
    
    if (jpegBuffer == nullptr) {
      DEBUG_PRINTLN("Out of memory");
      http.end();
      return;
    }
    
    // Download JPEG
    WiFiClient* stream = http.getStreamPtr();
    int totalRead = 0;
    while (http.connected() && totalRead < size) {
      size_t available = stream->available();
      if (available) {
        int toRead = min((int)available, size - totalRead);
        totalRead += stream->readBytes(jpegBuffer + totalRead, toRead);
        DEBUG_PRINT(".");
      }
      delay(1);
    }
    DEBUG_PRINTLN();
    
    // Get JPEG dimensions
    uint16_t jpegWidth, jpegHeight;
    TJpgDec.getJpgSize(&jpegWidth, &jpegHeight, jpegBuffer, totalRead);
    
    DEBUG_PRINT("Original: ");
    DEBUG_PRINT(jpegWidth);
    DEBUG_PRINT("x");
    DEBUG_PRINTLN(jpegHeight);
    
    // Calculate aspect ratios
    float aspectSrc = (float)jpegWidth / jpegHeight;
    float aspectDst = (float)imageWidth / imageHeight;
    
    // Calculate ideal target size (one dimension edge-to-edge)
    uint16_t targetWidth, targetHeight;
    
    if (aspectSrc > aspectDst) {
      // Source is wider - fit to width
      targetWidth = imageWidth;
      targetHeight = (uint16_t)((float)imageWidth / aspectSrc);
    } else {
      // Source is taller - fit to height
      targetHeight = imageHeight;
      targetWidth = (uint16_t)((float)imageHeight * aspectSrc);
    }
    
    DEBUG_PRINT("Ideal fit: ");
    DEBUG_PRINT(targetWidth);
    DEBUG_PRINT("x");
    DEBUG_PRINTLN(targetHeight);
    
    // Find best TJpg scale (1, 2, 4, or 8) to get closest to target
    uint8_t bestScale = 1;
    int bestDiff = 99999;
    
    for (uint8_t scale = 1; scale <= 8; scale *= 2) {
      uint16_t scaledW = jpegWidth / scale;
      uint16_t scaledH = jpegHeight / scale;
      
      // Calculate how far off this scale is from our target
      int diffW = abs((int)scaledW - (int)targetWidth);
      int diffH = abs((int)scaledH - (int)targetHeight);
      int totalDiff = diffW + diffH;
      
      DEBUG_PRINT("Scale 1/");
      DEBUG_PRINT(scale);
      DEBUG_PRINT(" = ");
      DEBUG_PRINT(scaledW);
      DEBUG_PRINT("x");
      DEBUG_PRINT(scaledH);
      DEBUG_PRINT(" (diff: ");
      DEBUG_PRINT(totalDiff);
      DEBUG_PRINTLN(")");
      
      if (totalDiff < bestDiff && scaledW <= imageWidth + 50 && scaledH <= imageHeight + 50) {
        bestScale = scale;
        bestDiff = totalDiff;
      }
    }
    
    lastScale = bestScale;
    uint16_t displayWidth = jpegWidth / bestScale;
    uint16_t displayHeight = jpegHeight / bestScale;
    
    DEBUG_PRINT("Using scale 1/");
    DEBUG_PRINT(bestScale);
    DEBUG_PRINT(" = ");
    DEBUG_PRINT(displayWidth);
    DEBUG_PRINT("x");
    DEBUG_PRINTLN(displayHeight);
    
    // Center the image
    lastXOffset = (imageWidth - displayWidth) / 2;
    lastYOffset = (imageHeight - displayHeight) / 2;
    
    // Handle overflow (image slightly too big)
    if (lastXOffset < 0) lastXOffset = 0;
    if (lastYOffset < 0) lastYOffset = 0;
    
    DEBUG_PRINT("Offset: ");
    DEBUG_PRINT(lastXOffset);
    DEBUG_PRINT(", ");
    DEBUG_PRINTLN(lastYOffset);
    
    // Clear and draw
    tft.fillScreen(ST77XX_BLACK);
    TJpgDec.setJpgScale(bestScale);
    TJpgDec.drawJpg(lastXOffset, lastYOffset, jpegBuffer, totalRead);
    
    // Free buffer immediately
    free(jpegBuffer);
    
    imageLoaded = true;
    lastImageUpdate = millis();
    DEBUG_PRINTLN("Done!");
    
  } else {
    DEBUG_PRINT("HTTP Error: ");
    DEBUG_PRINTLN(httpCode);
    
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(10, 100);
    tft.setTextColor(ST77XX_RED);
    tft.setTextSize(2);
    tft.println("Error:");
    tft.println(httpCode);
  }
  
  http.end();
}

// ============================================
// CHECK FOR RFID TAGS
// ============================================
// ============================================
// CHECK FOR RFID TAGS
// ============================================
void checkRFID() {
  uint64_t cardID = readRFID();
  
  if (cardID != 0) {
    DEBUG_PRINT("RFID: 0x");
    DEBUG_PRINTF((unsigned long)cardID, HEX);
    
    displayRFID(cardID);
    
    delay(RFID_DISPLAY_TIME);
    
    // Re-download and display image
    if (imageLoaded) {
      downloadAndDisplayImage();
    }
  }
}

// ============================================
// DISPLAY RFID NUMBER ON SCREEN
// ============================================
void displayRFID(uint64_t cardID) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(3);
  
  tft.setCursor(50, 80);
  tft.println("RFID:");
  
  tft.setCursor(20, 130);
  tft.println((unsigned long)cardID);
  
  DEBUG_PRINT("Displaying RFID: ");
  DEBUG_PRINTLN((unsigned long)cardID);
}
