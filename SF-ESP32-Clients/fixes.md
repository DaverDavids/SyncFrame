# Pixel Shift Bug — Debug Log

**Board:** ESP32-S3 (JC8048W550, 800×480 RGB parallel panel, GT911 touch)
**Library stack:** GFX Library for Arduino v1.6.5 (moononournation), TJpg_Decoder v1.1.0 (Bodmer), TAMC_GT911 v1.0.2
**Arduino core tested:** esp32 3.3.7, reverting to 3.1.1 for comparison
**File:** `config_s3.h` + `board_config.h`

---

## 2026-04-07 — Initial Investigation & All Attempts So Far

### Symptom Description

- When a JPEG is drawn to the screen via `TJpgDec.drawJpg()`, the image is sometimes shifted **vertically upward by a fixed amount** — the top ~10% of the image wraps around and appears at the bottom of the screen.
- The shift amount is always a **fixed multiple of 16 pixels** (typically 16 px or 32 px). Never random/arbitrary.
- Horizontal glitch lines flash briefly when the shift first occurs, then disappear, leaving the image stably shifted.
- The bad state **persists** until the next draw. Forcing a redraw (web UI refresh button) can go good or bad randomly.
- **Both** `currentJpg` and `lastJpg` (touch-held view) are shifted by the same amount when bad — meaning the shift is baked into the framebuffer, not a display-time artifact.
- The issue is **intermittent and probabilistic** — multiple force-refreshes will cycle through good and bad renders.
- Works perfectly on the ESP32-C3 with its SPI/parallel LCD (different panel architecture — has its own controller buffer, no continuous DMA streaming).

### Root Cause Theory (Current Best Understanding)

The ESP32-S3 RGB LCD peripheral streams pixel data **continuously** from a PSRAM-backed framebuffer via DMA at ~16 MHz pixel clock (~24 ms/frame). The DMA scanner runs non-stop regardless of CPU activity.

`TJpgDec.drawJpg()` decodes the JPEG and writes pixels to the framebuffer in **16×16 MCU blocks**, top-to-bottom, over ~200–400 ms. During this time the DMA scanner laps the CPU write pointer multiple times. Normally this is harmless because both travel top-to-bottom. However, if a CPU stall occurs mid-decode (WiFi interrupt, MQTT, HTTP activity, cache miss), `drawJpg` pauses long enough for the DMA scanner to **catch up and overtake** the write pointer. On the next VSYNC, the LCD controller latches the wrong line as "frame start," shifting all subsequent rendering by exactly the number of MCU rows that were partially written at the moment of the stall.

The **fixed 16 px / 32 px shift amount** confirms this theory: it is always a multiple of one TJpgDec MCU row (16 pixels tall), not a random byte offset that a pure memory error would produce.

The **bounce buffer** (800×20 lines of SRAM) decouples PSRAM latency from the DMA clock and eliminates a horizontal wobble artifact, but does **not** prevent the vertical frame-start desync because the frame-start latch is a separate mechanism from pixel-clock timing.

### Hardware Configuration (at time of testing)

```cpp
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40, 41, 39, 42,          // DE, VSYNC, HSYNC, PCLK
  45, 48, 47, 21, 14,      // R0-R4
  5,  6,  7,  15, 16, 4,   // G0-G5
  8,  3,  46, 9,  1,        // B0-B4
  0, 8, 4, 24,              // hsync_polarity, hsync_front_porch, hsync_pulse_width, hsync_back_porch
  0, 8, 4, 24,              // vsync_polarity, vsync_front_porch, vsync_pulse_width, vsync_back_porch
  1, 16000000,              // pclk_active_neg, prefer_speed (pixel clock Hz)
  true, 0, 0, 800*20        // auto_flush, x_gap, y_gap, bounce_buffer_size_px
);
```

- PSRAM: OPI (octal), enabled (`-DBOARD_HAS_PSRAM`)
- Arduino core loop/event cores: both set to Core 1
- Single-buffer mode (double-buffer removed — caused partial frame issues and watchdog reboots)
- `photoTask` and `otaUpdateTask` pinned to `APP_CORE` (Core 1)

