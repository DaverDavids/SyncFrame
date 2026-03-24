<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# Explain what exactly needs changed... give context so a third party can implement. be concise

Here's a concise spec for a third party:

***

## Problem

The server updates `photo.jpg` in two cases: **new upload** and **nightly desaturation**. Both trigger an MQTT `refresh`, causing the ESP32 client to rotate `currentJpg → lastJpg`. The touch-to-show-last-photo feature should only activate on genuine uploads, not desaturation.

## Solution: ETag on the photo endpoint

### Server (`SF-Server/syncframe-server.py`)

1. Maintain a variable `photo_upload_etag` — a short unique string (e.g. `secrets.token_hex(8)`).
2. **On upload** (`upload_file()`): generate a new `photo_upload_etag` and persist it to `data/photo_upload_etag.txt`.
3. **On desaturation** (`desaturate_image()` / `finalize_changes()`): do **not** change the etag.
4. On startup: load `photo_upload_etag` from `data/photo_upload_etag.txt` if it exists, else generate one and save it.
5. In `serve_photo()` and `serve_photo_variant()`: add the header `ETag: "<photo_upload_etag>"` to the response.

### Client (`SF-ESP32-Clients/SF-ESP32-Clients.ino`)

1. Add a global `char lastUploadEtag[32]` initialized to `""`.
2. In `downloadAndShowPhoto()`, before rotating `lastJpg = currentJpg`:
    - Do a `HEAD` request to the photo URL.
    - Parse the `ETag` response header.
    - If the ETag differs from `lastUploadEtag`, it's a new upload — rotate `lastJpg = currentJpg`, then save the new ETag to `lastUploadEtag`.
    - If the ETag is the same, skip the rotation (just update `currentJpg` with the new image data as usual).
3. Older clients without this code simply never check the ETag and always rotate — **no behavior change for them**.

### Key constraint

The ETag must only change on `upload_file()`, never in `desaturate_image()` or `finalize_changes()`.

