#pragma once
#include <Arduino.h>

// You can replace this array with your actual 800x480 JPEG hex dump.
// Even though it's 800x480, the C3 will automatically downscale it to fit its 280x240 screen
// perfectly, using the exact same logic it uses for downloaded photos.
// If you ever want board-specific logos, you can use #if defined(...) here just like board_config.h
const uint8_t splash_logo[] PROGMEM = {
  0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x01, 0x00, 0x48, 
  // ... paste rest of array here ...
  0xFF, 0xD9
};
const size_t splash_logo_len = sizeof(splash_logo);
