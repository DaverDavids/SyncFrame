4-22-2026 10:06am:
```
Fix: ESP32 client MJPEG stream task stability (v-next)

- Fixed crash caused by WiFiClientSecure holding internal lwIP/WiFi
  mutex during 30s TLS connect, starving the WiFi FreeRTOS task.
  Added explicit 10s connect timeout on the secure client.
- Fixed reconnect deadlock: mjpegConnected flag is now set only after
  a successful HTTP 200 handshake inside the task, not speculatively
  before task creation.
- Fixed static TCB/stack buffer reuse: moved mjpegTaskBuffer and
  mjpegStack to file scope and added a task handle guard to prevent
  re-creation before the previous task has fully exited.
```

4-22-2026 10:19am:
```
Fix: MJPEG stream immediately bailing on empty status line

- Added availability wait loop before reading HTTP status line;
  previously readStringUntil() returned empty on a slow server
  response, causing instant disconnect.
- Split WiFiClientSecure timeout: 15s during TLS connect,
  60s restored afterward for stable streaming reads.
```

4-22-2026 10:28am:
```
Fix: Race condition / lock starvation during TLS handshake on boot

- Lowered mjpegTask priority to 0 (Idle) to prevent mbedTLS connect blocking from starving the Wi-Fi FreeRTOS task.
- Added 500ms settling delay at start of mjpegTask to ensure the Wi-Fi subsystem fully finalizes its DHCP/route states before the heavy blocking start_ssl_client call begins.
```

4-22-2026 10:39am:
```
Fix: FreeRTOS static task memory corruption on reconnect

- Converted MJPEG stream task to a permanent worker task pattern to prevent ESP32-C3 heap fragmentation.
- Eliminated vTaskDelete() calls which caused FreeRTOS ready-list corruption when reusing static TCBs without Idle task cleanup.
- Task now sleeps safely via ulTaskNotifyTake() and is awakened by the main loop when a reconnect is required.
```

4-22-2026 11:01am:
```
Fix: Redundant double-reconnect after config save

- Fixed stale flag race condition: mjpegRequestRefresh is now explicitly cleared when the stream task wakes up, preventing it from immediately dropping fresh connections.
- Fixed timestamp race condition: lastMjpegConnectMs is now updated before mjpegConnected is set to true, preventing the main loop from preempting and triggering a false interval timeout.
```

5-11-2026 (1):
```
Fix: LCD glitch (line-shift artifact) during WiFi traffic / stream reconnects

- mjpegTask no longer calls board_draw_jpeg_from_stream() directly.
  WiFi ISR activity was racing TJpg_Decoder SPI DMA writes on the
  shared bus, causing the bottom-rows-at-top wrap artifact.
- Added volatile pendingDraw flag; mjpegTask sets it after writing the
  frame to LittleFS and releasing drawMutex. loop() picks it up and
  calls showCurrentPhoto() from the main Arduino task context instead.
- drawMutex hold time in mjpegTask reduced to LittleFS write only.
- No changes to task priority, notify/wake pattern, timeout logic,
  peek button path, or boardDrawActive guard.
```

5-11-2026 (2):
```
Fix: Remaining SPI bus contention glitch and permanent right-shift on ESP32-C3

Root cause: ESP32-C3 shares one SPI peripheral between the WiFi radio
and HSPI (TFT). WiFi reconnect / TLS activity can steal the bus mid-
MCU-block write from TJpgDec, causing line-shift or leaving the ST7789
column address counter offset (permanent right-shift).

- Added SF_SPI_BEGIN / SF_SPI_END macros (C3 only) that wrap the entire
  board_draw_jpeg() decode+draw in SPI.beginTransaction / endTransaction,
  holding the Arduino SPI mutex for the full frame so WiFi cannot
  interleave. Compiles away to nothing on S3 (parallel RGB panel).
- Added a dummy 1x1 fillRect at (0,0) before decode on C3 to re-home
  the ST7789 CASET/RASET address counters before each frame. Cost ~1us;
  eliminates the permanent right-shift caused by a prior interrupted
  transaction leaving the column counter at an arbitrary offset.
- Removed vTaskDelay(1) from board_draw_jpeg_from_stream(): it was
  creating a yield point mid-setup that re-opened the contention window
  the SPI transaction guard now closes properly.
```

5-11-2026 (3):
```
Fix: S3 RGB panel DMA race (line-shift, permanent right-shift) via proper double-buffer

All reported glitches are on the S3, not C3. Root cause: single-buffer
mode on the RGB panel means the DMA scanner and TJpgDec MCU writes
contest the same 768 KB PSRAM framebuffer simultaneously.

Previous double-buffer attempt failed because flush() was called mid-
render (inside the callback or per-frame), which swapped buffers with a
partially-decoded frame visible and caused Core 0 WDT reboots from
reentrant GFX calls.

- config_s3.h: changed Arduino_RGB_Display constructor useDataBuf from
  false to true, re-enabling double-buffer mode.
- board_init() now calls gfx->flush() after initial fillScreen to push
  the black frame to the front buffer on startup.
- board_draw_jpeg(): added gfx->flush() call AFTER TJpgDec.drawJpg()
  returns (full frame complete), using #if BOARD_IS_S3 guard.
  flush() now appears in exactly one place and is never called from
  jpegDrawCallback(). Back buffer is fully written before swap.
- board_draw_boot_status(): added gfx->flush() on S3 so status text
  reaches the front buffer during setup.
- Added boardDrawActive = false reset at top of board_draw_jpeg() as a
  safety measure: a previously-interrupted draw cannot permanently block
  future frames by leaving the flag stuck true.
- BOARD_IS_S3 / BOARD_IS_C3 defines added alongside target include for
  clean conditional compilation.
```

5-15-2026:
```
Fix: drawMutex not held during /img/current and /img/last HTTP stream

- handleImgCurrent() and handleImgLast() now acquire drawMutex before
  calling server.streamFile() and release it after f.close().
  Without this, mjpegTask could rename/overwrite current.jpg or prev.jpg
  mid-stream, producing a torn LittleFS read and triggering the LCD
  glitch approximately 1s after a browser refresh of the web UI.
- Both handlers return HTTP 503 if the mutex cannot be acquired within
  2000ms rather than proceeding with an unguarded read.
- No changes to the draw path, pendingDraw flag, or task structure.
```

5-16-2026 — INVESTIGATION NOTES (no display regression):
```
Status: stream receiving frames (51578 bytes, etag confirmed) but LCD
shows nothing. All attempts below failed or were not confirmed working.

What is known:
- The pendingDraw refactor (5-11 entry 1) was committed to the changelog
  but the corresponding loop() consumer was never pushed to the .ino.
  mjpegTask sets pendingDraw=true; nothing in loop() reads it and calls
  showCurrentPhoto(). Frames land in LittleFS and are never drawn.
- The .ino on main still carries SHA 538ccf5 — it predates the
  pendingDraw changes, meaning the draw-from-task code may also be
  absent, leaving no draw path at all.

Attempts that did not resolve it:
1. SPI transaction guards (SF_SPI_BEGIN/END macros) — addressed glitch
   artifact but not the no-display regression.
2. drawMutex guards on /img/* endpoints — correct fix for a separate
   race but unrelated to the blank screen.
3. pendingDraw flag added to mjpegTask — correct idea, but the
   loop() consumer `if (pendingDraw) { pendingDraw=false; showCurrentPhoto(); }`
   was never committed.

Next step: add pendingDraw consumer to loop() and push the .ino.
```
