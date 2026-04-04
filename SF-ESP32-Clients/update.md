<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# We have two logs in the config web UI that seem to be the same, 'live console' and 'connection log' don't seem different to me. Describe the exact fixes needed for this and the refresh issue, concisely, so a third party can implement them.

Here is a concise spec for a third party to implement both fixes.

***

## Fix Spec: `html.h` + `SF-ESP32-Clients.ino`

### Fix 1 — Remove the Duplicate "Connection Log"

**File:** `html.h` → `CONFIG_HTML`

**Problem:** The page has two sections that both poll `/api/log` and display the same data — "Live Console" (the `<pre id="logBox">` block with the enable checkbox) and "Connection Log" (the `<div id="connlog">` block below the `<hr>`). The Connection Log was an earlier prototype that was never removed.

```
**Change:** Delete the following block from the HTML (everything from `<hr>` down to and including its `<button>` and closing `</div>`):
```

```html
<hr>
<h3>Connection Log</h3>
<div id="connlog" style="..."> </div>
<div style="..." id="connlogstatus">Polling...</div>
<button onclick="document.getElementById('connlog').innerHTML=''">Clear log</button>
```

Also delete the entire Connection Log JS IIFE at the bottom of the `<script>` block — the self-invoking `(function(){ var connlog=... poll(); setInterval(poll,2000); })();` closure.

The "Live Console" section already does the same job with better UI (enable toggle, reload button, 80-line cap, sequence-based deduplication).

***

### Fix 2 — Manual "Refresh the Frame" Always Redraws

**Problem:** `handleActionRefresh()` sets `webRefreshPending = true`, which calls `spawnPhotoTask()` → `downloadAndShowPhoto()`. Inside that function, if the downloaded image bytes match what's already in `currentJpg` (memcmp), or if the ETag HEAD check says the file hasn't changed, the function short-circuits and skips the full rotation + redraw path. A manual refresh should always go through the complete draw pipeline.

#### Change A — `SF-ESP32-Clients.ino`: add a force flag

Near the other `volatile bool` flags at the top of the file, add:

```cpp
static volatile bool forceRedraw = false;
```


#### Change B — `handleActionRefresh()`: set the flag

```cpp
static void handleActionRefresh() {
  if (!requireWebAuth()) return;
  if (otaInProgress) {
    server.send(503, "application/json", "{\"ok\":false,\"err\":\"ota in progress\"}");
    return;
  }
  logEvent("WEB", "manual refresh requested (force)");
  forceRedraw = true;          // ← add this line
  webRefreshPending = true;
  server.send(200, "application/json", "{\"ok\":true}");
}
```


#### Change C — `downloadAndShowPhoto()`: consume the flag and skip both dedup checks

At the top of the function body, immediately after the `otaInProgress` guard:

```cpp
bool isForced = forceRedraw;
forceRedraw = false;
```

Then add `!isForced &&` to the condition of the memcmp bail-out:

```cpp
if (!isForced && currentJpg && newLen == currentJpgLen &&
    memcmp(newBuf, currentJpg, newLen) == 0) {
```

And add `!isForced &&` to the ETag block condition:

```cpp
if (!isForced && lastUploadEtag[0] != '\0') {
    // ... existing HEAD/ETag check unchanged ...
} else if (!isForced) {
    strcpy(lastUploadEtag, "init");
}
```

When `isForced` is true, both checks are skipped entirely and execution falls through to the full rotation + `board_draw_jpeg` path with the freshly downloaded buffer, regardless of whether the image content has changed.

