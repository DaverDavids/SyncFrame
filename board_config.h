#pragma once

// Choose board-specific setup based on chip target.
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ARDUINO_ESP32C3_DEV)
  #include "config_c3.h"
#elif defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)
  #include "config_s3.h"
#else
  #warning "Board target not automatically identified. Defaulting to ESP32-S3 configuration."
  #include "config_s3.h"
#endif

// Board interface expected by SyncFrame.ino:
// void board_init();
// void board_loop();
// int  board_screen_w();
// int  board_screen_h();
// void board_fill_black();
// void board_draw_rgb565_block(int x, int y, int w, int h, const uint16_t* pixels);
