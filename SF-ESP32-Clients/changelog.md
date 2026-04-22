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

