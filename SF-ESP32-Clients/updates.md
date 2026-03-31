
## 1. `board_loop()` Calls `showCurrentPhoto()` / `showLastPhoto()` Without the Mutex

This is the **primary remaining culprit**. Look at `board_loop()` in `config_s3.h`:

```cpp
void board_loop() {
  ts.read();
  bool pressed = ts.isTouched;
  if (pressed && !showingLast && hasLastPhoto()) {
    showLastPhoto();       // ← calls gfx->draw16bitRGBBitmap directly
  } else if (!pressed && showingLast) {
    showCurrentPhoto();    // ← same
  }
}
```

`board_loop()` is called from `loop()` on Core 1. But `showCurrentPhoto()` and `showLastPhoto()` in the `.ino` **do take the mutex** — however `board_loop()` in `config_s3.h` calls them fine. The real problem: **`board_loop()` is called after `delay(10)` every loop iteration**, which means it can fire a `showLastPhoto()` → `board_draw_jpeg()` → `gfx->fillScreen()` + many `draw16bitRGBBitmap` calls **mid-way through a `photoTask` render**. The `photoTask` itself takes `drawMutex` and calls `board_draw_jpeg()`, but `board_loop()` enters `showCurrentPhoto()`/`showLastPhoto()` via a separate path that also takes the mutex — so they'll block each other properly... **except** there's a window where `photoTask` just released the mutex after `board_draw_jpeg()` returns, but `board_loop()` immediately re-enters and calls `showCurrentPhoto()` again, triggering a second full re-render that stomps the panel mid-scan.

## 2. `fillScreen(0x0000)` Is Called BEFORE Every JPEG Draw — This Is The Line Shift

In `board_config.h`:

```cpp
gfx->fillScreen(0x0000);           // ← blacks out the whole screen
TJpgDec.drawJpg(x, y, jpg, len);   // ← then draws tile-by-tile via callback
```

The `fillScreen` writes ~384,000 pixels to the DMA FIFO. The panel is simultaneously scanning that buffer out at 16 MHz pixel clock. There is **no vsync/frame-sync wait** before `fillScreen` — so `fillScreen` can begin mid-scan, producing the black "gap" or shifted line you see on some refreshes but not others. Whether it glitches depends on timing randomness in `loop()` latency.

## 3. `delay(10)` in `loop()` is Unguarded During Draw

```cpp
void loop() {
  ...
  board_loop();
  delay(10);   // ← blocks the whole task for 10ms
}
```

`delay()` on ESP32 yields to FreeRTOS, which may resume `photoTask` mid-loop iteration. When `photoTask` finishes `board_draw_jpeg()` and releases `drawMutex`, `loop()` wakes up, hits `board_loop()`, and if the touch state says "not pressed and showingLast" it immediately calls `showCurrentPhoto()` → another full JPEG render → another `fillScreen` → another mid-scan corruption.

## The Fixes

**Fix 1 — Add vsync-equivalent guard before `fillScreen`:**

In `board_config.h`, replace:

```cpp
gfx->fillScreen(0x0000);
TJpgDec.drawJpg(x, y, jpg, len);
```

with:

```cpp
// Wait for the panel to be in the back-porch / blanking period.
// Arduino_GFX exposes lcd_cam_lcd_dma_done or you can just add a small
// fixed delay timed to be past the active scan region (~1-2ms is enough at 16MHz pclk)
delayMicroseconds(1500);  // ~1.5ms — enough to clear active scan at 800x480@16MHz
gfx->fillScreen(0x0000);
TJpgDec.drawJpg(x, y, jpg, len);
```

**Even better** — skip the `fillScreen` entirely for full-screen images. Since the JPEG covers the full display (it's centered and scaled to fill), the decoder will overwrite every pixel anyway. Remove the `fillScreen` call and the tearing immediately goes away for full-frame images.

**Fix 2 — Add a `draw_active` flag to suppress `board_loop()` redraws during JPEG decode:**

In `board_config.h`, add:

```cpp
static volatile bool boardDrawActive = false;
```

In `board_draw_jpeg()`, wrap the draw:

```cpp
boardDrawActive = true;
TJpgDec.drawJpg(x, y, jpg, len);
boardDrawActive = false;
```

In `board_loop()`:

```cpp
void board_loop() {
  if (boardDrawActive) return;  // ← add this guard
  ts.read();
  ...
}
```

**Fix 3 — The `spawnPhotoTask` still has no core pin:**

In the `.ino`, `xTaskCreate` for `photoTask` still has no core pin . Since you set "Arduino Runs On" = Core 1 in IDE, Arduino's loop is on Core 1 — but `xTaskCreate` (unpinned) can place `photoTask` on either core. Pin it explicitly:

```cpp
xTaskCreatePinnedToCore(
  [](void* param) {
    downloadAndShowPhoto();
    portENTER_CRITICAL(&logMux);
    photoTaskRunning = false;
    portEXIT_CRITICAL(&logMux);
    vTaskDelete(NULL);
  },
  "photoTask", 16384, NULL, 1, NULL, 1  // ← pin to Core 1
);
```

Same fix needed for `mqttReconnectTask` and `ota_check` — all should be `xTaskCreatePinnedToCore(..., 1)`.

## Summary Table

| Issue | Root Cause | Fix |
| :-- | :-- | :-- |
| Lines / shift mid-refresh | `fillScreen` fires mid-panel-scan with no vsync | Remove `fillScreen` before `drawJpg`, or add `delayMicroseconds(1500)` |
| Intermittent glitch on `board_loop` touch path | Touch handler re-triggers full JPEG render during decode | Add `boardDrawActive` flag guard in `board_loop()` |
| `photoTask` can land on Core 0 | `xTaskCreate` without core pin | Change to `xTaskCreatePinnedToCore(..., 1)` for all tasks |

The **`fillScreen` before `drawJpg` with no blanking sync** is the most likely cause of your "sometimes good, sometimes shifted" pattern — it's a pure race with the panel's DMA scan timing. Removing it is the quickest single fix to try.

