
## Core Assignment: What You Should Do

| Task | Recommended Core | Why |
| :-- | :-- | :-- |
| `loop()` / Arduino main | **Core 1** (Arduino default on S3) | Keeps GFX DMA interrupt on Core 0 uncontested |
| `photoTask` | **Core 1** (pin it) | Does GFX draws, must stay same core as loop |
| `mqttReconnectTask` | **Core 1** | TLS is CPU-heavy; fine on Core 1 |
| `ota_check` | **Core 1** | Heavy HTTP; keep off Core 0 |
| `board_loop()` / touch | doesn't matter | lightweight |

On ESP32-S3, **Arduino `loop()` runs on Core 1 by default**. The RGB DMA peripheral interrupt fires on **Core 0**. So the goal is: **keep all your app tasks on Core 1** and **leave Core 0 alone for the DMA/WiFi driver**.

## Specific Fixes Needed

**1. Re-pin all tasks to Core 1:**

In `spawnPhotoTask()`, change:

```cpp
xTaskCreate([](void*){...}, "photoTask", 16384, NULL, 1, NULL);
```

to:

```cpp
xTaskCreatePinnedToCore([](void*){...}, "photoTask", 16384, NULL, 1, NULL, 1);
```

Do the same for `mqttReconnectTask` and `otaUpdateTask` — all pinned to **core 1**.

**2. The `board_loop()` touch → draw path is unguarded:**

In `board_loop()`, `showLastPhoto()` / `showCurrentPhoto()` are called directly from `loop()`, which is correct since they take the `drawMutex`. But if `photoTask` is on Core 0 and also holding `drawMutex`, you get a priority inversion stall right in the middle of a DMA cycle. Pinning to Core 1 fixes this.

**3. The `delay(10)` at the bottom of `loop()` may be hurting you:**

The RGB bounce buffer relies on frequent DMA refills. A `delay(10)` on the same core as the DMA handler is fine, but if Core 0 is getting loaded by unpinned tasks, those 10ms windows compound. Consider reducing to `delay(1)` or using `vTaskDelay(pdMS_TO_TICKS(1))`.

**4. Arduino IDE "Events Run On" / "Arduino Runs On" settings:**

- **"Arduino Runs On"** → set to **Core 1**
- **"Events Run On"** → set to **Core 1** as well (both on Core 1)

This ensures Arduino's event system and `loop()` stay on Core 1, leaving Core 0 exclusively for the WiFi/BT stack and the RGB DMA peripheral interrupts. Having them on different cores (e.g. Events on Core 0) is what can destabilize the DMA timing because event callbacks may call GFX functions.

## TL;DR Summary

The March 28 refactor removed all `xTaskCreatePinnedToCore` calls in favor of unpinned `xTaskCreate`. This lets MQTT TLS, HTTP photo download, and OTA tasks land on Core 0, which starves the RGB panel's DMA bounce buffer refill interrupt — causing the line shift/shimmer you're seeing. Fix: pin all your app tasks to Core 1 explicitly, set both Arduino settings to Core 1, and leave Core 0 for the hardware drivers.

