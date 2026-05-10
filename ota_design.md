# OTA Implementation Design

Cross-session reference for the over-the-air update system.
Treat this as the source of truth — companion to `camera_manager_design.md` and `web_ui_spec.md`.

---

## 1. Summary & locked decisions

| Topic | Decision |
|---|---|
| Cloud hosting | GitHub Releases, fronted by Cloudflare Worker (CORS confirmed missing on direct GH downloads) |
| Channels | `stable` + `beta` published online; `dev` is local-only via `dev.ps1` |
| Integrity | SHA-256 of every blob, declared in manifest, verified on-device before commit |
| Signing | None — closed CAN bus, no PII, low-value target. Additive later if threat model changes. |
| Versioning | App + UI shipped as a pair; manifest declares both, install pushes both |
| SHA-skip | Recovery skips writing partitions whose existing SHA already matches incoming (free ergonomic upgrade) |
| Hardware recovery button | Future TODO — see §15 |
| SoftAP credentials | Shared SSID/PSK baked into `wifi_manager` defaults; both apps inherit |
| Two projects | `esp32_gopro_canbus_controller_v2` (main) + `esp32_gopro_canbus_recovery` (factory) |
| Daily dev | `.\dev.ps1` wrapper does build + OTA-flash + monitor |

---

## 2. Architecture overview

Two ESP-IDF projects share `partitions.csv`. Recovery lives in the `factory` partition (512 KB, dependency-free, embedded HTML, no LittleFS). Main app lives in `ota_0` (1.94 MB) with the web UI in the `storage` LittleFS partition (3 MB). The bootloader picks `ota_0` if `otadata` says so and the image is valid; otherwise falls back to `factory` automatically.

The browser is the only thing that ever talks to the public internet. The ESP32 makes no outbound TCP connections — it stays a SoftAP. A Cloudflare Worker fronts GitHub Releases purely to add CORS headers so the browser can `fetch()` from a page served by the device.

### Boot flow

```
  POWER ON
    │
    ▼
  bootloader
    │ reads otadata
    ▼
  ┌──────────────────────┐
  │ otadata points at    │
  │ a valid OTA slot?    │
  └──────────────────────┘
       │             │
      yes            no
       ▼             ▼
    ota_0 ── if app_main panics, watchdogs,
    (main)   or otherwise resets within 30 s
       │     before calling
       │     esp_ota_mark_app_valid_cancel_rollback()
       │     ──────────────────────────────────►
       │                                          │
       └──────────────────────────────────────────┤
                                                  ▼
                                              factory
                                              (recovery)
```

### Update flow (browser-as-proxy)

```
Browser (phone/laptop)        Cloudflare Worker          GitHub Releases       ESP32
        │                           │                          │                 │
        │── GET /api/version ───────────────────────────────────────────────────►│
        │◄────────────────────────────────────── {app, ui, channel} ─────────────│
        │                           │                          │                 │
        │── GET manifest.json ─────►│                          │                 │
        │                           │── GET github.com/... ───►│                 │
        │                           │◄──────── manifest.json ──│                 │
        │◄── manifest (CORS-OK) ────│                          │                 │
        │                           │                          │                 │
        │── GET app.bin ───────────►│                          │                 │
        │                           │── GET github.com/... ───►│                 │
        │                           │◄────────── app.bin ──────│                 │
        │◄── app.bin (CORS-OK) ─────│                          │                 │
        │   (same again for ui)     │                          │                 │
        │                           │                          │                 │
        │── POST /api/ota/upload-app ─────────────────────────────────────────────►
        │── POST /api/ota/upload-ui  ─────────────────────────────────────────────►
        │── POST /api/ota/commit     ─────────────────────────────────────────────►
        │       (device verifies SHAs, sets boot partition, reboots)              │
        │                           │                          │                 │
        │── poll GET /api/version until new version answers ─────────────────────►│
```

---

## 3. Partition layout

Replaces the existing single-app `partitions.csv`. 8 MB flash on ESP32-S3.