---

### Things Tried That Did NOT Fix It

#### 1. Adding decoupling capacitors to 3.3V rail
- Added 0.1 µF, 1 µF, and 10 µF ceramic capacitors on the ESP32-S3 3.3V power rail.
- **Result:** No change whatsoever.

#### 2. Double-buffering
- Re-enabled double-buffer mode in `Arduino_ESP32RGBPanel`.
- **Result:** Did not fix the vertical shift. Also introduced partial-frame rendering artifacts and caused watchdog reboots from re-entrant GFX calls on Core 0. Removed.

#### 3. Increasing bounce buffer size
- Increased from `800*20` to `800*40`.
- **Result:** No improvement on vertical shift. (Bounce buffer addresses horizontal wobble from PSRAM latency jitter, not frame-start desync.) Values above `800*20` caused boot failures on this board.

#### 4. Pinning tasks to cores / keeping loop on Core 1
- Ensured `photoTask`, `mqttReconnectTask`, `otaUpdateTask` all run on Core 1 (`APP_CORE = 1`).
- Arduino IDE `LoopCore=1`, `EventsCore=1`.
- **Result:** Reduced frequency slightly but did not eliminate the issue.

#### 5. `boardDrawActive` flag to prevent re-entrant draws
- Set `boardDrawActive = true` at the start of `board_draw_jpeg()` and `false` at the end.
- `board_loop()` (touch handler) and `board_draw_boot_status()` both check this flag and bail early.
- **Result:** Prevented re-entrant draw crashes and the worst glitching, but did not fix the vertical shift.

#### 6. Replacing `fillScreen()` with targeted `fillRect()` for letterbox bars only
- Eliminated the full `fillScreen(0x0000)` call before drawing, which was racing the DMA scanner over all 384,000 pixels.
- Now only fills the small letterbox strips that the JPEG does not paint.
- **Result:** Eliminated one class of artifact (full-frame race) but vertical shift persists for full 800×480 images where no letterbox fill fires at all.

#### 7. Reducing PCLK speed
- Tried reducing pixel clock below 16 MHz.
- **Result:** LCD panel stops functioning below ~14.8–15 MHz (panel has a minimum clock requirement). At 15 MHz frame period is ~26 ms; does not meaningfully change the race window given ~200–400 ms JPEG decode time. Reverted to 16 MHz.

#### 8. Blind `vTaskDelay(26)` before `drawJpg()` (attempted VSYNC sync)
- Added `vTaskDelay(pdMS_TO_TICKS(26))` (one frame period at 16 MHz) before calling `TJpgDec.drawJpg()`, intending to align the write start with the top of a fresh frame.
- **Result:** Did not fix the shift. A blind delay has no knowledge of where in the frame cycle the DMA scanner currently is, so it only helps statistically, not deterministically.

#### 9. True VSYNC semaphore via `esp_lcd_rgb_panel_register_event_callbacks()` — compile failure
- Attempted to register an ISR-backed VSYNC semaphore using the IDF's `esp_lcd_rgb_panel_event_callbacks_t` / `on_vsync` callback.
- Requires the `esp_lcd_panel_handle_t`, which is held in `Arduino_ESP32RGBPanel::_panel_handle`.
- **Problem 1:** `_panel_handle` is `private` in GFX Library for Arduino v1.6.5. Direct access fails to compile.
- **Problem 2:** Attempted a subclass (`Arduino_ESP32RGBPanel_Exposed : public Arduino_ESP32RGBPanel`) with a `getPanelHandle()` getter. C++ subclasses cannot access `private` members of a parent class — compile error.
- **Problem 3:** Attempted to patch the library locally (`private` → `protected`). Works but is fragile — patch is lost on library reinstall/update.
- **Result:** Never successfully compiled or tested. The IDF callback mechanism is the correct architectural fix but is blocked by the library's access modifier.

---

### Partial Fixes Applied (Currently in Codebase)

- `boardDrawActive` flag (re-entry guard)
- Letterbox-only `fillRect()` instead of `fillScreen()`
- Bounce buffer `800*20`
- All tasks on Core 1

