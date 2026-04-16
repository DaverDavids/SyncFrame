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
    h1{color:#ffffff;text-shadow:0 2px 10px rgba(0,0,0,0.7);font-size:2.5em;margin-bottom:20px}
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
    .image-container img{width:100%;height:100%;object-fit:contain;display:block}
    .controls{margin:22px 0 10px}
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
      margin-top:20px;padding:8px;
      color:rgba(255,255,255,0.55);
      font-size:0.82em;
      display:flex;flex-wrap:wrap;
      justify-content:center;gap:18px;align-items:center;
    }
    footer a{color:rgba(255,255,255,0.4);text-decoration:none;border-bottom:1px dotted rgba(255,255,255,0.25);transition:color 0.2s}
    footer a:hover{color:rgba(255,255,255,0.75)}
    #last-updated{width:100%;text-align:center;margin-top:4px;color:rgba(255,255,255,0.5);font-size:0.82em}
    @media(max-width:600px){h1{font-size:1.8em}footer{flex-direction:column;gap:6px}}
  </style>
</head>
<body>
  <h1>SyncFrame</h1>
  <div class="controls">
    <button onclick="refreshNow()">&#8635; Refresh the Frame</button>
  </div>
  <div class="image-container">
    <img id="img" alt="current photo"/>
  </div>
  <p id="last-updated">No photo loaded yet</p>
  <footer>
    <span id="f_ip">IP: &hellip;</span>
    <span id="f_mac">MAC: &hellip;</span>
    <span id="f_host">Host: &hellip;</span>
    <a href="/config">Config</a>
  </footer>
<script>
let lastImgStamp = null;
// Load image immediately on page open without waiting for a hash change
document.getElementById("img").src = "/img/current?ts=" + Date.now();
async function poll() {
  try {
    const r = await fetch("/api/status",{cache:"no-store",credentials:"include"});
    const s = await r.json();
    if (s.ip)       document.getElementById("f_ip").textContent   = "IP: "  +s.ip;
    if (s.mac)      document.getElementById("f_mac").textContent  = "MAC: " +s.mac;
    if (s.hostname) document.getElementById("f_host").textContent = "Host: "+s.hostname;
    if (s.photoHash !== lastImgStamp) {
      lastImgStamp = s.photoHash;
      document.getElementById("img").src = "/img/current?ts=" + encodeURIComponent(s.photoHash || Date.now());
      if (s.photoHash) {
        document.getElementById("last-updated").textContent =
          "Last updated: " + new Date().toLocaleString('en-US',{month:'short',day:'numeric',hour:'numeric',minute:'2-digit',hour12:true});
      }
    }
  } catch(e) {}
}
async function refreshNow() {
  await fetch("/api/refresh",{method:"POST",credentials:"include"});
  await poll();
}
async function rebootDevice() {
  await fetch("/api/reboot",{method:"POST",credentials:"include"});
}
poll();
setInterval(poll, 2000);
</script>
</body>
</html>
)HTML";

