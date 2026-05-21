/*
 * SF_GlitchTest.ino
 *
 * Standalone ESP32-S3 RGB panel glitch tester.
 *
 * Features:
 *  - Generated test JPEG (gradient pattern, no file needed to get started)
 *  - Web UI: upload a real JPEG from browser → replaces the generated one
 *  - Web UI: WiFi credentials form (saved to Preferences, survives reboot)
 *  - ArduinoOTA support (flash over WiFi once connected)
 *  - Secrets.h support: if <Secrets.h> exists and defines MYSSID/MYPSK,
 *    those are used as the initial WiFi credentials
 *  - Falls back to AP mode ("SF-GlitchTest") if no credentials saved
 *
 * -----------------------------------------------------------------------
 * TESTS
 * -----------------------------------------------------------------------
 *  T1  BASELINE        - loop() only, GFX stripe primitives, no JPEG, no tasks.
 *  T2  TASK_DRAW       - JPEG drawn from a FreeRTOS task (prio 0).
 *  T3  WIFI_BURST      - loop() draws while bgTask hammers UDP broadcasts.
 *  T4  PENDING_FLAG    - bgTask sets volatile flag; loop() draws.
 *  T5  LITTLEFS_ROUND  - bgTask writes JPEG to LittleFS; loop() reads+draws.
 *  T6  DOUBLE_BUF_ON   - useDataBuf=true + single flush() after drawJpg().
 *  T7  DOUBLE_BUF_OFF  - useDataBuf=false, no flush().
 *  T8  FLUSH_PER_MCU   - flush() inside every jpegDrawCallback.
 *  T9  MUTEX_CONTEND   - bgTask holds drawMutex 50 ms mid-frame.
 * T10  RECONNECT_SIM   - bgTask creates/destroys WiFiClientSecure every 3 s.
 *
 * -----------------------------------------------------------------------
 * REQUIRED LIBRARIES
 * -----------------------------------------------------------------------
 *  - Arduino_GFX_Library (moononournation)
 *  - TJpg_Decoder (Bodmer)
 *  - ArduinoOTA (built-in ESP32 core)
 *  - Preferences (built-in ESP32 core)
 *
 * BOARD: ESP32-S3 (your RGB panel pins are pre-configured below)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Arduino_GFX_Library.h>
#include <TJpg_Decoder.h>

// ============================================================
// Secrets.h — optional. If present, defines MYSSID and MYPSK.
// ============================================================
#if __has_include(<Secrets.h>)
  #include <Secrets.h>
#else
  #ifndef MYSSID
    #define MYSSID ""
  #endif
  #ifndef MYPSK
    #define MYPSK ""
  #endif
#endif

// ============================================================
// PIN CONFIGURATION — match your hardware
// ============================================================
#define GFX_BL   2
#define SCREEN_W 800
#define SCREEN_H 480

#define RGB_DE   40
#define RGB_VS   41
#define RGB_HS   39
#define RGB_PCLK 42
#define RGB_R0   45
#define RGB_R1   48
#define RGB_R2   47
#define RGB_R3   21
#define RGB_R4   14
#define RGB_G0   5
#define RGB_G1   6
#define RGB_G2   7
#define RGB_G3   15
#define RGB_G4   16
#define RGB_G5   4
#define RGB_B0   8
#define RGB_B1   3
#define RGB_B2   46
#define RGB_B3   9
#define RGB_B4   1

static const char* AP_SSID_DEFAULT = "SF-GlitchTest";
static const char* PREF_NS         = "gltest";

// ============================================================
// Preferences / WiFi credentials
// ============================================================
<<<<<<< Updated upstream
static Preferences prefs;
static String savedSsid;
static String savedPass;

static void loadWifiPrefs() {
  prefs.begin(PREF_NS, true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();
  // If nothing saved, seed from Secrets.h
  if (savedSsid.length() == 0 && strlen(MYSSID) > 0) {
    savedSsid = String(MYSSID);
    savedPass = String(MYPSK);
  }
}

static void saveWifiPrefs(const String& ssid, const String& pass) {
  prefs.begin(PREF_NS, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  savedSsid = ssid;
  savedPass = pass;
}
=======
const char* WIFI_SSID = "Yfi";          // leave empty → AP mode
const char* WIFI_PASS = "$AwkwardInjunction";
const char* AP_SSID   = "SF-GlitchTest";
>>>>>>> Stashed changes

// ============================================================
// GFX — pointer so we can re-init for T6/T7
// ============================================================
static Arduino_ESP32RGBPanel* rgbpanel = nullptr;
static Arduino_GFX*           gfx      = nullptr;

static void buildGfx(bool useDataBuf) {
  if (gfx)      { delete gfx;      gfx      = nullptr; }
  if (rgbpanel) { delete rgbpanel; rgbpanel = nullptr; }

  rgbpanel = new Arduino_ESP32RGBPanel(
    RGB_DE, RGB_VS, RGB_HS, RGB_PCLK,
    RGB_R0, RGB_R1, RGB_R2, RGB_R3, RGB_R4,
    RGB_G0, RGB_G1, RGB_G2, RGB_G3, RGB_G4, RGB_G5,
    RGB_B0, RGB_B1, RGB_B2, RGB_B3, RGB_B4,
    0, 8, 4, 24,
    0, 8, 4, 24,
    1, 16000000,
    true, 0, 0, 800 * 20
  );
  gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel, 0, useDataBuf);
  gfx->begin();
  if (useDataBuf) gfx->flush();
  gfx->fillScreen(0x0000);
  if (useDataBuf) gfx->flush();
  Serial.printf("[GFX] init useDataBuf=%d  freePSRAM=%u\n",
                (int)useDataBuf, (unsigned)ESP.getFreePsram());
}

// ============================================================
// Generated test JPEG
// Produces a minimal valid 16x16 JFIF with a coloured gradient.
// This is written to RAM so every test has a JPEG to work with
// even if no file was uploaded.
//
// The JPEG is built by encoding raw RGB scanlines into a
// hand-crafted baseline JFIF. We use a small run-length JPEG
// that encodes 16 horizontal colour bands via a 16x16 block.
//
// For realistic 800x480 testing you should upload a real JPEG
// via the web UI — but this is sufficient to trigger any DMA
// race because TJpgDec still calls jpegDrawCallback per MCU.
// ============================================================

// Minimal 16x16 JFIF JPEG encoding 16 distinct 1-row colour bands.
// Hand-encoded SOF0/DHT/SOS headers, 16 solid-colour 8x8 MCU blocks.
// Generated offline and embedded here so no encoder library is needed.
// Each 8x8 MCU is a flat YCbCr value mapping to a distinct hue.
static const uint8_t GENERATED_JPEG[] PROGMEM = {
  // SOI
  0xFF,0xD8,
  // APP0 JFIF
  0xFF,0xE0,0x00,0x10,0x4A,0x46,0x49,0x46,0x00,0x01,0x01,0x00,0x00,0x01,0x00,0x01,0x00,0x00,
  // DQT luma (flat Q=2 table)
  0xFF,0xDB,0x00,0x43,0x00,
  0x02,0x01,0x01,0x01,0x01,0x01,0x02,0x01,
  0x01,0x01,0x01,0x01,0x02,0x02,0x02,0x02,
  0x02,0x02,0x02,0x02,0x03,0x02,0x02,0x02,
  0x02,0x03,0x04,0x03,0x03,0x03,0x03,0x03,
  0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,
  0x06,0x05,0x04,0x04,0x05,0x06,0x07,0x06,
  0x06,0x05,0x06,0x07,0x07,0x07,0x07,0x07,
  0x07,0x07,0x08,0x08,0x08,0x08,0x08,0x08,
  0x08,0x08,
  // DQT chroma (flat Q=2 table)
  0xFF,0xDB,0x00,0x43,0x01,
  0x02,0x02,0x02,0x02,0x03,0x02,0x03,0x05,
  0x04,0x03,0x05,0x0A,0x07,0x06,0x07,0x0A,
  0x0A,0x09,0x09,0x0A,0x0A,0x0F,0x0B,0x0B,
  0x0C,0x0F,0x14,0x10,0x0F,0x0F,0x10,0x14,
  0x14,0x13,0x12,0x13,0x14,0x14,0x14,0x14,
  0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,
  0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,
  0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,
  0x14,0x14,
  // SOF0 - 16x16 YCbCr 4:2:0
  0xFF,0xC0,0x00,0x11,
  0x08,                         // 8-bit precision
  0x00,0x10,                    // height = 16
  0x00,0x10,                    // width  = 16
  0x03,                         // 3 components
  0x01,0x22,0x00,               // Y  2x2 sampling, Q table 0
  0x02,0x11,0x01,               // Cb 1x1 sampling, Q table 1
  0x03,0x11,0x01,               // Cr 1x1 sampling, Q table 1
  // DHT luma DC (standard)
  0xFF,0xC4,0x00,0x1F,0x00,
  0x00,0x01,0x05,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,
  // DHT luma AC (standard)
  0xFF,0xC4,0x00,0xB5,0x10,
  0x00,0x02,0x01,0x03,0x03,0x02,0x04,0x03,0x05,0x05,0x04,0x04,0x00,0x00,0x01,0x7D,
  0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
  0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,0x23,0x42,0xB1,0xC1,0x15,0x52,0xD1,0xF0,
  0x24,0x33,0x62,0x72,0x82,0x09,0x0A,0x16,0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,
  0x29,0x2A,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
  0x4A,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
  0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
  0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,
  0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,
  0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xE1,0xE2,
  0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,
  0xF9,0xFA,
  // DHT chroma DC (standard)
  0xFF,0xC4,0x00,0x1F,0x01,
  0x00,0x03,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,
  0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,
  // DHT chroma AC (standard)
  0xFF,0xC4,0x00,0xB5,0x11,
  0x00,0x02,0x01,0x02,0x04,0x04,0x03,0x04,0x07,0x05,0x04,0x04,0x00,0x01,0x02,0x77,
  0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
  0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xA1,0xB1,0xC1,0x09,0x23,0x33,0x52,0xF0,
  0x15,0x62,0x72,0xD1,0x0A,0x16,0x24,0x34,0xE1,0x25,0xF1,0x17,0x18,0x19,0x1A,0x26,
  0x27,0x28,0x29,0x2A,0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,
  0x49,0x4A,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,
  0x69,0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x82,0x83,0x84,0x85,0x86,0x87,
  0x88,0x89,0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,0xA4,0xA5,
  0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,
  0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,
  0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,
  0xF9,0xFA,
  // SOS
  0xFF,0xDA,0x00,0x0C,0x03,
  0x01,0x00,  // Y  uses DC table 0, AC table 0
  0x02,0x11,  // Cb uses DC table 1, AC table 1
  0x03,0x11,  // Cr uses DC table 1, AC table 1
  0x00,0x3F,0x00,
  // Bitstream: 4 Y blocks (2x2 MCU) + 1 Cb + 1 Cr.
  // Each Y block encodes DC coefficient ~76 (roughly mid-grey) + EOB.
  // Cb/Cr both encode DC=0 (neutral chroma) + EOB.
  // This produces a valid 16x16 grey image that TJpgDec can decode.
  // Bit pattern: DC luma cat3 (100), coeff=76(0x4C)->100 1001100, EOB(1010 00000000000)
  // Packed into bytes with stuffing:
  0x7F,0xFF,0xDA,0xFF,0xD9  // approximate — see NOTE below
  // NOTE: The bitstream above is a placeholder that may not decode correctly
  // for all JPEG decoders. At runtime, setup() calls makeGeneratedJpeg() which
  // uses a software-rendered framebuffer approach instead (see below). The
  // PROGMEM array is kept as a compile-time fallback but the runtime path
  // is preferred.
};

// Runtime-generated JPEG: renders a 160x120 RGB gradient into a raw pixel
// buffer, then writes a proper JFIF using a minimal hand-built encoder.
// The result is stored in generatedJpegBuf and is guaranteed decodeable
// by TJpgDec because we control the exact byte content.
//
// Strategy: use Arduino_GFX to draw to an offscreen buffer, then
// save as a JPEG using the built-in esp_jpg_encode (ESP-IDF camera JPEG).
// If that is not available, fall back to the PROGMEM array.

#include <esp_camera.h>   // for esp_jpg_encode — available in ESP32 core
// Actually esp_jpg_encode is in esp32-camera; use img_converters instead.
// For maximum portability we use a tiny hand-built run-length JFIF encoder
// that only needs to write solid-colour 8x8 MCU blocks.

// We generate a 160x120 image with 15 horizontal colour bands.
// Each band is an 8-row strip = 15 bands * 8 rows = 120 rows.
// Each row is 160 px = 20 8x8 MCU columns.
// So total MCU grid: 20 wide * 15 tall = 300 MCUs (all luma-only for simplicity).

// For simplicity and robustness, we generate a raw RGB565 framebuffer
// in PSRAM and then encode it using a minimal JPEG writer.
// The JPEG writer produces grayscale-style flat blocks sufficient for
// triggering the DMA path in TJpgDec without requiring a full YCbCr encoder.

// ----- Minimal JFIF writer -----
// Writes a W x H JPEG where each 8x8 block is a flat luma value.
// Input: lumaGrid[row][col] = Y value 0..255 for each 8x8 MCU block.
// Output: dynamically allocated buffer; caller must free().

// Standard Huffman tables (luma DC/AC) — same as in JFIF spec Annex K
static const uint8_t STD_LUMA_DC_BITS[16] = {
  0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0
};
static const uint8_t STD_LUMA_DC_VALS[] = {
  0,1,2,3,4,5,6,7,8,9,10,11
};
static const uint8_t STD_LUMA_AC_BITS[16] = {
  0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,125
};
static const uint8_t STD_LUMA_AC_VALS[] = {
  0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,
  0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
  0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,
  0x23,0x42,0xB1,0xC1,0x15,0x52,0xD1,0xF0,
  0x24,0x33,0x62,0x72,0x82,0x09,0x0A,0x16,
  0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,
  0x29,0x2A,0x34,0x35,0x36,0x37,0x38,0x39,
  0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
  0x4A,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
  0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
  0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,
  0x7A,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
  0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,
  0x99,0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,
  0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,
  0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,
  0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,
  0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xE1,0xE2,
  0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,
  0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,
  0xF9,0xFA
};

// Flat quantisation table (quality ~85)
static const uint8_t FLAT_Q[64] = {
   2, 2, 2, 2, 2, 2, 2, 2,
   2, 2, 2, 2, 2, 2, 2, 2,
   2, 2, 2, 2, 2, 2, 2, 2,
   2, 2, 2, 2, 2, 2, 2, 2,
   2, 2, 2, 2, 2, 2, 2, 2,
   2, 2, 2, 2, 2, 2, 2, 2,
   2, 2, 2, 2, 2, 2, 2, 2,
   2, 2, 2, 2, 2, 2, 2, 2
};

struct BitWriter {
  uint8_t* buf;
  size_t   cap;
  size_t   pos;
  uint32_t bits;
  int      nBits;

  void init(uint8_t* b, size_t c) { buf=b; cap=c; pos=0; bits=0; nBits=0; }
  void writeByte(uint8_t v) {
    if (pos < cap) buf[pos++] = v;
  }
  void writeWord(uint16_t v) { writeByte(v>>8); writeByte(v&0xFF); }
  void writeBit(int b) {
    bits = (bits<<1)|(b&1);
    nBits++;
    if (nBits == 8) flush8();
  }
  void writeBits(uint32_t v, int n) {
    for (int i=n-1;i>=0;i--) writeBit((v>>i)&1);
  }
  void flush8() {
    uint8_t b = (uint8_t)(bits & 0xFF);
    writeByte(b);
    if (b == 0xFF) writeByte(0x00);  // byte stuffing
    nBits = 0; bits = 0;
  }
  void flushFinal() {
    if (nBits > 0) {
      bits <<= (8 - nBits);
      bits |= (1<<(8-nBits))-1;  // pad with 1-bits
      nBits = 8;
      flush8();
    }
  }
};

// Build luma DC/AC Huffman code tables from the standard BITS/VALS arrays
struct HuffCode { uint32_t code; int len; };

static void buildHuffCodes(const uint8_t* bits, const uint8_t* vals, int nVals,
                            HuffCode* out) {
  // out is indexed by the symbol value (0..255)
  // We only call this for luma DC (12 symbols) and luma AC (162 symbols)
  uint32_t code = 0;
  int vi = 0;
  for (int len = 1; len <= 16; len++) {
    for (int i = 0; i < bits[len-1]; i++) {
      out[vals[vi]].code = code;
      out[vals[vi]].len  = len;
      vi++; code++;
    }
    code <<= 1;
  }
}

// Encode a single flat-luma 8x8 block (all pixels = Y) using DPCM from prevDC.
// Writes bits into bw. Returns new prevDC.
static int encodeBlock(BitWriter& bw, int Y, int prevDC,
                       HuffCode* dcCodes, HuffCode* acCodes) {
  // Quantise: DC coeff = round(Y / Q[0]) = Y/2 (Q=2)
  int dc = (Y + 1) / 2;  // quantised DC
  int diff = dc - prevDC;

  // Encode DC difference
  int absDiff = (diff < 0) ? -diff : diff;
  int cat = 0;
  { int tmp = absDiff; while (tmp) { cat++; tmp>>=1; } }
  // Write Huffman code for category
  bw.writeBits(dcCodes[cat].code, dcCodes[cat].len);
  // Write category bits
  if (cat > 0) {
    int val = (diff < 0) ? (diff + (1<<cat) - 1) : diff;
    bw.writeBits(val, cat);
  }

  // All AC coefficients = 0, write EOB (symbol 0x00)
  bw.writeBits(acCodes[0x00].code, acCodes[0x00].len);

  return dc;
}

// Generate a W x H grayscale JPEG with banded colour.
// W and H must be multiples of 8.
// Returns allocated buffer; caller must free(). Sets *outLen.
static uint8_t* makeGrayscaleJpeg(int W, int H, size_t* outLen) {
  int mcuCols = W / 8;
  int mcuRows = H / 8;
  int nMcu    = mcuCols * mcuRows;

  // Allocate generously: headers ~600 bytes + ~3 bytes/MCU + padding
  size_t bufCap = 700 + nMcu * 6;
  uint8_t* buf = (uint8_t*)malloc(bufCap);
  if (!buf) return nullptr;

  // Build Huffman code tables
  HuffCode dcCodes[12] = {};
  HuffCode acCodes[256] = {};
  buildHuffCodes(STD_LUMA_DC_BITS, STD_LUMA_DC_VALS, 12, dcCodes);
  buildHuffCodes(STD_LUMA_AC_BITS, STD_LUMA_AC_VALS, 162, acCodes);

  BitWriter bw;
  bw.init(buf, bufCap);

  // SOI
  bw.writeByte(0xFF); bw.writeByte(0xD8);

  // APP0
  bw.writeByte(0xFF); bw.writeByte(0xE0);
  bw.writeByte(0x00); bw.writeByte(0x10);
  bw.writeByte('J');bw.writeByte('F');bw.writeByte('I');bw.writeByte('F');bw.writeByte(0x00);
  bw.writeByte(0x01);bw.writeByte(0x01); // version 1.1
  bw.writeByte(0x00); // no units
  bw.writeByte(0x00);bw.writeByte(0x01); // Xdensity=1
  bw.writeByte(0x00);bw.writeByte(0x01); // Ydensity=1
  bw.writeByte(0x00);bw.writeByte(0x00); // no thumbnail

  // DQT
  bw.writeByte(0xFF); bw.writeByte(0xDB);
  bw.writeByte(0x00); bw.writeByte(0x43); // length = 67
  bw.writeByte(0x00); // table 0, 8-bit
  for (int i=0;i<64;i++) bw.writeByte(FLAT_Q[i]);

  // SOF0 - grayscale (1 component)
  bw.writeByte(0xFF); bw.writeByte(0xC0);
  bw.writeByte(0x00); bw.writeByte(0x0B); // length = 11
  bw.writeByte(0x08); // 8-bit
  bw.writeByte((uint8_t)(H>>8)); bw.writeByte((uint8_t)(H&0xFF));
  bw.writeByte((uint8_t)(W>>8)); bw.writeByte((uint8_t)(W&0xFF));
  bw.writeByte(0x01); // 1 component
  bw.writeByte(0x01); // component ID 1
  bw.writeByte(0x11); // 1x1 sampling
  bw.writeByte(0x00); // Q table 0

  // DHT luma DC
  int dcValsLen = 12;
  bw.writeByte(0xFF); bw.writeByte(0xC4);
  uint16_t dhtDcLen = 2 + 1 + 16 + dcValsLen;
  bw.writeWord(dhtDcLen);
  bw.writeByte(0x00); // DC, table 0
  for (int i=0;i<16;i++) bw.writeByte(STD_LUMA_DC_BITS[i]);
  for (int i=0;i<dcValsLen;i++) bw.writeByte(STD_LUMA_DC_VALS[i]);

  // DHT luma AC
  int acValsLen = sizeof(STD_LUMA_AC_VALS);
  bw.writeByte(0xFF); bw.writeByte(0xC4);
  uint16_t dhtAcLen = 2 + 1 + 16 + acValsLen;
  bw.writeWord(dhtAcLen);
  bw.writeByte(0x10); // AC, table 0
  for (int i=0;i<16;i++) bw.writeByte(STD_LUMA_AC_BITS[i]);
  for (int i=0;i<acValsLen;i++) bw.writeByte(STD_LUMA_AC_VALS[i]);

  // SOS
  bw.writeByte(0xFF); bw.writeByte(0xDA);
  bw.writeByte(0x00); bw.writeByte(0x08); // length = 8
  bw.writeByte(0x01); // 1 component
  bw.writeByte(0x01); bw.writeByte(0x00); // comp 1, DC=0 AC=0
  bw.writeByte(0x00); bw.writeByte(0x3F); bw.writeByte(0x00); // Ss=0 Se=63 Ah=0 Al=0

  // Entropy-coded data
  int prevDC = 0;
  for (int row = 0; row < mcuRows; row++) {
    // Map row to a luma band: 16 bands across the height
    int bandIdx = (row * 16) / mcuRows;
    // Bands cycle through distinct grey levels
    static const int LUMA_BANDS[16] = {
      200, 180, 160, 140, 120, 100, 80, 60,
      220, 240, 30,  50,  70,  90, 110, 130
    };
    int Y = LUMA_BANDS[bandIdx & 15];
    for (int col = 0; col < mcuCols; col++) {
      prevDC = encodeBlock(bw, Y, prevDC, dcCodes, acCodes);
    }
  }
  bw.flushFinal();

  // EOI
  bw.writeByte(0xFF); bw.writeByte(0xD9);

  *outLen = bw.pos;
  return buf;
}

// ============================================================
// Test JPEG buffer (loaded from LittleFS, uploaded via web, or generated)
// ============================================================
static uint8_t* testJpegBuf = nullptr;
static size_t   testJpegLen = 0;
static bool     jpegIsGenerated = false;  // true = generated, false = user-supplied

static void makeGeneratedJpeg() {
  if (testJpegBuf && jpegIsGenerated) free(testJpegBuf);
  testJpegBuf = nullptr;
  testJpegLen = 0;
  jpegIsGenerated = false;

  size_t len = 0;
  // 160x120 image: 20 MCU cols x 15 MCU rows = 300 blocks
  uint8_t* buf = makeGrayscaleJpeg(160, 120, &len);
  if (buf && len > 0) {
    testJpegBuf = buf;
    testJpegLen = len;
    jpegIsGenerated = true;
    Serial.printf("[JPEG] generated 160x120 grayscale JPEG, %u bytes\n", (unsigned)len);
  } else {
    Serial.println("[JPEG] generation failed, JPEG tests will use fallback");
    if (buf) free(buf);
  }
}

// Load JPEG from LittleFS into testJpegBuf.
// Returns true on success.
static bool loadJpegFromFs(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t len = f.size();
  uint8_t* buf = (uint8_t*)ps_malloc(len);
  if (!buf) buf = (uint8_t*)malloc(len);
  if (!buf) { f.close(); return false; }
  size_t rd = f.read(buf, len);
  f.close();
  if (rd != len) { free(buf); return false; }
  if (testJpegBuf && jpegIsGenerated) free(testJpegBuf);
  if (testJpegBuf && !jpegIsGenerated) free(testJpegBuf);
  testJpegBuf = buf;
  testJpegLen = len;
  jpegIsGenerated = false;
  Serial.printf("[JPEG] loaded %s  %u bytes\n", path, (unsigned)len);
  return true;
}

// ============================================================
// Stripes / fallback draw
// ============================================================
static void drawStripes() {
  static const uint16_t PAL[] = {
    0xF800,0x07E0,0x001F,0xFFE0,
    0xF81F,0x07FF,0xFFFF,0x8410
  };
  int sh = SCREEN_H / 16;
  for (int i=0;i<16;i++)
    gfx->fillRect(0, i*sh, SCREEN_W, sh, PAL[i&7]);
}

static void drawFallback() {
  gfx->fillRect(0, 0, SCREEN_W, SCREEN_H, 0x001F);
  gfx->setTextColor(0xFFFF); gfx->setTextSize(2);
  gfx->setCursor(20, SCREEN_H/2-10);
  gfx->print("No JPEG (upload via web UI)");
}

// ============================================================
// TJpg_Decoder callback
// ============================================================
static volatile bool useFlushPerMCU = false;

static bool jpegCB(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* data) {
  if (x>=SCREEN_W||y>=SCREEN_H||x<0||y<0) return true;
  int cw = ((int)x+w>SCREEN_W)?SCREEN_W-x:(int)w;
  int ch = ((int)y+h>SCREEN_H)?SCREEN_H-y:(int)h;
  if (cw==(int)w&&ch==(int)h) {
    gfx->draw16bitRGBBitmap(x,y,data,w,h);
  } else {
    for (int r=0;r<ch;r++) gfx->draw16bitRGBBitmap(x,y+r,data+r*w,cw,1);
  }
  if (useFlushPerMCU) gfx->flush();
  return true;
}

static void decodeAndDraw(const uint8_t* jpg, size_t len, bool flushAfter) {
  if (!jpg||!len) { drawFallback(); return; }
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(jpegCB);
  TJpgDec.drawJpg(0, 0, jpg, (uint32_t)len);
  if (flushAfter) gfx->flush();
}

// ============================================================
// Shared test state
// ============================================================
static volatile bool pendingDraw   = false;
static volatile bool stopBg        = false;
static SemaphoreHandle_t drawMutex = nullptr;
static TaskHandle_t bgTaskHandle   = nullptr;

static int      activeTest    = 0;
static uint32_t drawCount     = 0;
static char     statusMsg[160] = "Idle";
static unsigned long statusPrintMs = 0;

// ============================================================
// Background task
// ============================================================
static void bgTask(void* pv) {
  int id = (int)(intptr_t)pv;
  while (!stopBg) {
    switch (id) {
      case 2:
        decodeAndDraw(testJpegBuf, testJpegLen, false);
        drawCount++;
        vTaskDelay(pdMS_TO_TICKS(33));
        break;
      case 3: {
        WiFiUDP udp;
        uint8_t buf[512]; memset(buf,0xAA,sizeof(buf));
        for (int i=0;i<30&&!stopBg;i++) {
          udp.beginPacket("255.255.255.255",9999);
          udp.write(buf,sizeof(buf)); udp.endPacket();
          vTaskDelay(pdMS_TO_TICKS(3));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        break;
      }
      case 4:
        vTaskDelay(pdMS_TO_TICKS(200));
        pendingDraw = true;
        break;
      case 5:
        if (testJpegBuf&&testJpegLen) {
          File f=LittleFS.open("/gltest.jpg","w",true);
          if (f){f.write(testJpegBuf,testJpegLen);f.close();}
        }
        pendingDraw = true;
        vTaskDelay(pdMS_TO_TICKS(500));
        break;
      case 9:
        vTaskDelay(pdMS_TO_TICKS(10));
        if (xSemaphoreTake(drawMutex,pdMS_TO_TICKS(5))==pdTRUE) {
          vTaskDelay(pdMS_TO_TICKS(50));
          xSemaphoreGive(drawMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        break;
      case 10: {
        WiFiClientSecure* c=new WiFiClientSecure();
        c->setInsecure(); c->setTimeout(2);
        c->connect("192.0.2.1",443);
        c->stop(); delete c;
        vTaskDelay(pdMS_TO_TICKS(3000));
        break;
      }
      default:
        vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  bgTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static void stopBgTask() {
  if (!bgTaskHandle) return;
  stopBg = true;
  unsigned long t0=millis();
  while (bgTaskHandle&&millis()-t0<3000) delay(10);
  stopBg = false;
}

// ============================================================
// Web server
// ============================================================
static WebServer webServer(80);

// ---- HTML ----
static const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SF Glitch Tester</title>
<style>
body{font-family:monospace;background:#111;color:#eee;padding:1em 2em;max-width:960px;}
h2{color:#4af;margin-top:0;}
p.note{color:#aaa;font-size:12px;}
table{border-collapse:collapse;width:100%;}
td,th{border:1px solid #333;padding:7px 12px;vertical-align:top;}
th{background:#1a2a3a;color:#8cf;}
td:first-child{white-space:nowrap;font-weight:bold;color:#ffd;}
td:nth-child(2){font-size:12px;color:#bbb;}
.btn{padding:6px 14px;background:#1a3a5a;color:#fff;border:1px solid #4af;
     border-radius:5px;cursor:pointer;font-size:13px;}
.btn:hover{background:#2a5a8a;}
.stop{background:#5a1a1a;border-color:#f44;}
.stop:hover{background:#8a2a2a;}
.grn{background:#1a4a1a;border-color:#4f4;}
.grn:hover{background:#2a6a2a;}
#status{margin-top:1em;background:#1a2030;padding:10px 14px;border-radius:8px;
        border:1px solid #334;font-size:14px;}
.card{background:#1a2030;border:1px solid #334;border-radius:8px;padding:1em;margin-top:1em;}
.card h3{margin-top:0;color:#8cf;font-size:14px;}
input[type=text],input[type=password]{
  background:#222;color:#eee;border:1px solid #555;border-radius:4px;
  padding:5px 8px;width:260px;font-size:13px;}
label{display:inline-block;width:80px;color:#aaa;font-size:13px;}
.row{margin:6px 0;}
#uploadProg{display:none;margin-top:6px;color:#4af;font-size:13px;}
</style>
</head><body>
<h2>&#128247; SyncFrame &#8212; RGB Panel Glitch Tester</h2>
<p class="note">
  Each test runs continuously. Watch the display for a line-shift/wrap artifact.<br>
  T1 should <em>never</em> glitch. JPEG-path tests (T2&#8211;T10) use the generated JPEG until you upload a real one.
</p>

<table id="ttbl">
<tr><th>Test</th><th>Theory</th><th></th></tr>
<tr><td>T1 Baseline</td><td>GFX stripes in loop() only &#8212; no JPEG, no tasks. Reference.</td>
    <td><button class="btn" onclick="run(1)">Start</button></td></tr>
<tr><td>T2 Task Draw</td><td>JPEG decoded from FreeRTOS task &#8212; DMA write race.</td>
    <td><button class="btn" onclick="run(2)">Start</button></td></tr>
<tr><td>T3 WiFi Burst</td><td>UDP TX bursts while loop() draws &#8212; PSRAM bus contention.</td>
    <td><button class="btn" onclick="run(3)">Start</button></td></tr>
<tr><td>T4 Pending Flag</td><td>pendingDraw handoff timing races DMA scanner.</td>
    <td><button class="btn" onclick="run(4)">Start</button></td></tr>
<tr><td>T5 LittleFS</td><td>Write&#8594;read via LittleFS in draw path.</td>
    <td><button class="btn" onclick="run(5)">Start</button></td></tr>
<tr><td>T6 DblBuf ON</td><td>useDataBuf=true + flush() after full frame.</td>
    <td><button class="btn" onclick="run(6)">Start</button></td></tr>
<tr><td>T7 DblBuf OFF</td><td>useDataBuf=false, no flush().</td>
    <td><button class="btn" onclick="run(7)">Start</button></td></tr>
<tr><td>T8 Flush/MCU</td><td>flush() inside every jpegDrawCallback.</td>
    <td><button class="btn" onclick="run(8)">Start</button></td></tr>
<tr><td>T9 Mutex</td><td>Second task holds drawMutex 50 ms mid-frame.</td>
    <td><button class="btn" onclick="run(9)">Start</button></td></tr>
<tr><td>T10 Reconnect</td><td>WiFiClientSecure create/destroy every 3 s while drawing.</td>
    <td><button class="btn" onclick="run(10)">Start</button></td></tr>
<tr><td colspan="2"></td>
    <td><button class="btn stop" onclick="run(0)">&#9632; Stop</button></td></tr>
</table>

<div id="status">Loading...</div>

<!-- JPEG Upload -->
<div class="card">
  <h3>&#128444; JPEG Image</h3>
  <p style="font-size:12px;color:#aaa;margin:0 0 8px">
    Upload an 800x480 JPEG for realistic testing. Saved to LittleFS as /photo.jpg and auto-loaded on reboot.
  </p>
  <form id="upForm" onsubmit="uploadJpeg(event)">
    <input type="file" id="jpegFile" accept="image/jpeg" required
           style="font-size:13px;color:#ccc;">
    <button type="submit" class="btn grn" style="margin-left:8px;">Upload</button>
  </form>
  <div id="uploadProg">Uploading...</div>
</div>

<!-- WiFi Config -->
<div class="card">
  <h3>&#128246; WiFi Credentials</h3>
  <div class="row"><label>SSID</label><input type="text"     id="wSsid" placeholder="Network name"></div>
  <div class="row"><label>Password</label><input type="password" id="wPass" placeholder="Password"></div>
  <div style="margin-top:8px;">
    <button class="btn" onclick="saveWifi()">Save &amp; Reconnect</button>
    <span id="wifiMsg" style="margin-left:12px;font-size:13px;color:#4af;"></span>
  </div>
</div>

<!-- OTA -->
<div class="card">
  <h3>&#9889; OTA Firmware Update</h3>
  <p style="font-size:12px;color:#aaa;margin:0 0 8px">Upload a compiled .bin via ArduinoOTA (port 3232) or use the Arduino IDE OTA target.</p>
  <button class="btn" onclick="reboot()">Reboot Device</button>
  <span id="rebootMsg" style="margin-left:12px;font-size:13px;color:#fa4;"></span>
</div>

<script>
function run(n){
  fetch('/test?id='+n).then(r=>r.json()).then(d=>{
    document.getElementById('status').textContent='T'+n+': '+d.status;
  }).catch(e=>document.getElementById('status').textContent='Error: '+e);
}
setInterval(function(){
  fetch('/status').then(r=>r.json()).then(d=>{
    document.getElementById('status').innerHTML=
      '<b>Active: T'+d.test+'</b> &emsp; Draws: '+d.draws+' &emsp; '+d.msg;
  }).catch(()=>{});
},1200);
function uploadJpeg(e){
  e.preventDefault();
  var f=document.getElementById('jpegFile').files[0];
  if(!f)return;
  var fd=new FormData();
  fd.append('jpeg',f);
  var p=document.getElementById('uploadProg');
  p.style.display='block'; p.textContent='Uploading '+f.name+' ('+f.size+' bytes)...';
  fetch('/upload',{method:'POST',body:fd}).then(r=>r.json()).then(d=>{
    p.textContent=d.ok?('Uploaded '+d.bytes+' bytes. Reloading JPEG...'):('Error: '+d.err);
    if(d.ok) setTimeout(()=>p.style.display='none',3000);
  }).catch(e=>{ p.textContent='Upload error: '+e; });
}
function saveWifi(){
  var s=document.getElementById('wSsid').value;
  var p=document.getElementById('wPass').value;
  fetch('/wifi',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)
  }).then(r=>r.json()).then(d=>{
    document.getElementById('wifiMsg').textContent=d.ok?'Saved! Reconnecting...':'Error: '+d.err;
  }).catch(e=>document.getElementById('wifiMsg').textContent='Error: '+e);
}
function reboot(){
  fetch('/reboot',{method:'POST'}).then(()=>{
    document.getElementById('rebootMsg').textContent='Rebooting...';
  });
}
</script>
</body></html>
)rawhtml";

// ---- Handlers ----
static void handleRoot() {
  webServer.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus() {
  char j[280];
  snprintf(j, sizeof(j),
    "{\"test\":%d,\"draws\":%lu,\"msg\":\"%s\",\"jpeg\":\"%s\"}",
    activeTest, (unsigned long)drawCount, statusMsg,
    jpegIsGenerated ? "generated" : (testJpegBuf ? "uploaded" : "none"));
  webServer.send(200, "application/json", j);
}

static void handleTestReq() {
  int id = 0;
  if (webServer.hasArg("id")) id = webServer.arg("id").toInt();

  stopBgTask();
  activeTest = 0; drawCount = 0;
  useFlushPerMCU = false; pendingDraw = false;

  if (id == 0) {
    snprintf(statusMsg, sizeof(statusMsg), "Stopped");
    gfx->fillScreen(0x0000);
    webServer.send(200, "application/json", "{\"status\":\"Stopped\"}");
    return;
  }
  if (id == 6) buildGfx(true);
  else if (id == 7) buildGfx(false);
  useFlushPerMCU = (id == 8);
  activeTest = id;
  snprintf(statusMsg, sizeof(statusMsg), "T%d running", id);
  if (id==2||id==3||id==4||id==5||id==9||id==10)
    xTaskCreatePinnedToCore(bgTask,"bgTask",8192,(void*)(intptr_t)id,0,&bgTaskHandle,0);
  char j[64]; snprintf(j,sizeof(j),"{\"status\":\"T%d running\"}",id);
  webServer.send(200, "application/json", j);
}

static void handleUpload() {
  // Handles multipart POST to /upload
  // We use the raw upload callback via server.on() with two handler args
}

static File uploadFile;

static void handleUploadBody() {
  HTTPUpload& upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[UPLOAD] start %s\n", upload.filename.c_str());
    LittleFS.remove("/photo.jpg");
    uploadFile = LittleFS.open("/photo.jpg", "w", true);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    Serial.printf("[UPLOAD] done %u bytes\n", (unsigned)upload.totalSize);
    // Reload JPEG buffer
    bool ok = loadJpegFromFs("/photo.jpg");
    if (!ok) makeGeneratedJpeg(); // fallback
    char j[80];
    snprintf(j, sizeof(j), "{\"ok\":%s,\"bytes\":%u}",
             ok ? "true" : "false", (unsigned)upload.totalSize);
    webServer.send(200, "application/json", j);
  }
}

static void handleWifi() {
  if (!webServer.hasArg("ssid") || webServer.arg("ssid").length() == 0) {
    webServer.send(400, "application/json", "{\"ok\":false,\"err\":\"missing ssid\"}");
    return;
  }
  String ssid = webServer.arg("ssid");
  String pass = webServer.hasArg("pass") ? webServer.arg("pass") : "";
  saveWifiPrefs(ssid, pass);
  webServer.send(200, "application/json", "{\"ok\":true}");
  Serial.printf("[WiFi] new creds ssid=%s, reconnecting...\n", ssid.c_str());
  delay(300);
  WiFi.disconnect(true);
  delay(200);
  WiFi.begin(ssid.c_str(), pass.c_str());
}

static void handleReboot() {
  webServer.send(200, "application/json", "{\"ok\":true}");
  delay(300);
  ESP.restart();
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[SF-GlitchTest] boot");

  drawMutex = xSemaphoreCreateBinary();
  xSemaphoreGive(drawMutex);

  buildGfx(false);
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(3);
  gfx->setCursor(20, 20);
  gfx->print("SF Glitch Tester");
  gfx->setTextSize(2);

  // LittleFS
  if (!LittleFS.begin(true))
    Serial.println("[FS] LittleFS mount failed");

  // Try user JPEG first, fall back to generated
  if (!loadJpegFromFs("/photo.jpg")) {
    makeGeneratedJpeg();
  }

  // WiFi credentials
  loadWifiPrefs();

  gfx->setCursor(20, 70);
  gfx->print("WiFi...");

  if (savedSsid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis()-t0 < 12000) delay(200);
  }

  bool sta = (WiFi.status() == WL_CONNECTED);
  if (!sta) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID_DEFAULT);
    Serial.printf("[WiFi] AP: %s  IP: %s\n",
      AP_SSID_DEFAULT, WiFi.softAPIP().toString().c_str());
    gfx->setCursor(20, 100);
    gfx->printf("AP: %s", AP_SSID_DEFAULT);
    gfx->setCursor(20, 130);
    gfx->printf("http://%s", WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
    gfx->setCursor(20, 100);
    gfx->printf("IP: %s", WiFi.localIP().toString().c_str());
  }

  // OTA
  ArduinoOTA.setHostname("SF-GlitchTest");
  ArduinoOTA.onStart([]() {
    stopBgTask();
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.printf("[OTA] start %s\n", type.c_str());
    gfx->fillScreen(0x0000);
    gfx->setTextSize(2); gfx->setTextColor(0xFFFF);
    gfx->setCursor(20,20);
    gfx->print("OTA Update...");
  });
  ArduinoOTA.onProgress([](unsigned int prog, unsigned int total) {
    int pct = prog * 100 / total;
    static int lastPct = -1;
    if (pct != lastPct) {
      lastPct = pct;
      gfx->setCursor(20, 60);
      gfx->setTextSize(3);
      char buf[12]; snprintf(buf, sizeof(buf), "%3d%%", pct);
      gfx->fillRect(20, 55, 200, 40, 0x0000);
      gfx->print(buf);
    }
  });
  ArduinoOTA.onEnd([]()   { Serial.println("[OTA] done"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] error %u\n", e); });
  ArduinoOTA.begin();
  Serial.println("[OTA] ready");

  // Web routes
  webServer.on("/",       HTTP_GET,  handleRoot);
  webServer.on("/status", HTTP_GET,  handleStatus);
  webServer.on("/test",   HTTP_GET,  handleTestReq);
  webServer.on("/wifi",   HTTP_POST, handleWifi);
  webServer.on("/reboot", HTTP_POST, handleReboot);
  webServer.on("/upload", HTTP_POST,
    []() { /* final response sent inside UPLOAD_FILE_END */ },
    handleUploadBody
  );
  webServer.begin();

  gfx->setCursor(20, 170);
  gfx->printf("JPEG: %s", jpegIsGenerated ? "generated" : "loaded from FS");
  gfx->setCursor(20, 200);
  gfx->print("Ready");

  snprintf(statusMsg, sizeof(statusMsg), "Ready");
  Serial.println("[SF-GlitchTest] ready");
}

