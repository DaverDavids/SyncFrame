# SyncFrame Changelog

---

## ESP32-S3 JC8048 LCD Glitch — Investigation Log

**Board:** ESP32-S3 JC8048 · RGB panel · PSRAM framebuffer · DMA at ~16 MHz pixel clock  
**Symptom:** Horizontal line-shift / wrap artifact, intermittent, long-running.

---

### Root Cause (confirmed)

The ESP32-S3 RGB LCD DMA engine streams the framebuffer from PSRAM continuously.
Any operation that holds the AHB/PSRAM bus long enough starves the DMA mid-scanline,
causing the HSYNC counter to drift — producing the horizontal shift artifact.

Two confirmed stall sources were identified:

1. **LittleFS flash I/O** (`f.write()` / `f.read()` in `bgTask` or `loop()`) holds the
   bus 1–5 ms per sector operation.
2. **`WiFiClientSecure::connect()` TLS handshake** — mbedTLS allocates large temporaries
   in PSRAM and calls `esp_fill_random()` (hardware RNG via AHB bus), stalling the bus
   for 200 ms – 2 s per reconnect.

---

### Attempts That Did Not Resolve the Glitch

#### Attempt 1 — Move LittleFS writes out of `bgTask`, into `loop()` inter-frame gap
**Commit:** `9cc26e0`

Moved `f.write()` / `LittleFS.rename()` calls out of `mjpegTask` (Core 0) and into
`loop()` (Core 1) immediately after `board_draw_jpeg()` returns, so flash I/O only
happens when DMA is provably idle between frames. Added `pendingCommit` flag to
coordinate the handoff.

**Why it failed:** Eliminated the LittleFS stall source but the glitch persisted.
The TLS handshake stall (source 2 above) was not addressed; reconnect events
continued to corrupt the scanline counter.

---

#### Attempt 2 — `networkBusy` flag to pause draw loop during TLS handshake
**Commit:** `dc1110c`

Added `volatile bool networkBusy`. `mjpegTask` sets it `true` immediately before
`client->connect()` and clears it when `connect()` returns. `loop()` skips the
entire `pendingDraw` block while `networkBusy` is set, so Core 1 makes zero PSRAM
bus requests during the handshake window. Also gated `ArduinoOTA.handle()` behind
`!networkBusy`.

**Why it failed:** The flag correctly pauses new draw calls, but the RGB DMA engine
is *always running* — it continuously re-scans the current framebuffer from PSRAM
even when no new frame is being written. Pausing `loop()` does not stop DMA bus
traffic; the DMA engine itself still contends the PSRAM bus with the TLS stack,
just with existing framebuffer data instead of new data. The stall and drift can
still occur.

---

### Open / Next Steps

The fix must target the DMA engine itself, not the software draw loop:

- **Option A:** Suspend the RGB peripheral (`lcd_cam` peripheral clock gate or
  `esp_lcd_panel_disp_on_off()`) for the duration of the TLS handshake, then
  re-enable and re-trigger DMA. This fully releases the PSRAM bus.
- **Option B:** Pre-resolve DNS and pre-open the TCP socket during an idle window;
  move TLS negotiation to a period where DMA can be briefly gated.
- **Option C:** Store the framebuffer in internal SRAM (if size permits) so DMA
  never touches the PSRAM bus, eliminating the contention entirely.
