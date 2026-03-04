#pragma once
#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>SyncFrame</title>
  <style>
    body{font-family:system-ui,Arial;margin:16px;background:#0b0f14;color:#e8eef5;}
    .row{display:flex;gap:16px;flex-wrap:wrap;align-items:flex-start;}
    .card{background:#121a24;border:1px solid #223146;border-radius:12px;padding:14px;max-width:980px}
    .title{font-size:20px;margin:0 0 8px 0;}
    .status span{display:inline-block;margin-right:14px}
    .ok{color:#37d67a;font-weight:600}
    .bad{color:#ff5d5d;font-weight:600}
    .imgbox{
      width:min(92vw,900px);
      aspect-ratio: 800 / 480;
      background:#000;
      border:1px solid #2a3c55;
      border-radius:12px;
      overflow:hidden;
      display:flex;
      align-items:center;
      justify-content:center;
    }
    .imgbox img{width:100%;height:100%;object-fit:contain}
    button{background:#1f7ae0;color:#fff;border:0;border-radius:10px;padding:10px 14px;font-weight:600;cursor:pointer}
    button:active{transform:scale(.99)}
    a{color:#8ecbff}
    .small{opacity:.85;font-size:13px}
  </style>
</head>
<body>
  <div class="card">
    <h1 class="title">SyncFrame</h1>

    <div class="status small">
      <span id="s_wifi">WiFi: …</span>
      <span id="s_mdns">mDNS: …</span>
      <span id="s_mqtt">MQTT: …</span>
      <span id="s_dl">Photo: …</span>
    </div>

    <div style="margin:10px 0" class="row">
      <button onclick="refreshNow()">Refresh now</button>
      <div class="small">Tip: hold the touchscreen to show the last photo.</div>
      <div style="flex:1"></div>
      <a class="small" href="/config">Config / Debug</a>
    </div>

    <div class="imgbox">
      <img id="img" src="/img/current" alt="current photo"/>
    </div>

    <div class="small" style="margin-top:10px" id="meta"></div>
  </div>

<script>
function setStatus(id, label, ok) {
  const el = document.getElementById(id);
  el.className = ok ? "ok" : "bad";
  el.textContent = label + ": " + (ok ? "✓" : "✗");
}
let lastImgStamp = 0;
async function poll() {
  try {
    const r = await fetch("/api/status", {cache:"no-store"});
    const s = await r.json();

    setStatus("s_wifi","WiFi", !!s.wifi);
    setStatus("s_mdns","mDNS", !!s.mdns);
    setStatus("s_mqtt","MQTT", !!s.mqtt);
    setStatus("s_dl","Photo", !!s.lastDownloadOk);

    const meta = document.getElementById("meta");
    meta.textContent = (s.ip ? ("IP: " + s.ip + "  |  ") : "") +
      (s.lastDownloadOk ? "" : ("Last error: " + (s.lastDownloadErr || "unknown")));

    // Bust cache so you always see the latest image
    if (s.lastDownloadMs && s.lastDownloadMs !== lastImgStamp) {
    lastImgStamp = s.lastDownloadMs;
    document.getElementById("img").src = "/img/current?ts=" + lastImgStamp;
  }

  } catch(e) {
    setStatus("s_wifi","WiFi", false);
    setStatus("s_mdns","mDNS", false);
    setStatus("s_mqtt","MQTT", false);
    setStatus("s_dl","Photo", false);
  }
}

async function refreshNow() {
  await fetch("/api/refresh", {method:"POST"});
  await poll();
}

poll();
setInterval(poll, 2000);
</script>
</body>
</html>
)HTML";

static const char CONFIG_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>SyncFrame Config</title>
  <style>
    body{font-family:system-ui,Arial;margin:16px;background:#0b0f14;color:#e8eef5;}
    .card{background:#121a24;border:1px solid #223146;border-radius:12px;padding:14px;max-width:980px}
    label{display:block;margin:10px 0 4px 0;opacity:.9}
    input[type="text"],input[type="password"],input[type="number"]{width:100%;padding:10px;border-radius:10px;border:1px solid #2a3c55;background:#0b111a;color:#e8eef5;box-sizing:border-box}
    .row{display:flex;gap:10px;flex-wrap:wrap}
    .row > div{flex:1;min-width:240px}
    button{background:#37d67a;color:#05110a;border:0;border-radius:10px;padding:10px 14px;font-weight:700;cursor:pointer;margin-top:12px}
    a{color:#8ecbff}
    .small{opacity:.85;font-size:13px}
    .msg{background:#2a4a2e;border:1px solid:#37d67a;padding:10px;border-radius:8px;margin:10px 0}
  </style>
</head>
<body>
  <div class="card">
    <div class="row" style="align-items:center">
      <h1 style="margin:0;flex:1">Config / Debug</h1>
      <a href="/">Back</a>
    </div>
    
    <div id="saveMsg" class="msg" style="display:none">Settings saved!</div>

    <form id="configForm">
      <h3>Photo</h3>
      <label>Photo base URL (include trailing / if you want)</label>
      <input type="text" name="photoBaseUrl" id="photoBaseUrl" placeholder="https://192.168.1.10:9369/syncframe/"/>

      <label>Photo filename</label>
      <input type="text" name="photoFilename" id="photoFilename" placeholder="photo.800x480.jpg"/>

      <label>
        <input type="checkbox" name="httpsInsecure" id="httpsInsecure"/> 
        Allow insecure HTTPS (self-signed)
      </label>

      <div class="row">
        <div>
          <label>HTTP Basic auth user (optional)</label>
          <input type="text" name="httpUser" id="httpUser" placeholder="admin"/>
        </div>
        <div>
          <label>HTTP Basic auth pass (leave blank to keep)</label>
          <input type="password" name="httpPass" id="httpPass" placeholder=""/>
        </div>
      </div>

      <h3>MQTT</h3>
      <div class="row">
        <div>
          <label>Host</label>
          <input type="text" name="mqttHost" id="mqttHost" placeholder="192.168.1.10"/>
        </div>
        <div>
          <label>Port</label>
          <input type="number" name="mqttPort" id="mqttPort" placeholder="9368"/>
        </div>
      </div>

      <label>Topic</label>
      <input type="text" name="mqttTopic" id="mqttTopic" placeholder="photos"/>

      <div class="row">
        <div>
          <label>User (optional)</label>
          <input type="text" name="mqttUser" id="mqttUser" placeholder="mqttuser"/>
        </div>
        <div>
          <label>Pass (leave blank to keep)</label>
          <input type="password" name="mqttPass" id="mqttPass" placeholder=""/>
        </div>
      </div>

      <label>
        <input type="checkbox" name="mqttUseTLS" id="mqttUseTLS"/> 
        Use TLS
      </label>
      
      <label>
        <input type="checkbox" name="mqttTlsInsecure" id="mqttTlsInsecure"/> 
        Allow insecure TLS (self-signed)
      </label>

      <button type="submit">Save</button>
      <div class="small" style="margin-top:10px">
        Wi-Fi credentials are configured via captive portal if the device can't connect.
      </div>
    </form>
  </div>

<script>
let currentConfig = {};

async function loadCfg() {
  try {
    const r = await fetch("/api/config", {cache:"no-store"});
    const c = await r.json();
    currentConfig = c;
    
    photoBaseUrl.value = c.photoBaseUrl || "";
    photoFilename.value = c.photoFilename || "";
    httpsInsecure.checked = !!c.httpsInsecure;
    httpUser.value = c.httpUser || "";
    httpPass.value = "";  // Never populate password
    
    mqttHost.value = c.mqttHost || "";
    mqttPort.value = c.mqttPort || "";
    mqttTopic.value = c.mqttTopic || "";
    mqttUser.value = c.mqttUser || "";
    mqttPass.value = "";  // Never populate password
    mqttUseTLS.checked = !!c.mqttUseTLS;
    mqttTlsInsecure.checked = !!c.mqttTlsInsecure;
  } catch(e) {
    console.error("Failed to load config:", e);
  }
}

configForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  
  // Build form data - only include fields that have values or checkboxes
  const formData = new URLSearchParams();
  
  // Always send text fields (even if empty, to allow clearing)
  formData.append('photoBaseUrl', photoBaseUrl.value);
  formData.append('photoFilename', photoFilename.value);
  formData.append('httpUser', httpUser.value);
  formData.append('mqttHost', mqttHost.value);
  formData.append('mqttPort', mqttPort.value);
  formData.append('mqttTopic', mqttTopic.value);
  formData.append('mqttUser', mqttUser.value);
  
  // Only send password if filled
  if (httpPass.value.length > 0) {
    formData.append('httpPass', httpPass.value);
  }
  if (mqttPass.value.length > 0) {
    formData.append('mqttPass', mqttPass.value);
  }
  
  // Send checkboxes only if checked
  if (httpsInsecure.checked) formData.append('httpsInsecure', '1');
  if (mqttUseTLS.checked) formData.append('mqttUseTLS', '1');
  if (mqttTlsInsecure.checked) formData.append('mqttTlsInsecure', '1');
  
  try {
    const resp = await fetch('/api/config', {
      method: 'POST',
      body: formData
    });
    
    if (resp.ok) {
      saveMsg.style.display = 'block';
      setTimeout(() => saveMsg.style.display = 'none', 3000);
      // Reload config after 500ms
      setTimeout(loadCfg, 500);
    }
  } catch(e) {
    alert('Save failed: ' + e);
  }
});

// Initial load
loadCfg();
</script>
</body>
</html>
)HTML";