### Next Things to Try

- Test with Arduino core **3.1.1** instead of 3.3.7 — determine if the issue is core-version-dependent (IDF 5.1 vs 5.3 DMA/LCD driver differences).
- **Submit PR to moononournation** to add a public `getPanelHandle()` getter to `Arduino_ESP32RGBPanel`. This is the legitimate upstream fix that unblocks the VSYNC semaphore approach.
- If the library adds the getter (or `protected:` access): implement `esp_lcd_rgb_panel_register_event_callbacks()` with `on_vsync` posting a FreeRTOS binary semaphore, and take that semaphore (draining stale first) at the top of `board_draw_jpeg()` before calling `drawJpg()`. This is the correct deterministic fix.
- Investigate whether `CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y` + `CONFIG_SPIRAM_RODATA=y` (moves code/rodata to PSRAM, freeing PSRAM bus for DMA) reduces stall frequency enough to matter.
- Consider decoding JPEG to a **separate RAM buffer** first, then doing a single `memcpy` or DMA transfer to the framebuffer timed to VSYNC — this would make the "write window" a fast memcpy (~10 ms) instead of a slow JPEG decode (~300 ms), dramatically reducing the race window.

---

## 2026-04-08 -- Arduino 3.1.1
Changed to Arduino core 3.1.1 and seeing if it fixes.

*Append new entries below this line with date/timestamp heading.*

---

## 2026-04-09 — MQTT Double-Free Crash (separate crash, now fixed)

### Crash Summary

**Panic:** `assert failed: multi_heap_free multi_heap_poisoning.c:279 (head != NULL)`
**Crashed task:** `loopTask`
**Exception:** `StoreProhibitedCause` (excvaddr=0x0)

This is a **separate crash from the pixel-shift bug** — a heap corruption triggered by a concurrent double-free of the same mbedTLS buffer.

### Root Cause (confirmed by coredump)

Two threads were simultaneously inside `mbedtls_ssl_free()` on the **same SSL context** (`ssl=0x3fca8428`):

- **Thread 1 (`loopTask`):** `loop()` → `mqttMaybeReconnect()` → `mqtt.connected()` → `NetworkClientSecure::connected()` → `NetworkClientSecure::available()` → `NetworkClientSecure::stop()` → `mbedtls_ssl_free(ptr=0x3fcd4fe4)`
- **Thread 2 (`mqttRecon`):** `mqttReconnectTask()` → `mqtt.connect()` → `PubSubClient::connect()` → `NetworkClientSecure::stop()` → `mbedtls_ssl_free(ptr=0x3fcd4fe4)`

Both threads freed the **same internal TLS record buffer** (`ptr=0x3fcd4fe4`, len=16717). The second free corrupted the heap block header (`head != NULL` assert).

**Why it happened:** `mqttReconnectTask` intentionally releases `mqttMutex` *before* calling `mqtt.connect()` to avoid blocking lwIP. This is correct. However, `mqttMaybeReconnect()` in `loop()` then called `mqtt.connected()` (which probes the SSL socket state and can trigger an internal `stop()`) without verifying that the reconnect task had actually finished. The `mqttTaskRunning` guard that prevents spawning a new task was checked **after** the mutex try and the `mqtt.connected()` call — too late to block the race.

### Fix Applied

Moved the `mqttTaskRunning` check to the **very top** of `mqttMaybeReconnect()`, before any mutex or `mqtt.connected()` call:

```cpp
static void mqttMaybeReconnect() {
  if (mqttTaskRunning) return;                    // NEW: bail before any SSL access
  if (xSemaphoreTake(mqttMutex, 0) != pdTRUE) return;
  bool connected = mqtt.connected();
  xSemaphoreGive(mqttMutex);
  ...
  // removed: if (mqttTaskRunning) return;  (was checked too late)
```

This ensures `loop()` never touches `mqtt.connected()` — and therefore never enters the shared SSL context — while `mqttReconnectTask` is active.

### Files Changed

- `SF-ESP32-Clients/SF-ESP32-Clients.ino` — `mqttMaybeReconnect()` function
