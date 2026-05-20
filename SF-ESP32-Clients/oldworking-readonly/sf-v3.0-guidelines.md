# SyncFrame Protocol Redesign — Implementation Spec

## Overview

Replace the current two-connection architecture (SSE trigger + separate HTTPS photo download + separate OTA polling) with a single persistent `multipart/x-mixed-replace` stream per client. The server pushes JPEG frames and OTA firmware over this one connection. Metadata flows from client to server via HTTP request headers on every (re)connect. This eliminates the dual-TLS memory exhaustion problem on ESP32-C3.

MQTT and `/events` SSE endpoint are retained unchanged for backward compatibility.

---

## Server Changes (`syncframe-server.py`)

### New: `/stream` endpoint

- Accepts `GET /stream`
- Reads these request headers from the client:
  - `X-SF-Hostname` — device hostname
  - `X-SF-MAC` — device MAC address
  - `X-SF-Uptime` — uptime in seconds
  - `X-SF-Compiled` — firmware compile ID (e.g. `20260412-133319`)
  - `X-SF-Photo-Hash` — CRC32 hex string of the JPEG currently displayed on the client; empty string if none
- Logs and stores the metadata in memory, keyed by MAC address (same structure as existing OTA clients JSON)
- Registers the client's response object in a module-level `connected_stream_clients` dict (keyed by MAC)
- Responds `200 multipart/x-mixed-replace; boundary=frame` with no `Content-Length` (streaming)
- **OTA check (before sending any frames):** compare `X-SF-Compiled` against the latest firmware available for that client in the existing OTA clients JSON. If an update is available, push one OTA frame (see OTA frame format below), then continue streaming normally. The client will flash and reboot; the connection will drop.
- **Photo check (after OTA check):** compare `X-SF-Photo-Hash` against the hash of the current photo. If they differ, immediately push the current JPEG as the first frame.
- **Keepalive loop:** every 60 seconds with no new photo, push an empty keepalive boundary (see format below) to keep the TCP connection alive through NAT/firewalls.
- On client disconnect: remove from `connected_stream_clients`, log disconnect.

### Multipart frame formats

**JPEG photo frame:**
```
--frame

Content-Type: image/jpeg

Content-Length: <n>



<n bytes of JPEG data>
```

**OTA firmware frame:**
```
--frame

Content-Type: application/octet-stream

X-SF-Frame-Type: ota

Content-Length: <n>



<n bytes of firmware binary>
```

**Keepalive frame (no body):**
```
--frame



```

### Modified: photo upload handler (existing)

- After generating all resolution thumbnails: compute CRC32 of the newly saved `photo.jpg` and store in a module-level variable `current_photo_hash`.
- Push the appropriate resolution JPEG frame to every client currently in `connected_stream_clients`. Each client's stored resolution (from `X-SF-Compiled` or a separate `X-SF-Resolution` header — see note below) determines which thumbnail file to push.
- Note: to push the correct resolution per client, the client should also send `X-SF-Resolution: WxH` (e.g. `280x240`) in its connect headers. Server stores this alongside other metadata and uses it when pushing frames.

### Modified: photo hash

- Compute CRC32 of `photo.jpg` at server startup (in case server restarts mid-session) and store in `current_photo_hash`.
- Recompute and update `current_photo_hash` on every new upload.

### Removed

- Separate `/ota` endpoint can be kept for backward compatibility but is no longer used by updated clients.
- No other removals on the server side.

---

## Client Changes (`SF-ESP32-Clients.ino`)

### New: `mjpegTask` (replaces `sseTask`)

A FreeRTOS task that manages the single persistent stream connection.

**On start:**
- Construct `WiFiClientSecure` as a **module-level persistent pointer** (allocated once, never deleted between reconnects — only reconnected). On first call, allocate with `new`. On reconnect, reuse the existing object and call `stop()` then re-`connect()`.
- Build URL: `<cfg.photoBaseUrl>stream` (append `stream` to base URL, ensuring single `/` separator)
- Send `GET /syncframe/stream HTTP/1.1` with headers:
  - `X-SF-Hostname: <HOSTNAME>`
  - `X-SF-MAC: <MAC_STR>` (no colons)
  - `X-SF-Uptime: <seconds since boot>`
  - `X-SF-Compiled: <compileIdStr>`
  - `X-SF-Photo-Hash: <currentPhotoHash>` (CRC32 hex of displayed JPEG, or empty string)
  - `X-SF-Resolution: <SCREEN_W>x<SCREEN_H>`
  - `Accept: multipart/x-mixed-replace`
  - `Authorization: Basic <base64(user:pass)>` if credentials configured
  - `Connection: keep-alive`