```
# Name      Type  SubType   Offset     Size       Notes
nvs         data  nvs       0x009000   0x006000   24 KB — NVS, unchanged
otadata     data  ota       0x00F000   0x002000   8 KB — which OTA slot is active
phy_init    data  phy       0x011000   0x001000   4 KB — PHY init data
factory     app   factory   0x020000   0x080000   512 KB — recovery app
ota_0       app   ota_0     0x0A0000   0x1F0000   1.94 MB — main app
storage     data  spiffs    0x290000   0x300000   3 MB — LittleFS (web UI for main app)
# free space 0x590000 .. 0x800000 = ~2.4 MB — reserved for future ota_1 (A/B updates)
```

Notes:
- `factory` is the *recovery* image, not the main app. Naming follows ESP-IDF convention: `factory` = the always-present fallback.
- `otadata` size 8 KB (two 4 KB sectors) is required for the bootloader's redundant-write scheme.
- The free 2.4 MB at the end is intentional. Adding `ota_1` later is purely a partition-table change; firmware code already uses the OTA APIs that handle multi-slot.

---

## 4. Threat model & explicit non-goals

### What we protect against
- Accidental corruption mid-upload (truncated download, dropped Wi-Fi).
- Wrong file uploaded by mistake (e.g., wrong release).
- Bricked main app — recovery is always reachable via factory fallback or rollback timer.
- Half-written partition from a power failure during update.

### What we do NOT protect against
- A malicious actor on the user's network pushing crafted firmware. (No code-signing.)
- A compromised GitHub account pushing malicious binaries.
- A compromised browser MITM'ing between cloud and device.
- Physical attack on flash chip / debug header.

### Why this is OK for this project
- Closed CAN bus, no bridge to vehicle-critical systems.
- No PII, no proprietary algorithms, no payment data.
- Open-source firmware on open-source hardware — nothing to "steal" by injecting a custom build.
- Worst case: someone who compromises the update flow can brick a small number of hobbyist boards. Bricked boards recover via USB. Low blast radius, low attacker payoff.

If any of this changes, see §15 (future TODOs) for the signing-addition path.

---

## 5. Manifest format

Single JSON file per channel. Hosted as a release asset on GitHub Releases, fetched via the Cloudflare Worker.

```json
{
  "channel": "stable",
  "released": "2026-05-15",
  "app": {
    "version": "1.4.2",
    "size": 1820416,
    "sha256": "9f1c...3b",
    "url": "app.bin"
  },
  "ui": {
    "version": "1.4.0",
    "size": 142336,
    "sha256": "ab02...cc",
    "url": "storage.bin"
  },
  "min_recovery_version": "0.1.0",
  "release_notes_url": "https://github.com/.../releases/tag/v1.4.2"
}
```

### Field semantics
- `channel`: must match the channel the device is asking for. Mismatch = manifest is wrong, abort.
- `released`: ISO-8601 date, informational only (shown in UI).
- `app.version`, `ui.version`: semver. Browser uses these for upgrade/downgrade detection.
- `app.size`, `ui.size`: expected byte count. Browser pre-allocates progress bar; device validates.
- `app.sha256`, `ui.sha256`: lowercase hex, 64 chars. Device verifies during upload.
- `app.url`, `ui.url`: relative to the manifest URL. Browser resolves them against the manifest's location.
- `min_recovery_version`: if device's recovery app is older than this, refuse install — recovery may not understand the upload protocol used by this build.
- `release_notes_url`: optional, shown in UI as a link.

### Channel routing

Two channels, two floating tags on GitHub:
- `stable` → `https://github.com/<owner>/<repo>/releases/download/latest-stable/manifest.json`
- `beta` → `https://github.com/<owner>/<repo>/releases/download/latest-beta/manifest.json`

CI moves the floating tag on each release. Device picks the URL by reading NVS `ota/channel` (default `"stable"`).

Through the Worker, the browser fetches from:
```
https://firmware-proxy.<account>.workers.dev/<owner>/<repo>/releases/download/latest-<channel>/manifest.json
```

---

## 6. API contracts

