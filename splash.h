#pragma once
#include <Arduino.h>

// IMPORTANT: Do not use an "image to C array" tool that outputs RGB565 / uint16_t!
// The drawing function expects a COMPRESSED JPEG file buffer (uint8_t).
// You need to use a "File to Hex" converter that simply reads your .jpg file 
// byte-by-byte. The resulting array should start with 0xFF, 0xD8, 0xFF.

const uint8_t splash_logo[] PROGMEM = {
  // Replace this with the raw bytes of your compressed .jpg file!
  0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x01, 0x00, 0x48, 
  0xFF, 0xD9
};

const size_t splash_logo_len = sizeof(splash_logo);