- Read HTTP response status line; if not `200`, log error, set `mjpegConnected = false`, exit task.
- Read and discard response headers until blank line.

**Stream read loop:**
- Read lines until `--frame` boundary is found.
- Read subsequent header lines until blank line, extracting:
  - `Content-Type`
  - `Content-Length`
  - `X-SF-Frame-Type`
- If `Content-Length` is missing or 0: this is a keepalive frame — reset the 90s idle timer, continue loop.
- If `Content-Type: image/jpeg`:
  - On no-PSRAM builds: if `Content-Length > C3_MAX_PREALLOC` (20480), log error and skip frame (read and discard the bytes). Otherwise `malloc(Content-Length)`.
  - On PSRAM builds: `heap_caps_malloc(Content-Length, MALLOC_CAP_SPIRAM)`.
  - Read exactly `Content-Length` bytes into buffer.
  - Compute CRC32 of received bytes; store as `currentPhotoHash`.
  - Display via `board_draw_jpeg` (take `drawMutex`).
  - Free buffer.
- If `Content-Type: application/octet-stream` and `X-SF-Frame-Type: ota`:
  - Set `otaInProgress = true`.
  - Call `Update.begin(Content-Length)`.
  - Read exactly `Content-Length` bytes from stream directly into `Update.writeStream()`.
  - Call `Update.end()`. If finished: `ESP.restart()`. If not: log error, set `otaInProgress = false`, continue.
- If no data received for 90 seconds: treat as dead connection, exit loop.
- On any read error or stream end: exit loop.

**On task exit:**
- Set `mjpegConnected = false`.
- Call `vTaskDelete(NULL)`.

### New: `mjpegMaybeReconnect` (replaces `sseMaybeReconnect`)

Called from `loop()`. Logic:

- If `mjpegConnected`: check if 10 minutes have elapsed since `lastMjpegConnectMs`. If yes: set `mjpegForceReconnect = true`.
- If `mjpegForceReconnect`: close stream (`streamClient->stop()`), wait 200ms, clear flag, set `mjpegConnected = false`, reset `lastMjpegAttemptMs = 0`.
- If not connected and WiFi up and `millis() - lastMjpegAttemptMs >= 15000`: set `lastMjpegConnectMs = millis()`, `lastMjpegAttemptMs = millis()`, `mjpegConnected = true`, spawn `mjpegTask`.

### Modified: web UI refresh button handler (`handleActionRefresh`)

- Instead of setting `refreshPending = true`: set `mjpegForceReconnect = true`.
- Remove the `refreshPending` check from `loop()`.

### New globals

```
static WiFiClientSecure* streamClient = nullptr;  // persistent, allocated once
static bool    mjpegConnected       = false;
static unsigned long lastMjpegConnectMs  = 0;     // time of last successful connect
static unsigned long lastMjpegAttemptMs  = 0;     // for 15s reconnect cooldown
static bool    mjpegForceReconnect  = false;
static char    currentPhotoHash[12] = "";          // CRC32 hex of displayed JPEG
```

### Removed

- `sseTask`, `sseMaybeReconnect`
- `sseConnected`, `lastSseAttemptMs`, `lastSseMsgMs`, `sseClient` globals
- `spawnPhotoTask` (photo display moves inside `mjpegTask`)
- `refreshPending` flag and all references
- `otaUpdateTask`, `startOtaTask`, `otaTaskHandle` — OTA is now delivered by the server over the stream
- `httpDownloadToBuffer`, `downloadAndShowPhoto` — no longer needed

### Retained unchanged

- `board_draw_jpeg`, `drawMutex`, display logic
- WiFi connection management, portal, `ensureWifi`
- Web server and all UI handlers (except `handleActionRefresh` above)
- ArduinoOTA (Arduino IDE OTA unrelated to SyncFrame firmware delivery)
- All logging infrastructure

---

## Status JSON (`/api/status`) — Updated Fields

Replace SSE fields with stream fields:

- Remove: `sse`, `lastSseMsgMs`
- Add: `mjpeg` (bool, stream connected), `lastMjpegConnectMs` (ms timestamp), `photoHash` (current CRC32 hex)