All endpoints below are served by **both** main app and recovery app unless marked otherwise. The browser doesn't need to know which mode it's talking to — endpoint shapes are identical.

### `GET /api/version`

Returns the running firmware's identity.

**Response 200:**
```json
{
  "app": "1.4.2",
  "ui": "1.4.0",
  "recovery": "0.1.0",
  "channel": "stable",
  "running_partition": "ota_0",
  "mode": "main"
}
```

Recovery returns `"mode": "recovery"` and `"running_partition": "factory"`. Recovery's `app` and `ui` fields reflect *its own* version (not the most recent main app's version).

The `ui` field in main mode is read from `/www/manifest.json` inside the LittleFS image. If that file is missing or unparseable, return `"ui": "unknown"`.

---

### `POST /api/ota/upload-app`

Streams a new main-app image into the inactive OTA slot.

**Headers (required):**
- `X-Sha256: <64-char hex>` — expected SHA-256 of body
- `X-Size: <decimal>` — expected byte length
- `Content-Type: application/octet-stream`

**Body:** raw bytes of `app.bin`.

**Response:**
- `200 OK`: `{ "written": <bytes>, "sha256": "<actual hex>", "skipped": false }`
- `200 OK` (SHA-skip): `{ "written": 0, "sha256": "<existing>", "skipped": true }` if device's current `ota_0` already has matching SHA.
- `400 Bad Request`: missing required headers or malformed values.
- `413 Payload Too Large`: body exceeds partition size.
- `422 Unprocessable Entity`: SHA mismatch at end of stream.
- `507 Insufficient Storage`: flash write failure.

**Side effects:** writes the inactive OTA slot. Does **not** change `otadata`. Caller must `POST /api/ota/commit`.

---

### `POST /api/ota/upload-ui`

Same shape as `upload-app` but writes the LittleFS `storage` partition.

The body is a complete LittleFS image (the same `storage.bin` produced by `littlefs_create_partition_image` at build time).

SHA-skip applies here too — if existing storage hashes to the incoming `X-Sha256`, return `{"skipped": true}` without writing.

---

### `POST /api/ota/commit`

Activates the staged update. Both `upload-app` and `upload-ui` must have completed successfully (or been SHA-skipped) before this call.

**Body:** none.

