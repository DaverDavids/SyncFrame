
## The Fix

The core problem is that `mqttNetSecure` (a global `WiFiClientSecure`) is accessed unsafely from both `loopTask` and `mqttReconnectTask`. You need a mutex guarding all MQTT socket operations.

### Fix 1 — Add an MQTT mutex

Near your other globals, add:

```cpp
static SemaphoreHandle_t mqttMutex = nullptr;
```

In `setup()`, after `drawMutex` is created:

```cpp
mqttMutex = xSemaphoreCreateMutex();
```


### Fix 2 — Guard `mqttMaybeReconnect()` in `loop()`

```cpp
static void mqttMaybeReconnect() {
  // Take mutex before touching mqtt.connected() — it calls into NetworkClientSecure
  if (xSemaphoreTake(mqttMutex, 0) != pdTRUE) return; // skip this tick if busy
  bool connected = mqtt.connected();
  xSemaphoreGive(mqttMutex);

  if (connected) { mqttConnected = true; return; }
  mqttConnected = false;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttemptMs < 15000) return;
  if (mqttTaskRunning) return;
  if (cfg.mqttHost.length() == 0) return;

  lastMqttAttemptMs = millis();
  mqttTaskRunning   = true;
  xTaskCreatePinnedToCore(mqttReconnectTask, "mqttRecon", 16384, nullptr, 1, nullptr, APP_CORE);
}
```

Also wrap `mqtt.loop()` in `loop()`:

```cpp
if (mqtt.connected()) {
  if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    mqtt.loop();
    xSemaphoreGive(mqttMutex);
  }
}
```


### Fix 3 — Hold the mutex inside `mqttReconnectTask()`

```cpp
static void mqttReconnectTask(void* pv) {
  // ... setup code (snapshots of cfg) ...

  xSemaphoreTake(mqttMutex, portMAX_DELAY);  // take before touching socket

  if (useTLS) {
    mqttNetSecure.setInsecure();
    mqttNetSecure.setTimeout(5);
    mqtt.setClient(mqttNetSecure);
  } else {
    mqttNetPlain.setTimeout(5);
    mqtt.setClient(mqttNetPlain);
  }
  mqtt.setServer(host, cfg.mqttPort);
  mqtt.setSocketTimeout(5);
  mqtt.setCallback(mqttCallback);

  bool ok = (cfg.mqttUser.length() > 0)
    ? mqtt.connect(HOSTNAME, cfg.mqttUser.c_str(), cfg.mqttPass.c_str())
    : mqtt.connect(HOSTNAME);

  xSemaphoreGive(mqttMutex);  // release after connect() completes

  if (ok) {
    // subscribe after giving mutex back (subscribe also uses the socket)
    if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
      bool subOk = mqtt.subscribe(topic);
      xSemaphoreGive(mqttMutex);
      logEvent("MQTT", "connected sub=%s topic=%s", subOk ? "ok" : "fail", topic);
      mqttConnected = true;
    }
  } else {
    logEvent("MQTT", "connect failed rc=%d", mqtt.state());
  }

  mqttTaskRunning = false;
  vTaskDelete(NULL);
}
```


### Fix 4 — Explicit `stop()` before reconnect

Inside `mqttReconnectTask`, before the `mqtt.connect()` call, explicitly stop any prior connection while you hold the mutex:

```cpp
mqtt.disconnect();       // sends MQTT DISCONNECT if connected
mqttNetSecure.stop();    // tears down TLS cleanly under mutex protection
delay(50);
```

This ensures only one context ever calls `mbedtls_ssl_free` .

***

### Secondary Issue: `PHOTO SPIRAM alloc failed, free=0`

You'll also see `free=0` on SPIRAM after repeated reconnect cycles. Each failed TLS connect allocates mbedTLS certificate chain buffers (~50-100KB in SPIRAM) and may not free them cleanly if the connection aborts mid-handshake . The mutex fix prevents the double-free corruption that causes the crash itself, but you may also want to call `mqttNetSecure.stop()` explicitly after each failed `mqtt.connect()` within the mutex to reclaim that SPIRAM.

<div align="center">⁂</div>

[^1]: coredump_report.txt

