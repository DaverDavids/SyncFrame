#include <Arduino.h>
#include <LovyanGFX.hpp>
#include "SyncFrame_Config.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include "JPEGDEC.h"
#include "splash.h" // the small uint8_t compressed JPEG array

// Declare global instances
extern LGFX lcd;
JPEGDEC jpeg;

// WiFi / MQTT
WiFiClient espClient;
PubSubClient client(espClient);

// Timing and logic
unsigned long lastUpdate = 0;
int imageDuration = 30000; 
bool imageShowing = false;

// Function declarations
void connectWiFi();
void connectMQTT();
void callback(char* topic, byte* payload, unsigned int length);
void downloadAndDrawImage(const char* url);

// Helper for JPEGDEC
int JPEGDraw(JPEGDRAW *pDraw) {
  lcd.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
  return 1;
}

void setup() {
  Serial.begin(115200);
  
  // Initialize LCD
  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(255);
  lcd.fillScreen(TFT_BLACK);
  
  // 1. Draw the actual compressed JPEG splash logo
  Serial.println("Drawing splash logo...");
  if (jpeg.openRAM((uint8_t*)splash_logo, splash_logo_len, JPEGDraw)) {
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    // Draw it in the center! The image is smaller than the screen.
    int xPos = (lcd.width() - jpeg.getWidth()) / 2;
    int yPos = (lcd.height() - jpeg.getHeight()) / 2;
    
    // Fallback if the image is bigger than the screen (unlikely here)
    if (xPos < 0) xPos = 0;
    if (yPos < 0) yPos = 0;
    
    jpeg.decode(xPos, yPos, 0);
    jpeg.close();
  } else {
    Serial.println("Failed to open JPEG from PROGMEM!");
  }

  // 2. Setup text styling for status updates over the logo
  lcd.setTextSize(2);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK); // White text, black background box
  lcd.setTextDatum(textdatum_t::bottom_center); // Draw from bottom center

  // Connect to WiFi
  int textY = lcd.height() - 20; // 20 pixels from bottom
  lcd.drawString("Connecting to WiFi...", lcd.width()/2, textY);
  connectWiFi();
  
  // Show IP Address for 1 second
  String ipMsg = "IP: " + WiFi.localIP().toString();
  lcd.drawString(ipMsg.c_str(), lcd.width()/2, textY);
  delay(1000);

  // Connect to MQTT
  lcd.drawString("Connecting to MQTT...", lcd.width()/2, textY);
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  connectMQTT();
  
  // Ready to download
  lcd.drawString("Waiting for image...", lcd.width()/2, textY);
}

void loop() {
  if (!client.connected()) {
    connectMQTT();
  }
  client.loop();

  // If we've shown an image and the time has expired, clear the screen
  if (imageShowing && (millis() - lastUpdate > imageDuration)) {
    lcd.fillScreen(TFT_BLACK);
    imageShowing = false;
  }
}

void connectWiFi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
}

void connectMQTT() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect("SyncFrameClient", mqtt_user, mqtt_password)) {
      Serial.println("connected");
      client.subscribe(mqtt_topic);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  String url;
  for (int i = 0; i < length; i++) {
    url += (char)payload[i];
  }
  
  Serial.print("Received URL: ");
  Serial.println(url);

  // Show downloading status text at the bottom of current screen
  lcd.drawString("Downloading new image...", lcd.width()/2, lcd.height() - 20);

  downloadAndDrawImage(url.c_str());
  
  lastUpdate = millis();
  imageShowing = true;
}

void downloadAndDrawImage(const char* url) {
  HTTPClient http;
  http.begin(url);
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    int len = http.getSize();
    WiFiClient *stream = http.getStreamPtr();
    
    // Clear the screen right before we start drawing the new image
    lcd.fillScreen(TFT_BLACK);

    // We can't use jpeg.openRAM for streaming data. We have to buffer it, 
    // or use a stream decoding method. Because RAM on C3 is tight, let's 
    // allocate a buffer if it fits, or you can implement a chunked stream reader.
    
    // Let's attempt to buffer it into PSRAM (if available) or heap
    uint8_t *imgBuffer = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_8BIT);
    if (!imgBuffer) {
      Serial.println("Not enough RAM to buffer image!");
      lcd.drawString("Image too large for RAM", lcd.width()/2, lcd.height() - 20);
      http.end();
      return;
    }

    // Read the stream into buffer
    int bytesRead = 0;
    while (http.connected() && (len > 0 || len == -1)) {
      size_t size = stream->available();
      if (size) {
        int c = stream->readBytes(&imgBuffer[bytesRead], size);
        bytesRead += c;
        if (len > 0) len -= c;
      }
      delay(1);
    }

    if (jpeg.openRAM(imgBuffer, bytesRead, JPEGDraw)) {
      jpeg.setPixelType(RGB565_BIG_ENDIAN);
      
      // Center the downloaded image too
      int xPos = (lcd.width() - jpeg.getWidth()) / 2;
      int yPos = (lcd.height() - jpeg.getHeight()) / 2;
      if (xPos < 0) xPos = 0;
      if (yPos < 0) yPos = 0;

      jpeg.decode(xPos, yPos, 0);
      jpeg.close();
    } else {
      Serial.println("Failed to decode downloaded JPEG");
      lcd.drawString("Invalid JPEG file", lcd.width()/2, lcd.height() - 20);
    }

    free(imgBuffer);
  } else {
    Serial.printf("HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
    lcd.drawString("Download Failed", lcd.width()/2, lcd.height() - 20);
  }
  http.end();
}