**Response:**
- `200 OK`: `{ "rebooting": true, "boot_partition": "ota_0" }`. Device reboots within ~100 ms of sending the response.
- `409 Conflict`: no successful upload in this session, nothing to commit.
- `422 Unprocessable Entity`: integrity check failed (shouldn't normally happen; uploads validate per-blob).

**Side effects:** `esp_ota_set_boot_partition(ota_0)`, then `esp_restart()`.

---

### `POST /api/ota/boot-main` *(recovery only)*

Switch back to the main app without uploading new firmware. Useful for testing, or if a user entered recovery by mistake.

**Body:** none. **Response:** `{ "rebooting": true, "boot_partition": "ota_0" }`.

Refuses (`409 Conflict`) if `ota_0` does not contain a valid app image.

---

### `POST /api/ota/reboot-recovery` *(main only)*

Force reboot into the recovery app.

**Body:** none. **Response:** `{ "rebooting": true, "boot_partition": "factory" }`.

**Side effects:** `esp_ota_set_boot_partition(factory)`, then `esp_restart()`.

---

### `GET /api/ota/channel` *(main only)*

**Response 200:** `{ "current": "stable", "available": ["stable", "beta"] }`.

The `available` list is hardcoded in `api_ota.c` for now. Compile-time flag `CONFIG_OTA_ALLOW_DEV_CHANNEL` adds `"dev"` for developer builds.

---

### `POST /api/ota/channel` *(main only)*

**Body:** `{ "channel": "beta" }`.

**Response 200:** `{ "current": "beta" }`.

**Side effects:** persists to NVS namespace `ota`, key `channel`. Takes effect on next manifest fetch — does not trigger an immediate reboot or update check.

---

## 7. Failure modes & recovery paths

| Failure | Detection | Recovery |
|---|---|---|
| User uploads truncated `app.bin` | SHA-256 mismatch on `upload-app` | Recovery returns 422; nothing committed; device unaffected |
| Power lost during `upload-app` | Inactive partition partially written, but `otadata` untouched | Bootloader keeps booting current slot; user re-uploads |
| Power lost during `upload-ui` | Storage partition corrupt | LittleFS re-formats on next mount (`format_if_mount_failed`); UI is gone but recovery is reachable from `factory`; user re-uploads UI |
| Power lost during `commit` after `set_boot_partition` but before reboot | New boot partition selected | Bootloader boots new app; if it crashes within rollback window, falls back to `factory` |
| New main app crashes during boot | No `mark_app_valid_cancel_rollback()` call within 30 s | Bootloader auto-reverts to `factory` on next reset (rollback) |
| New main app boots OK but is buggy | User notices after rollback window expired | "Reboot to recovery" button in UI → `set_boot_partition(factory)` → user re-uploads previous version |
| Main app marks valid then crashes hard, no rollback | Rollback already disarmed; main app keeps booting and crashing | **Last-resort path.** Currently: USB recovery via `flash-usb` from `dev.ps1`. Future: hardware boot button (§15). |
| Storage SHA matches but app SHA doesn't (or vice versa) | Per-blob SHA check at upload time | Whichever blob mismatches is rejected; the other can still be skipped or written. `commit` proceeds if all uploaded blobs validated. |
| Cloudflare Worker is down | Manifest fetch fails in browser | UI shows "Update server unreachable. Try again later, or upload manually." Device is unaffected. |
| User on SoftAP without internet on phone | Manifest fetch fails | UI shows "Phone needs internet. Keep cellular data on, or use a laptop with another connection." |

---

## 8. Phase 0 — Foundations

**Goal:** make the build system OTA-aware. No behavior change yet.

**Deliverables:**
- New `partitions.csv` with the layout from §3.
- `sdkconfig.defaults` additions:
  ```
  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
  CONFIG_PARTITION_TABLE_CUSTOM=y
  CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
  CONFIG_APP_PROJECT_VER_FROM_CONFIG=y
  CONFIG_APP_PROJECT_VER="0.1.0"
  ```
- New `Kconfig.projbuild` with `CONFIG_OTA_BASE_URL` (default the Cloudflare Worker URL).
- Move SoftAP SSID/PSK constants into `components/wifi_manager` as defaults.

**Files touched:** `partitions.csv`, `sdkconfig.defaults`, new `Kconfig.projbuild`, `components/wifi_manager/wifi_manager.c`, `components/wifi_manager/include/wifi_manager.h`.

**Done when:** `idf.py build` succeeds, partition table dump (`idf.py partition-table`) shows the new layout, factory provisioning still works.

---

## 9. Phase 1 — Recovery app

**Goal:** standalone bootable recovery image with a usable upload UI.

**Deliverables:**
- New project directory `esp32_gopro_canbus_recovery/` (sibling of main project).
- `CMakeLists.txt` referencing main project's `components/` via `EXTRA_COMPONENT_DIRS`.
- `sdkconfig.defaults` pointing at shared partition table via `"../esp32_gopro_canbus_controller_v2/partitions.csv"`.
- `main/main.c` — minimal: NVS, esp_netif, esp_event, `wifi_manager_init`, `recovery_http_init`.
- `main/recovery.html` — single-page, dependency-free, embedded via `EMBED_TXTFILES`.
- New `components/recovery_http/`:
  - `recovery_http.c` — handlers for `/`, `/api/version`, `/api/ota/upload-*`, `/api/ota/commit`, `/api/ota/boot-main`.
  - `ota_writer.c` — incremental SHA-256 + `esp_ota_*` writes.
  - `storage_writer.c` — incremental SHA-256 + `esp_partition_*` writes for `storage`.
  - `include/recovery_http.h` — exports `recovery_http_init()`.

**Files touched:** all new under `esp32_gopro_canbus_recovery/`.

**Done when:** flashing `recovery.bin` to `factory` and erasing `otadata` boots into recovery; SoftAP comes up with the same SSID as main; browsing to `http://192.168.4.1/` shows the upload page; uploading a valid `app.bin` + `storage.bin` then committing transitions cleanly to main app.

---

## 10. Phase 2 — Main app endpoints

**Goal:** main app can be updated in place via OTA, can switch channels, can reboot to recovery.

**Deliverables:**
- New `components/http_server/api_ota.c` with the endpoints from §6 (excluding recovery-only).
- Bump `config.max_uri_handlers` from 28 → 35 in `components/http_server/driver.c`.
- Extend `GET /api/version` to read `/www/manifest.json` and report `ui` field. Add `mode: "main"`.
- New NVS namespace `ota`, key `channel` (default `"stable"`).
- SHA-skip optimization: store last-written SHA per partition in NVS keys `app_sha`, `ui_sha`. Compare before each upload's first byte.

**Files touched:** new `components/http_server/api_ota.c`, edits to `components/http_server/driver.c`, `components/http_server/api_system.c` (for extended `/api/version`).

**Done when:** all endpoints from §6 respond correctly. Channel switch persists across reboots. SHA-skip avoids rewriting unchanged partitions. "Reboot to recovery" puts the device into recovery mode.

---

## 11. Phase 3 — Health check & dev workflow

**Goal:** make rollback work reliably, and make the daily dev loop fast.

**Deliverables:**
- `main.c` change: 30-second one-shot `esp_timer` after `http_server_init()` returns; on fire, calls `esp_ota_mark_app_valid_cancel_rollback()`. (If the function returns `ESP_ERR_NOT_SUPPORTED`, that means the running partition is `factory` — no-op, ignore.)
- `dev.ps1` in repo root with subcommands `build`, `flash` (OTA, default), `flash-usb`, `monitor`, `all`. Source in Appendix B.
- `tools/flash_factory.ps1` — one-shot USB provisioning. Source in Appendix C.

**Files touched:** `main/main.c`, new `dev.ps1`, new `tools/flash_factory.ps1`.

**Done when:** deliberately broken `app.bin` (panic in `app_main`) reverts to recovery automatically. `.\dev.ps1` performs full build + OTA-flash + monitor cycle.

---

## 12. Phase 4 — Cloud + CI + Worker

**Goal:** publishing a release moves bytes into a place the browser can fetch.

**Deliverables:**
- Cloudflare Worker `firmware-proxy/` deployed via `wrangler deploy`. Source in Appendix A.
- `tools/release/make_manifest.py`: runs `idf.py build` for both projects (or just main, if recovery unchanged), hashes `app.bin` and `storage.bin`, emits `manifest.json` per §5.
- `.github/workflows/release.yml`:
  - Triggered on tags `v[0-9]+.[0-9]+.[0-9]+` (stable) and `v[0-9]+.[0-9]+.[0-9]+-{rc,beta}.*` (beta).
  - Builds, hashes, generates manifest, attaches `app.bin`, `storage.bin`, `manifest.json` to a GitHub Release.
  - Moves the floating tag (`latest-stable` or `latest-beta`) to the new release.

**Files touched:** new `firmware-proxy/` (separate repo or subdirectory), new `tools/release/make_manifest.py`, new `.github/workflows/release.yml`.

**Done when:** pushing `v0.1.0-rc.1` results in `https://firmware-proxy.<account>.workers.dev/<owner>/<repo>/releases/download/latest-beta/manifest.json` returning the expected JSON with CORS headers.

---

## 13. Phase 5 — Web UI

**Goal:** "Check for Updates" button works end-to-end.

**Deliverables:**
- New "Updates" panel in `web_ui/index.html`:
  - Current `app` / `ui` / `recovery` versions and channel.
  - Channel selector (Stable / Beta) — POSTs to `/api/ota/channel`.
  - "Check for updates" button — fetches manifest, compares semver, displays delta.
  - "Install" button — downloads both blobs from cloud, POSTs to device with progress bar (`XHR.upload.onprogress`), POSTs `/api/ota/commit`, polls `/api/version` until new version answers.
  - Downgrade and channel-switch warnings.
  - Offline-fallback message.
- New `web_ui/updates.js` to keep `app.js` from bloating.
- Build-time addition: `compress.py` writes `manifest.json` into the staging dir (`{ "ui_version": "<version>" }`) so `/api/version` can report it.

**Files touched:** `web_ui/index.html`, `web_ui/style.css`, new `web_ui/updates.js`, `web_ui/compress.py`.

**Done when:** clicking "Check for updates" with a newer version on the cloud surfaces it; clicking "Install" downloads, uploads, commits, reboots, and the UI confirms the new version came up. Offline / mismatched / corrupted cases display sensible errors.

---

## 14. Phase 6 — Integration tests

**Goal:** catch the failure modes from §7 before users do. This is the acceptance gate.

**Acceptance checklist:**

- [ ] Fresh-board flash via `flash_factory.ps1` → main app boots, web UI loads.
- [ ] OTA upgrade from `dev.ps1` → boots new version, version field reflects it.
- [ ] **Rollback test:** OTA-flash a deliberately-bad `app.bin` (e.g., `while(1);` in `app_main` before `mark_app_valid_cancel_rollback()`) → bootloader reverts to factory after 30 s.
- [ ] From recovery web UI, OTA-upload working `app.bin` → boots into main app.
- [ ] Corrupt one byte of an upload → device returns 422, commit refuses, no boot partition change.
- [ ] Channel switch beta → stable when stable is older → downgrade warning shown, then installs cleanly.
- [ ] Cloudflare Worker proxy serves manifest with `Access-Control-Allow-Origin: *` (verified via browser DevTools, not just curl).
- [ ] Power-cycle mid-upload → device boots normally, partial upload discarded, user can retry.
- [ ] SHA-skip works: re-installing the same version reports `skipped: true` for both partitions and skips the write.

---

## 15. Future TODOs

Out of scope for the initial implementation. Listed here so they're not forgotten.

- **Hardware force-recovery button.** Free GPIO + button + boot-time check → `esp_ota_set_boot_partition(factory)`. Closes the last brick scenario (main app marks valid then crashes). Requires hardware revision.
- **`ota_1` slot for true A/B updates.** Free space at `0x590000` is reserved. Adding it is a partition-table change plus minor firmware tweaks (`esp_ota_get_next_update_partition` already handles multi-slot).
- **Ed25519 signing of manifests.** Additive — older firmware ignores new `sig` field. Add if threat model changes (e.g., open CAN bus on a vehicle).
- **Anti-rollback eFuse counter.** Once release format stabilizes. One-way operation, defer until confident.
- **Telemetry / phone-home update checks.** Requires AP+STA mode on the ESP32, which has tradeoffs (channel sharing, credential storage). Defer indefinitely.
- **Secure Boot v2 + Flash Encryption.** Hardware root of trust. eFuse-based, one-way. Only if shipping commercial product with strict supply-chain requirements.
- **Delta updates.** Bandwidth saving. Not worth the complexity at this scale.
- **Multi-language UI.** Recovery's embedded HTML is currently English-only. Could template at build time if needed.

---

## Appendix A — Cloudflare Worker source

`firmware-proxy/src/index.js`:

```js
export default {
  async fetch(request) {
    const url = new URL(request.url);

    // CORS preflight
    if (request.method === "OPTIONS") {
      return new Response(null, {
        status: 204,
        headers: {
          "Access-Control-Allow-Origin": "*",
          "Access-Control-Allow-Methods": "GET, HEAD, OPTIONS",
          "Access-Control-Allow-Headers": "*",
          "Access-Control-Max-Age": "86400",
        },
      });
    }

    // Pass-through to github.com, preserving the path
    const target = "https://github.com" + url.pathname + url.search;
    const upstream = await fetch(target, {
      method: request.method,
      redirect: "follow",
    });

    const headers = new Headers(upstream.headers);
    headers.set("Access-Control-Allow-Origin", "*");
    headers.set("Access-Control-Expose-Headers",
                "Content-Length, Content-Type, ETag, Last-Modified");

    return new Response(upstream.body, {
      status: upstream.status,
      statusText: upstream.statusText,
      headers,
    });
  },
};
```

`firmware-proxy/wrangler.toml`:

```toml
name = "firmware-proxy"
main = "src/index.js"
compatibility_date = "2025-01-01"
```

Deploy:
```
wrangler login
wrangler deploy
```

---

## Appendix B — `dev.ps1`

```powershell
# dev.ps1 — daily dev wrapper for main app
param(
    [Parameter(Position=0)] [string]$cmd = "all",
    [string]$ip   = "192.168.4.1",
    [string]$port = "COM3"
)

$ErrorActionPreference = "Stop"
$proj = "$PSScriptRoot\esp32_gopro_canbus_controller_v2"
$bin  = "$proj\build\esp32_gopro_canbus_controller_v2.bin"
$ui   = "$proj\build\storage.bin"

function Build   { idf.py -C $proj build }
function Monitor { idf.py -C $proj -p $port monitor }

function Sha256OfFile($path) {
    return (Get-FileHash -Algorithm SHA256 $path).Hash.ToLower()
}

function FlashOta {
    $appSha = Sha256OfFile $bin
    $appSize = (Get-Item $bin).Length
    Write-Host "Uploading app.bin ($appSize bytes, sha256=$appSha) to $ip..."
    curl.exe --fail -X POST `
        -H "X-Sha256: $appSha" -H "X-Size: $appSize" `
        --data-binary "@$bin" "http://$ip/api/ota/upload-app"

    if (Test-Path $ui) {
        $uiSha = Sha256OfFile $ui
        $uiSize = (Get-Item $ui).Length
        Write-Host "Uploading storage.bin ($uiSize bytes, sha256=$uiSha)..."
        curl.exe --fail -X POST `
            -H "X-Sha256: $uiSha" -H "X-Size: $uiSize" `
            --data-binary "@$ui" "http://$ip/api/ota/upload-ui"
    }

    curl.exe --fail -X POST "http://$ip/api/ota/commit"
    Write-Host "Device rebooting into new firmware."
}

function FlashUsb {
    Build
    python -m esptool --chip esp32s3 -p $port -b 921600 `
        write_flash 0xA0000 $bin
}

