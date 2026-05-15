// Define board target BEFORE including shared code so board_config.h
// selects config_viewe7.h instead of falling through to config_s3.h.
// (build_property in sketch.yaml is silently ignored by arduino-cli;
//  this #define is the reliable alternative.)
#define BOARD_TARGET_VIEWE7

#include "../SF-ESP32-Clients.ino"