// Config page. The token CFG_INJECT_PLACEHOLDER is replaced at runtime by
// handleConfigPage() with <script>window._cfg={...};</script> before serving.
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
      margin:0;padding:20px;min-height:100vh;
      background:
        radial-gradient(circle at 20% 20%, rgba(255,0,150,0.8) 0%, transparent 50%),
        radial-gradient(circle at 80% 80%, rgba(0,255,100,0.7) 0%, transparent 50%),
        radial-gradient(circle at 80% 20%, rgba(0,200,255,0.8) 0%, transparent 50%),
        radial-gradient(circle at 20% 80%, rgba(255,100,0,0.7) 0%, transparent 50%),
        linear-gradient(135deg,#1a0033 0%,#003333 100%);
      background-attachment:fixed;background-size:150% 150%;
      font-family:Arial,sans-serif;color:#ffffff;
      animation:gradientShift 15s ease infinite;
    }
    @keyframes gradientShift{0%{background-position:0% 50%}50%{background-position:100% 50%}100%{background-position:0% 50%}}
    .card{max-width:860px;margin:0 auto;background:rgba(0,0,0,0.45);backdrop-filter:blur(12px);border:1px solid rgba(255,255,255,0.15);border-radius:14px;padding:24px}
    h1{margin:0 0 4px;font-size:1.8em;text-shadow:0 2px 8px rgba(0,0,0,0.6)}
    h3{margin:18px 0 6px;font-size:1em;opacity:0.8;text-transform:uppercase;letter-spacing:.05em}
    label{display:block;margin:10px 0 4px;font-size:0.9em;opacity:0.85}
    input[type="text"],input[type="password"],input[type="number"],input[type="url"]{
      width:100%;padding:10px;border-radius:8px;
      border:1px solid rgba(255,255,255,0.2);
      background:rgba(0,0,0,0.4);color:#fff;font-size:0.95em;
    }
    input[type="checkbox"]{accent-color:#37d67a;width:15px;height:15px;vertical-align:middle;margin-right:6px}
    .row{display:flex;gap:14px;flex-wrap:wrap}
    .row>div{flex:1;min-width:220px}
    button{
      background:rgba(255,255,255,0.15);color:#ffffff;
      border:1px solid rgba(255,255,255,0.3);
      padding:10px 20px;border-radius:6px;cursor:pointer;
      backdrop-filter:blur(10px);font-size:1em;font-weight:600;
      margin-top:12px;transition:background 0.3s ease;
    }
    button:hover{background:rgba(255,255,255,0.25)}
    button.save{background:rgba(55,214,122,0.25);border-color:rgba(55,214,122,0.6);color:#37d67a}
    button.save:hover{background:rgba(55,214,122,0.4)}
    button:disabled{opacity:0.4;cursor:not-allowed}
    button.save:disabled{background:rgba(55,214,122,0.08);color:rgba(55,214,122,0.4)}
    .msg{background:rgba(55,214,122,0.15);border:1px solid rgba(55,214,122,0.5);padding:10px;border-radius:8px;margin:10px 0;color:#37d67a;display:none}
    .err{background:rgba(255,93,93,0.12);border:1px solid rgba(255,93,93,0.4);padding:10px;border-radius:8px;margin:10px 0;color:#ff8a8a;display:none}
    .status-bar{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:4px;font-size:13px}
    .status-bar span{background:rgba(0,0,0,0.3);backdrop-filter:blur(6px);border:1px solid rgba(255,255,255,0.15);border-radius:20px;padding:4px 14px;font-weight:600}
    .status-box{margin-top:6px;padding:12px;border-radius:10px;background:rgba(0,0,0,0.35);border:1px solid rgba(255,255,255,0.12);display:flex;flex-wrap:wrap;gap:14px;font-size:0.85em}
    .ok{color:#37d67a;font-weight:600}.bad{color:#ff5d5d;font-weight:600}
    .toolbar{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:10px 0;font-size:0.88em}
    .console{margin-top:8px;background:rgba(0,0,0,0.55);border:1px solid rgba(255,255,255,0.12);border-radius:10px;padding:10px;min-height:200px;max-height:260px;overflow:auto;white-space:pre-wrap;font:12px/1.35 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;color:#a8d8b0;text-align:left}
    .back-link{color:rgba(255,255,255,0.6);text-decoration:none;font-size:0.9em}
    .back-link:hover{color:#fff}
    .reboot-btn{background:#c00;color:#fff;border:none;padding:6px 12px;border-radius:4px;cursor:pointer;font-size:0.85em;margin-left:auto}
    .reboot-btn:hover{background:#e00}
    .header-row{display:flex;align-items:center;gap:14px;margin-bottom:16px}
    .small{font-size:0.82em;opacity:0.7}
    .section-divider{border:none;border-top:1px solid rgba(255,255,255,0.1);margin:20px 0 4px}
    .warn{background:rgba(255,180,0,0.12);border:1px solid rgba(255,180,0,0.35);color:#ffd54f;padding:8px 12px;border-radius:8px;font-size:0.82em;margin-top:6px}
    .uptime-badge{display:inline-block;background:rgba(55,214,122,0.12);border:1px solid rgba(55,214,122,0.3);border-radius:20px;padding:3px 12px;font-size:0.82em;color:#37d67a;font-weight:600;margin-left:8px;vertical-align:middle}
  </style>
</head>
<body>
<div class="card">
  <div class="header-row">
    <h1>Config / Debug</h1>
    <span id="hostnameHint" class="small"></span>
    <a class="back-link" href="/">&larr; Back</a>
    <button type="button" class="reboot-btn" onclick="rebootDevice()">Reboot</button>
  </div>

  <h3>Device Status</h3>
  <div class="status-bar">
    <span id="s_wifi">WiFi: &hellip;</span>
    <span id="s_mdns">mDNS: &hellip;</span>
    <span id="s_stream">Stream: &hellip;</span>
    <span id="s_dl">Photo: &hellip;</span>
    <span id="s_ota">OTA: &hellip;</span>
    <span id="s_uptime">Up: &hellip;</span>
  </div>

  <div id="saveMsg" class="msg">Settings saved!</div>
  <div id="errMsg"  class="err"></div>

  <form id="configForm">
    <h3>Photo</h3>
    <label>Photo base URL</label>
    <input type="url" name="photoBaseUrl" id="photoBaseUrl" placeholder="https://192.168.1.10:9369/syncframe/"/>

    <div class="row" style="align-items:flex-end">
      <div style="flex:7">
        <label>Photo filename</label>
        <input type="text" name="photoFilename" id="photoFilename" placeholder="photo.800x480.jpg" style="width:100%"/>
      </div>
      <div style="flex:3">
        <label>Reconnect (min)</label>
        <input type="number" name="streamReconnectMin" id="streamReconnectMin" min="1" max="1440" value="10" style="width:100%"/>
      </div>
    </div>

    <label><input type="checkbox" name="httpsInsecure" id="httpsInsecure"/> Allow insecure HTTPS (self-signed)</label>

    <label>Peek button GPIO pin <span class="small">(C3 only; -1 = disabled)</span></label>
    <input type="number" name="peekButtonPin" id="peekButtonPin" min="-1" max="48" value="-1" style="width:120px"/>

    <div class="row">
      <div>
        <label>HTTP Basic auth user</label>
        <input type="text" name="httpUser" id="httpUser" placeholder="admin"/>
      </div>
      <div>
        <label>HTTP Basic auth pass <span class="small">(blank = keep current)</span></label>
        <input type="password" name="httpPass" id="httpPass" autocomplete="off"/>
      </div>
    </div>

    <hr class="section-divider"/>
    <h3>Web UI Login</h3>
    <p class="warn">&#9888; Leave password blank to disable authentication (open access on local network).</p>
    <div class="row">
      <div>
        <label>Username</label>
        <input type="text" name="webUser" id="webUser" placeholder="admin" autocomplete="username"/>
      </div>
      <div>
        <label>Password <span class="small">(blank = keep current)</span></label>
        <input type="password" name="webPass" id="webPass" autocomplete="new-password"/>
      </div>
    </div>
    <label style="margin-top:8px">
      <input type="checkbox" id="webPassClearCb"/> Clear password (disable authentication)
    </label>

    <button type="submit" id="saveBtn" class="save" style="margin-top:20px;width:100%">
      &#10003; Save Settings
    </button>
  </form>

  <h3 style="margin-top:24px">Live Status</h3>
  <div id="debugStatus" class="status-box small">
    <span id="ds_host">Host: &hellip;</span>
    <span id="ds_mac">MAC: &hellip;</span>
    <span id="ds_ip">IP: &hellip;</span>
    <span id="ds_wifi">WiFi: &hellip;</span>
    <span id="ds_stream">Stream: &hellip;</span>
    <span id="ds_photo">Photo: &hellip;</span>
    <span id="ds_ota">OTA: idle</span>
    <span id="ds_uptime">Uptime: &hellip;</span>
  </div>

  <h3 style="margin-top:20px">Live Console</h3>
  <div class="toolbar">
    <label style="margin:0"><input type="checkbox" id="logEnable"/> Enable console polling</label>
    <button type="button" id="clearLogBtn">Reload log</button>
    <span class="small">Polling runs while this page is open and console is enabled.</span>
  </div>
  <pre id="logBox" class="console">Console is idle. Enable polling to load recent events.</pre>
</div>

CFG_INJECT_PLACEHOLDER
<script>
var c = window._cfg || {};

function apiFetch(url, opts) {
  return fetch(url, Object.assign({credentials:"include",cache:"no-store"}, opts||{}));
}

async function rebootDevice() {
  if (!confirm("Reboot the device?")) return;
  try { await apiFetch("/api/reboot", {method:"POST"}); } catch(e) {}
}

let logSince = 0;
let logTimer = null;

function applyCfg(c) {
  document.getElementById("photoBaseUrl").value      = c.photoBaseUrl      || "";
  document.getElementById("photoFilename").value     = c.photoFilename     || "";
  document.getElementById("httpsInsecure").checked   = !!c.httpsInsecure;
  document.getElementById("httpUser").value          = c.httpUser          || "";
  document.getElementById("httpPass").value          = "";
  document.getElementById("httpPass").placeholder    = c.httpPass  ? "\u25CF\u25CF\u25CF\u25CF\u25CF\u25CF\u25CF\u25CF" : "(not set)";
  document.getElementById("webUser").value           = c.webUser           || "";
  document.getElementById("webPass").value           = "";
  document.getElementById("webPass").placeholder     = c.webPass  ? "\u25CF\u25CF\u25CF\u25CF\u25CF\u25CF\u25CF\u25CF" : "(not set \u2014 auth disabled)";
  if (c.hostname) document.getElementById("hostnameHint").textContent = c.hostname;
  if (document.getElementById("streamReconnectMin")) document.getElementById("streamReconnectMin").value = c.streamReconnectMin || 10;
  if (document.getElementById("peekButtonPin")) document.getElementById("peekButtonPin").value = (c.peekButtonPin !== undefined) ? c.peekButtonPin : -1;
}

applyCfg(c);

function formatUptime(ms) {
  if (!ms && ms !== 0) return "?";
  var s = Math.floor(ms / 1000);
  var d = Math.floor(s / 86400); s %= 86400;
  var h = Math.floor(s / 3600);  s %= 3600;
  var m = Math.floor(s / 60);    s %= 60;
  if (d > 0) return d+"d "+h+"h "+m+"m";
  if (h > 0) return h+"h "+m+"m "+s+"s";
  return m+"m "+s+"s";
}

function setPill(id, label, ok) {
  const el = document.getElementById(id);
  if (!el) return;
  el.innerHTML = label+": "+(ok
    ? "<span class='ok'>&#10003;</span>"
    : "<span class='bad'>&#10007;</span>");
}
function setSpan(id, text, ok) {
  const el = document.getElementById(id);
  if (!el) return;
  el.className = (ok===true)?"ok":(ok===false)?"bad":"";
  el.textContent = text;
}

async function loadStatus() {
  try {
    const r = await apiFetch("/api/status");
    if (!r.ok) { setSpan("ds_host","Status unavailable (HTTP "+r.status+")",false); return; }
    const s = await r.json();
    setPill("s_wifi", "WiFi",  !!s.wifi);
    setPill("s_mdns", "mDNS",  !!s.mdns);
    setPill("s_stream", "Stream", !!s.mjpeg);
    setPill("s_mjpeg", "MJPEG", !!s.mjpeg);
    setPill("s_dl",   "Photo", !!s.photoHash);
    setPill("s_ota",  "OTA",   true);
    // Uptime pill
    if (s.uptimeMs !== undefined) {
      const el = document.getElementById("s_uptime");
      if (el) el.textContent = "Up: " + formatUptime(s.uptimeMs);
    }
    setSpan("ds_host",  "Host: "+(s.hostname||"-"),       null);
    setSpan("ds_mac",   "MAC: " +(s.mac     ||"-"),       null);
    setSpan("ds_ip",    "IP: "  +(s.ip      ||"offline"), null);
    setSpan("ds_wifi",  "WiFi: "+(s.wifi  ?"connected":"disconnected"), !!s.wifi);
    setSpan("ds_stream", "Stream: "+(s.mjpeg?"connected":"disconnected"), !!s.mjpeg);
    setSpan("ds_mjpeg", "MJPEG: "+(s.mjpeg?"connected":"disconnected"), !!s.mjpeg);
    setSpan("ds_photo", "Photo hash: "+(s.photoHash||"none"), !!s.photoHash);
    setSpan("ds_ota",   "OTA: idle", true);
    if (s.uptimeMs !== undefined)
      setSpan("ds_uptime", "Uptime: "+formatUptime(s.uptimeMs), null);
    if (s.hostname) document.getElementById("hostnameHint").textContent = s.hostname;
  } catch(e) { setSpan("ds_host","Status unavailable",false); }
}

function appendLogLine(item) {
  const t = (Number(item.ms||0)/1000).toFixed(3);
  const line = "["+t+"] "+(item.tag||"LOG")+" "+(item.msg||"");
  const lb = document.getElementById("logBox");
  const lines = lb.textContent ? lb.textContent.split("\n") : [];
  lines.push(line);
  if (lines.length > 80) lines.splice(0, lines.length-80);
  lb.textContent = lines.join("\n");
  lb.scrollTop = lb.scrollHeight;
}

async function pollLogs(reset) {
  const logEnable = document.getElementById("logEnable");
  if (!logEnable.checked || document.hidden) return;
  let url = "/api/log";
  if (!reset && logSince) url += "?since="+encodeURIComponent(logSince);
  const r = await apiFetch(url);
  const data = await r.json();
  const lb = document.getElementById("logBox");
  if (reset) lb.textContent = "";
  (data.items||[]).forEach(appendLogLine);
  if (typeof data.nextSince==="number") logSince = data.nextSince;
  if (!lb.textContent) lb.textContent = "No events yet.";
}

function setLogPolling(enabled) {
  if (logTimer) { clearInterval(logTimer); logTimer = null; }
  if (!enabled) return;
  logSince = 0;
  pollLogs(true);
  logTimer = setInterval(()=>pollLogs(false), 2000);
}

document.getElementById("configForm").addEventListener('submit', async (e) => {
  e.preventDefault();
  const saveBtn = document.getElementById("saveBtn");
  if (saveBtn.disabled) return;
  const f = new URLSearchParams();
  f.append('photoBaseUrl',       document.getElementById("photoBaseUrl").value);
  f.append('photoFilename',      document.getElementById("photoFilename").value);
  f.append('streamReconnectMin', document.getElementById("streamReconnectMin").value);
  f.append('peekButtonPin', document.getElementById("peekButtonPin").value);
  f.append('httpUser',           document.getElementById("httpUser").value);
  const webUserVal  = document.getElementById("webUser").value;
  const httpPassVal = document.getElementById("httpPass").value;
  const webPassVal  = document.getElementById("webPass").value;
  if (webUserVal.length  > 0) f.append('webUser',  webUserVal);
  if (httpPassVal.length > 0) f.append('httpPass', httpPassVal);
  if (webPassVal.length  > 0) f.append('webPass',  webPassVal);
  if (document.getElementById('webPassClearCb').checked) f.append('webPassClear','1');
  if (document.getElementById("httpsInsecure").checked)   f.append('httpsInsecure',   '1');
  saveBtn.disabled = true;
  saveBtn.textContent = 'Saving\u2026';
  try {
    const resp = await apiFetch('/api/config',{method:'POST',body:f});
    if (resp.ok) {
      document.getElementById("saveMsg").style.display = 'block';
      setTimeout(()=>document.getElementById("saveMsg").style.display='none', 3000);
      saveBtn.disabled = false;
      saveBtn.textContent = '\u2713 Save Settings';
      loadStatus();
    } else {
      saveBtn.disabled = false;
      saveBtn.textContent = '\u2713 Save Settings';
    }
  } catch(e) {
    alert('Save failed: '+e);
    saveBtn.disabled = false;
    saveBtn.textContent = '\u2713 Save Settings';
  }
});

document.getElementById("logEnable").addEventListener('change', function() {
  if (!this.checked) { if (logTimer){clearInterval(logTimer);logTimer=null;} return; }
  setLogPolling(true);
});
document.getElementById("clearLogBtn").addEventListener('click', ()=>{logSince=0;pollLogs(true);});
document.addEventListener('visibilitychange', ()=>{
  if (!document.hidden){loadStatus();const le=document.getElementById("logEnable");if(le.checked)pollLogs(false);}
});

loadStatus();
setInterval(()=>{if(!document.hidden)loadStatus();}, 3000);
</script>
</body>
</html>
)HTML";