switch ($cmd) {
    "build"     { Build }
    "flash"     { Build; FlashOta }
    "flash-usb" { FlashUsb }
    "monitor"   { Monitor }
    "all"       { Build; FlashOta; Monitor }
    default     { Write-Host "usage: .\dev.ps1 [build|flash|flash-usb|monitor|all]" }
}
```

---

## Appendix C — `tools\flash_factory.ps1`

```powershell
# tools/flash_factory.ps1 — one-shot USB provisioning of a fresh board
param(
    [string]$port = "COM3"
)

$ErrorActionPreference = "Stop"
$root = "$PSScriptRoot\.."
$main = "$root\esp32_gopro_canbus_controller_v2\build"
$rec  = "$root\esp32_gopro_canbus_recovery\build"

# Build both projects first
idf.py -C "$root\esp32_gopro_canbus_controller_v2" build
idf.py -C "$root\esp32_gopro_canbus_recovery" build

python -m esptool --chip esp32s3 -p $port -b 921600 write_flash `
    0x000000 "$main\bootloader\bootloader.bin" `
    0x008000 "$main\partition_table\partition-table.bin" `
    0x020000 "$rec\esp32_gopro_canbus_recovery.bin" `
    0x0A0000 "$main\esp32_gopro_canbus_controller_v2.bin" `
    0x290000 "$main\storage.bin"

# otadata is left blank — bootloader picks ota_0 because it's valid;
# falls back to factory (recovery) if ota_0 ever fails.
Write-Host "Factory provisioning complete. Power-cycle the board."
```

---

*End of document. Update §1 if decisions change. Tick boxes in §14 as integration tests pass.*
