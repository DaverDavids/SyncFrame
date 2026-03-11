#pragma once
#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
  <title>SyncFrame</title>
  <style>
    *,*::before,*::after{box-sizing:border-box}
    body{
      margin:0;
      padding:20px;
      min-height:100vh;
      background:
        radial-gradient(circle at 20% 20%, rgba(255,0,150,0.8) 0%, transparent 50%),
        radial-gradient(circle at 80% 80%, rgba(0,255,100,0.7) 0%, transparent 50%),
        radial-gradient(circle at 80% 20%, rgba(0,200,255,0.8) 0%, transparent 50%),
        radial-gradient(circle at 20% 80%, rgba(255,100,0,0.7) 0%, transparent 50%),
        linear-gradient(135deg,#1a0033 0%,#003333 100%);
      background-attachment:fixed;
      background-size:150% 150%;
      font-family:Arial,sans-serif;
      text-align:center;
      color:#ffffff;
      animation:gradientShift 15s ease infinite;
      overflow-x:hidden;
    }
    @keyframes gradientShift{
      0%{background-position:0% 50%}
      50%{background-position:100% 50%}
      100%{background-position:0% 50%}
    }
    h1{
      color:#ffffff;
      text-shadow:0 2px 10px rgba(0,0,0,0.7);
      font-size:2.5em;
      margin-bottom:10px;
    }
    .status-bar{
      display:flex;
      flex-wrap:wrap;
      justify-content:center;
      gap:14px;
      margin-bottom:18px;
      font-size:13px;
    }
    .status-bar span{
      background:rgba(0,0,0,0.3);
      backdrop-filter:blur(6px);
      border:1px solid rgba(255,255,255,0.15);
      border-radius:20px;
      padding:4px 14px;
      font-weight:600;
    }
    .ok{color:#37d67a}
    .bad{color:#ff5d5d}
    .image-container{
      display:inline-block;
      width:min(92vw,860px);
      aspect-ratio:800/480;
      background:#000;
      margin-top:20px;
      border-radius:4px;
      border:15px solid;
      border-image:linear-gradient(145deg,#8b6f47,#d4a574,#c4956a,#8b6f47,#6a5537) 1;
      box-shadow:
        inset 0 0 30px rgba(0,0,0,0.8),
        0 10px 40px rgba(0,0,0,0.8),
        0 0 0 3px #4a3a2a,
        0 0 0 4px #2a1a0a;
      position:relative;
      overflow:hidden;
    }
    .image-container img{
      width:100%;
      height:100%;
      object-fit:contain;
      display:block;
    }
    .controls{
      margin:22px 0 10px;
    }
    button{
      background:rgba(255,255,255,0.15);
      color:#ffffff;
      border:1px solid rgba(255,255,255,0.3);
      padding:10px 24px;
      border-radius:6px;
      cursor:pointer;
      backdrop-filter:blur(10px);
      font-size:1em;
      font-weight:600;
      transition:background 0.3s ease;
    }
    button:hover{background:rgba(255,255,255,0.25)}
    button:active{transform:scale(0.98)}
    footer{
      margin-top:24px;
      padding:8px;
      color:rgba(255,255,255,0.55);
      font-size:0.82em;
      display:flex;
      flex-wrap:wrap;
      justify-content:center;
      gap:18px;
      align-items:center;
    }
    footer a{
      color:rgba(255,255,255,0.4);
      text-decoration:none;
      border-bottom:1px dotted rgba(255,255,255,0.25);
      transition:color 0.2s;
    }
    footer a:hover{color:rgba(255,255,255,0.75)}
    @media(max-width:600px){
      h1{font-size:1.8em}
      footer{flex-direction:column;gap:6px}
    }
  </style>
</head>
<body>
  <h1>SyncFrame</h1>

  <div class="status-bar">
    <span id="s_wifi">WiFi: &hellip;</span>
    <span id="s_mdns">mDNS: &hellip;</span>
    <span id="s_mqtt">MQTT: &hellip;</span>
    <span id="s_dl">Photo: &hellip;</span>
  </div>

  <div class="controls">
    <button onclick="refreshNow()">&#8635; Refresh</button>
  </div>

  <div class="image-container">
    <img id="img" src="/img/current" alt="current photo"/>
  </div>

  <footer>
    <span id="f_ip">IP: &hellip;</span>
    <span id="f_mac">MAC: &hellip;</span>
    <span id="f_host">Host: &hellip;</span>
    <a href="/config">Config</a>
  </footer>

<script>
function setStatus(id, label, ok) {
  const el = document.getElementById(id);
  el.innerHTML = label + ": " + (ok
    ? "<span class='ok'>&#10003;</span>"
    : "<span class='bad'>&#10007;</span>");
}
let lastImgStamp = 0;
async function poll() {
  try {
    const r = await fetch("/api/status", {cache:"no-store"});
    const s = await r.json();
    setStatus("s_wifi", "WiFi",  !!s.wifi);
    setStatus("s_mdns", "mDNS",  !!s.mdns);
    setStatus("s_mqtt", "MQTT",  !!s.mqtt);
    setStatus("s_dl",   "Photo", !!s.lastDownloadOk);
    if (s.ip)       document.getElementById("f_ip").textContent   = "IP: "   + s.ip;
    if (s.mac)      document.getElementById("f_mac").textContent  = "MAC: "  + s.mac;
    if (s.hostname) document.getElementById("f_host").textContent = "Host: " + s.hostname;
    if (s.lastDownloadMs && s.lastDownloadMs !== lastImgStamp) {
      lastImgStamp = s.lastDownloadMs;
      document.getElementById("img").src = "/img/current?ts=" + lastImgStamp;
    }
  } catch(e) {
    ["s_wifi","s_mdns","s_mqtt","s_dl"].forEach(id => setStatus(id.replace("s_",""), id.replace("s_",""), false));
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
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
  <title>SyncFrame &mdash; Config</title>
  <style>
    *,*::before,*::after{box-sizing:border-box}
    body{
      margin:0;
      padding:20px;
      min-height:100vh;
      background:
        radial-gradient(circle at 20% 20%, rgba(255,0,150,0.8) 0%, transparent 50%),
        radial-gradient(circle at 80% 80%, rgba(0,255,100,0.7) 0%, transparent 50%),
        radial-gradient(circle at 80% 20%, rgba(0,200,255,0.8) 0%, transparent 50%),
        radial-gradient(circle at 20% 80%, rgba(255,100,0,0.7) 0%, transparent 50%),
        linear-gradient(135deg,#1a0033 0%,#003333 100%);
      background-attachment:fixed;
      background-size:150% 150%;
      font-family:Arial,sans-serif;
      color:#ffffff;
      animation:gradientShift 15s ease infinite;
    }
    @keyframes gradientShift{
      0%{background-position:0% 50%}
      50%{background-position:100% 50%}
      100%{background-position:0% 50%}
    }
    .card{
      max-width:860px;
      margin:0 auto;
      background:rgba(0,0,0,0.45);
      backdrop-filter:blur(12px);
      border:1px solid rgba(255,255,255,0.15);
      border-radius:14px;
      padding:24px;
    }
    h1{margin:0 0 4px;font-size:1.8em;text-shadow:0 2px 8px rgba(0,0,0,0.6)}
    h3{margin:18px 0 6px;font-size:1em;opacity:0.8;text-transform:uppercase;letter-spacing:.05em}
    label{display:block;margin:10px 0 4px;font-size:0.9em;opacity:0.85}
    input[type="text"],
    input[type="password"],
    input[type="number"]{
      width:100%;
      padding:10px;
      border-radius:8px;
      border:1px solid rgba(255,255,255,0.2);
      background:rgba(0,0,0,0.4);
      color:#fff;
      font-size:0.95em;
    }
    input[type="checkbox"]{accent-color:#37d67a;width:15px;height:15px;vertical-align:middle;margin-right:6px}
    .row{display:flex;gap:14px;flex-wrap:wrap}
    .row>div{flex:1;min-width:220px}
    button{
      background:rgba(255,255,255,0.15);
      color:#ffffff;
      border:1px solid rgba(255,255,255,0.3);
      padding:10px 20px;
      border-radius:6px;
      cursor:pointer;
      backdrop-filter:blur(10px);
      font-size:1em;
      font-weight:600;
      margin-top:12px;
      transition:background 0.3s ease;
    }
    button:hover{background:rgba(255,255,255,0.25)}
    button.save{
      background:rgba(55,214,122,0.25);
      border-color:rgba(55,214,122,0.6);
      color:#37d67a;
    }
    button.save:hover{background:rgba(55,214,122,0.4)}
    .msg{
      background:rgba(55,214,122,0.15);
      border:1px solid rgba(55,214,122,0.5);
      padding:10px;
      border-radius:8px;
      margin:10px 0;
      color:#37d67a;
      display:none;
    }
    .status-box{
      margin-top:6px;
      padding:12px;
      border-radius:10px;
      background:rgba(0,0,0,0.35);
      border:1px solid rgba(255,255,255,0.12);
      display:flex;
      flex-wrap:wrap;
      gap:14px;
      font-size:0.85em;
    }
    .ok{color:#37d67a;font-weight:600}
    .bad{color:#ff5d5d;font-weight:600}
    .toolbar{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:10px 0;font-size:0.88em}
    .console{
      margin-top:8px;
      background:rgba(0,0,0,0.55);
      border:1px solid rgba(255,255,255,0.12);
      border-radius:10px;
      padding:10px;
      min-height:200px;
      max-height:260px;
      overflow:auto;
      white-space:pre-wrap;
      font:12px/1.35 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
      color:#a8d8b0;
      text-align:left;
    }
    .back-link{color:rgba(255,255,255,0.6);text-decoration:none;font-size:0.9em}
    .back-link:hover{color:#fff}
    .header-row{display:flex;align-items:center;gap:14px;margin-bottom:16px}
    .small{font-size:0.82em;opacity:0.7}
  </style>
</head>
<body>
  <div class="card">
    <div class="header-row">
      <h1>Config / Debug</h1>
      <a class="back-link" href="/">&larr; Back</a>
    </div>

    <div id="saveMsg" class="msg">Settings saved!</div>

    <form id="configForm">
      <h3>Photo</h3>
      <label>Photo base URL (include trailing / if needed)</label>
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
          <label>HTTP Basic auth pass <span class="small">(leave blank to keep current)</span></label>
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
          <label>Pass <span class="small">(leave blank to keep current)</span></label>
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

      <button type="submit" class="save">Save Settings</button>
      <p class="small" style="margin-top:10px">
        Wi-Fi credentials are configured via captive portal only before the device has successfully joined Wi-Fi.
      </p>
    </form>

    <h3 style="margin-top:20px">Live Status</h3>
    <div id="debugStatus" class="status-box small">
      <span id="ds_host">Host: &hellip;</span>
      <span id="ds_mac">MAC: &hellip;</span>
      <span id="ds_ip">IP: &hellip;</span>
      <span id="ds_wifi">WiFi: &hellip;</span>
      <span id="ds_mqtt">MQTT: &hellip;</span>
      <span id="ds_photo">Photo: &hellip;</span>
    </div>

    <h3 style="margin-top:20px">Live Console</h3>
    <div class="toolbar">
      <label style="margin:0">
        <input type="checkbox" id="logEnable"/>
        Enable console polling
      </label>
      <button type="button" id="clearLogBtn">Reload log</button>
      <span class="small">Polling runs while this page is open and console is enabled.</span>
    </div>
    <pre id="logBox" class="console">Console is idle. Enable polling to load recent events.</pre>
  </div>

<script>
let currentConfig = {};
let logSince = 0;
let logTimer = null;

async function loadCfg() {
  try {
    const r = await fetch("/api/config", {cache:"no-store"});
    const c = await r.json();
    currentConfig = c;
    photoBaseUrl.value    = c.photoBaseUrl    || "";
    photoFilename.value   = c.photoFilename   || "";
    httpsInsecure.checked = !!c.httpsInsecure;
    httpUser.value        = c.httpUser        || "";
    httpPass.value        = "";
    mqttHost.value        = c.mqttHost        || "";
    mqttPort.value        = c.mqttPort        || "";
    mqttTopic.value       = c.mqttTopic       || "";
    mqttUser.value        = c.mqttUser        || "";
    mqttPass.value        = "";
    mqttUseTLS.checked    = !!c.mqttUseTLS;
    mqttTlsInsecure.checked = !!c.mqttTlsInsecure;
  } catch(e) { console.error("Failed to load config:", e); }
}

function setSpan(id, text, ok) {
  const el = document.getElementById(id);
  if (!el) return;
  if (ok === true)       el.className = "ok";
  else if (ok === false) el.className = "bad";
  else                   el.className = "";
  el.textContent = text;
}

async function loadStatus() {
  try {
    const r = await fetch("/api/status", {cache:"no-store"});
    const s = await r.json();
    setSpan("ds_host",  "Host: "  + (s.hostname || "-"), null);
    setSpan("ds_mac",   "MAC: "   + (s.mac      || "-"), null);
    setSpan("ds_ip",    "IP: "    + (s.ip       || "offline"), null);
    setSpan("ds_wifi",  "WiFi: "  + (s.wifi  ? "connected" : "disconnected"), !!s.wifi);
    setSpan("ds_mqtt",  "MQTT: "  + (s.mqtt  ? "connected" : "disconnected"), !!s.mqtt);
    setSpan("ds_photo", s.lastDownloadOk
      ? "Photo: ok"
      : "Photo: failed (" + (s.lastDownloadErr || "unknown") + ")",
      !!s.lastDownloadOk);
  } catch(e) { setSpan("ds_host", "Status unavailable", false); }
}

function appendLogLine(item) {
  const t = (Number(item.ms || 0) / 1000).toFixed(3);
  const line = "[" + t + "] " + (item.tag || "LOG") + " " + (item.msg || "");
  const lines = logBox.textContent ? logBox.textContent.split("\n") : [];
  lines.push(line);
  if (lines.length > 80) lines.splice(0, lines.length - 80);
  logBox.textContent = lines.join("\n");
  logBox.scrollTop = logBox.scrollHeight;
}

async function pollLogs(reset) {
  if (!logEnable.checked || document.hidden) return;
  let url = "/api/log";
  if (!reset && logSince) url += "?since=" + encodeURIComponent(logSince);
  const r = await fetch(url, {cache:"no-store"});
  const data = await r.json();
  if (reset) logBox.textContent = "";
  (data.items || []).forEach(appendLogLine);
  if (typeof data.nextSince === "number") logSince = data.nextSince;
  if (!logBox.textContent) logBox.textContent = "No events yet.";
}

function setLogPolling(enabled) {
  if (logTimer) { clearInterval(logTimer); logTimer = null; }
  if (!enabled) return;
  logSince = 0;
  pollLogs(true);
  logTimer = setInterval(() => pollLogs(false), 2000);
}

configForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  const f = new URLSearchParams();
  f.append('photoBaseUrl',  photoBaseUrl.value);
  f.append('photoFilename', photoFilename.value);
  f.append('httpUser',      httpUser.value);
  f.append('mqttHost',      mqttHost.value);
  f.append('mqttPort',      mqttPort.value);
  f.append('mqttTopic',     mqttTopic.value);
  f.append('mqttUser',      mqttUser.value);
  if (httpPass.value.length > 0)  f.append('httpPass',  httpPass.value);
  if (mqttPass.value.length > 0)  f.append('mqttPass',  mqttPass.value);
  if (httpsInsecure.checked)      f.append('httpsInsecure',   '1');
  if (mqttUseTLS.checked)         f.append('mqttUseTLS',      '1');
  if (mqttTlsInsecure.checked)    f.append('mqttTlsInsecure', '1');
  try {
    const resp = await fetch('/api/config', {method:'POST', body:f});
    if (resp.ok) {
      saveMsg.style.display = 'block';
      setTimeout(() => saveMsg.style.display = 'none', 3000);
      setTimeout(loadCfg, 500);
      setTimeout(loadStatus, 500);
      if (logEnable.checked) setTimeout(() => pollLogs(false), 500);
    }
  } catch(e) { alert('Save failed: ' + e); }
});

logEnable.addEventListener('change', () => {
  if (!logEnable.checked) { if (logTimer) { clearInterval(logTimer); logTimer = null; } return; }
  setLogPolling(true);
});
clearLogBtn.addEventListener('click', () => { logSince = 0; pollLogs(true); });
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) { loadStatus(); if (logEnable.checked) pollLogs(false); }
});

loadCfg();
loadStatus();
setInterval(() => { if (!document.hidden) loadStatus(); }, 3000);
</script>
</body>
</html>
)HTML";