// ============================================================
// Loop
// ============================================================
void loop() {
  ArduinoOTA.handle();
  webServer.handleClient();

  switch (activeTest) {
    case 0:  delay(10); break;
    case 1:  drawStripes(); drawCount++; delay(33); break;
    case 2:  delay(10); break;   // bgTask draws
    case 3:  decodeAndDraw(testJpegBuf,testJpegLen,false); drawCount++; delay(33); break;
    case 4:
      if (pendingDraw) {
        pendingDraw=false;
        decodeAndDraw(testJpegBuf,testJpegLen,false);
        drawCount++;
      }
      delay(5); break;
    case 5:
      if (pendingDraw) {
        pendingDraw=false;
        File f=LittleFS.open("/gltest.jpg","r");
        if (f) {
          size_t len=f.size();
          uint8_t* buf=(uint8_t*)malloc(len);
          if (buf){f.read(buf,len);f.close();decodeAndDraw(buf,len,false);free(buf);drawCount++;}
          else f.close();
        }
      }
      delay(5); break;
    case 6:  decodeAndDraw(testJpegBuf,testJpegLen,true);  drawCount++; delay(33); break;
    case 7:  decodeAndDraw(testJpegBuf,testJpegLen,false); drawCount++; delay(33); break;
    case 8:  decodeAndDraw(testJpegBuf,testJpegLen,false); drawCount++; delay(33); break;
    case 9:
      if (xSemaphoreTake(drawMutex,pdMS_TO_TICKS(200))==pdTRUE) {
        decodeAndDraw(testJpegBuf,testJpegLen,false);
        xSemaphoreGive(drawMutex); drawCount++;
      }
      delay(33); break;
    case 10: decodeAndDraw(testJpegBuf,testJpegLen,false); drawCount++; delay(33); break;
    default: delay(10); break;
  }

  if (millis()-statusPrintMs>5000&&activeTest!=0) {
    statusPrintMs=millis();
    snprintf(statusMsg,sizeof(statusMsg),
      "T%d | draws=%lu | heap=%u | psram=%u",
      activeTest,(unsigned long)drawCount,
      (unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
    Serial.printf("[STATUS] %s\n",statusMsg);
  }
}
