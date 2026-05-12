# ESP32 GoPro CAN-Bus Controller v2

An ESP32-S3 firmware that acts as a wireless RC controller for up to four GoPro cameras simultaneously. It presents itself as a GoPro WiFi Remote on both BLE and Wi-Fi, controls cameras over the Open GoPro API, and accepts control inputs from a CAN bus (e.g., a vehicle network or external button panel).

The repo is a monorepo holding two ESP-IDF projects — the **main app** and a much smaller **recovery app** — that share a partition table and a handful of components. Updates ship as a versioned pair (app + UI) via signed-by-hash OTA from GitHub Releases through a Cloudflare Worker proxy.

---

## Repository Layout

```
esp32_gopro_controller/
├── apps/
│   ├── main/                       — primary firmware (Hero control, web UI, CAN, OTA)
│   │   ├── components/             — main-app-only components
│   │   ├── web_ui/                 — index.html, app.js, style.css, updates.js, compress.py
│   │   ├── main/                   — app_main + Kconfig.projbuild
│   │   └── sdkconfig.defaults
│   └── recovery/                   — recovery firmware (factory partition; manual + cloud OTA upload)
│       ├── components/recovery_http
│       ├── main/                   — app_main + embedded recovery.html
│       └── sdkconfig.defaults
├── components/                     — shared between both apps
│   ├── wifi_manager                — SoftAP, DHCP, station tracking (used by both)
│   └── ota_io                      — partition writer, SHA-skip, channel NVS (used by both)
├── tools/
│   ├── firmware-proxy/             — Cloudflare Worker (deployed via wrangler)
│   └── release/make_manifest.py    — hashes built binaries into a release manifest
├── .github/workflows/
│   ├── release-beta.yml            — cuts v$VERSION, moves latest-beta pointer
│   └── release-promote.yml         — pointer-move promote: latest-stable → existing tag
├── partitions.csv                  — shared between both apps (`../../partitions.csv`)
├── VERSION                         — next planned release version line
├── dev.ps1                         — daily-dev wrapper: build + OTA-flash + monitor
├── tools/flash_factory.ps1         — full-board USB provisioning
├── ota_design.md                   — OTA system source of truth
├── camera_manager_design.md        — camera lifecycle / driver vtable design
└── web_ui_spec.md                  — HTTP API contract
```

The two apps are independent ESP-IDF projects with their own `CMakeLists.txt` and `sdkconfig.defaults`. Each uses `EXTRA_COMPONENT_DIRS=../../components` to pick up the top-level shared components.

---

## Hardware

| Item | Detail |
|------|--------|
| SoC | ESP32-S3 |
| Flash | 8 MB |
| Partition table | Custom (`partitions.csv` at repo root) — both apps share it |
| CAN transceiver | TWAI-compatible (wired to ESP32-S3 TWAI peripheral) |

### Partition layout (8 MB, A/B updates enabled)

| Partition | Type | Offset | Size | Contents |
|---|---|---|---|---|
| `nvs` | data | `0x009000` | 24 KB | NVS (camera slots, OTA state, bonds) |
| `otadata` | data | `0x00F000` | 8 KB | which OTA slot is active |
| `phy_init` | data | `0x011000` | 4 KB | PHY init data |
| `factory` | app | `0x020000` | 768 KB | **recovery app** (always-present fallback) |
| `ota_0` | app | `0x0E0000` | 1.69 MB | main app slot A |
| `storage` | data | `0x290000` | 3 MB | LittleFS (web UI for main app) |
| `ota_1` | app | `0x590000` | 1.69 MB | main app slot B |
| *(free)* | | `0x740000` | 768 KB | reserved |

Bootloader picks the most recent valid OTA slot; falls back to `factory` on bad image or 30 s rollback timer. See `ota_design.md` §3 for the full table and §6 for the boot/upgrade flow.

---

## Architecture Overview

### Main app

```
┌─────────────────────────────────────────────────────────────────┐
│                          app_main                               │
│  NVS → netif → event loop → components → AP → http_server      │
│  (30 s rollback timer disarmed once http_server is up)          │
└────────┬──────────────────────────┬────────────────────────────┘
         │                          │
  ┌──────▼──────┐            ┌──────▼──────┐
  │  ble_core   │            │ wifi_manager│
  │  (core 1)   │            │  (core 0)   │
  └──────┬──────┘            └──────┬──────┘
         │                          │
  ┌──────▼──────┐            ┌──────▼──────┐
  │open_gopro_  │            │gopro_wifi_rc│
  │    ble      │            │  (Hero 4)   │
  │ (Hero 9+)   │            │             │
  └──────┬──────┘            └──────┬──────┘
         │                          │
  ┌──────▼──────────────────────────▼──────┐
  │             camera_manager              │
  └─────────────────┬───────────────────────┘
                    │
             ┌──────▼──────┐
             │ can_manager │
             └─────────────┘

┌──────────────────────────────────────────────────┐
│                  http_server                      │
│  esp_httpd · LittleFS /www · all /api/ handlers  │
│  (sits at the top of the dependency graph)       │
└──────────────────────────────────────────────────┘
```

### Recovery app

A stripped-down ESP-IDF project that lives in the `factory` partition. Same `wifi_manager`-based SoftAP, embedded HTML page (no LittleFS dependency), accepts manual `.bin` uploads via web form, can also auto-update from the cloud using the same `latest-{channel}/manifest.json` flow as the main app. No camera control, no CAN, no BLE. Reachable by:
- bootloader fallback (bad main app or rollback fire),
- `POST /api/ota/reboot-recovery` from the main app,
- or USB reflash via `tools/flash_factory.ps1`.

### Core affinity

| Core | Tasks |
|------|-------|
| Core 0 | WiFi, `esp_timer`, HTTP handlers |
| Core 1 | NimBLE host, BT controller, `can_rx` |

BLE and WiFi are pinned to opposite cores to reduce cache thrashing and radio coexistence latency.

---

## Components

### Shared (top-level `components/`)

| Component | Description |
|-----------|-------------|
| [`wifi_manager`](components/wifi_manager/README.md) | SoftAP, DHCP, MAC spoofing, station tracking. Both apps. |
| `ota_io` | OTA partition writer, storage writer, SHA-skip NVS, channel get/set. Both apps. |

### Main-app only (`apps/main/components/`)

| Component | Description |
|-----------|-------------|
| [`ble_core`](apps/main/components/ble_core/README.md) | NimBLE scan/connect/encrypt/notify/bond management |
| [`camera_manager`](apps/main/components/camera_manager) | Slot lifecycle, NVS records, driver vtable, mismatch correction |
| [`gopro/gopro_model.h`](apps/main/components/gopro/gopro_model.h) | GoPro model capability helpers (`uses_rc_emulation`, `uses_ble_control`) |
| [`open_gopro_ble`](apps/main/components/gopro/open_gopro_ble/README.md) | Discovery, pairing, BLE control driver for Hero 9+ |
| [`gopro_wifi_rc`](apps/main/components/gopro/gopro_wifi_rc/README.md) | RC-emulation driver over WiFi (Hero 4) |
| [`can_manager`](apps/main/components/can_manager/README.md) | TWAI node, 0x600/0x602 RX, 0x601 TX at 5 Hz, GPS UTC, NVS persistence |
| [`http_server`](apps/main/components/http_server) | esp_httpd, LittleFS web UI, all `/api/` handlers |

### Recovery-app only (`apps/recovery/components/`)

| Component | Description |
|-----------|-------------|
| `recovery_http` | esp_httpd, embedded HTML page, manual + cloud OTA upload handlers, `/api/version`, `/api/ota/channel` |

---

## Boot Sequence (main app)

```c
app_main()
 1. nvs_flash_init()
 2. esp_netif_init()
 3. esp_event_loop_create_default()
 4. camera_manager_init()
 5. gopro_wifi_rc_init()
 6. open_gopro_ble_init()
 7. ble_core_init()
 8. can_manager_register_callbacks(...)
 9. can_manager_init()
10. wifi_manager_set_callbacks(...)
11. wifi_manager_init()
12. wifi_manager_wait_for_ap_ready()
13. http_server_init()
14. arm 30 s one-shot timer → esp_ota_mark_app_valid_cancel_rollback()
```

`ble_core_init()` and `wifi_manager_init()` overlap intentionally: NimBLE startup is asynchronous, so the AP can be raised while the host comes up. The rollback timer disarms only after the HTTP server has been listening for 30 s — sufficient signal that the new app is healthy enough that the bootloader shouldn't revert to factory.

---

## Getting Started

### Prerequisites

- ESP-IDF v6.0.1 (the project pins this version explicitly in the workflows)
- Target: `esp32s3`
- PowerShell (`dev.ps1` and `flash_factory.ps1` are PS scripts)
- `curl.exe` (ships with Windows 10/11) for the OTA upload helpers

Source the IDF environment once per shell:

```powershell
& 'C:\esp\v6.0.1\esp-idf\export.ps1'
```

### Daily dev — `dev.ps1`

`dev.ps1` lives at the repo root and wraps build + OTA-flash + monitor. Run from the repo root:

```powershell
.\dev.ps1                          # build + OTA-flash + monitor (main app)
.\dev.ps1 build                    # build only
.\dev.ps1 flash                    # build + OTA-flash (no monitor)
.\dev.ps1 flash-usb                # build + USB-flash to running slot
.\dev.ps1 monitor                  # idf.py monitor

.\dev.ps1 -App recovery build      # build the recovery app
.\dev.ps1 -App recovery flash-usb  # USB-flash recovery to the factory slot

.\dev.ps1 -ip 10.71.79.1 ...       # override target IP (default = SoftAP IP)
.\dev.ps1 -port COM7 ...           # override serial port (default COM3)
```

The OTA path posts to `http://10.71.79.1/api/ota/{upload-app,upload-ui,commit}` — your laptop must be associated to the `HERO-RC-XXXX` SoftAP first. OTA is main-app only (recovery has no `storage.bin`).

### First flash from blank — `flash_factory.ps1`

For a brand-new board, or after `python -m esptool erase_flash`, use the full provisioning script:

```powershell
.\tools\flash_factory.ps1
```

This builds both apps and writes everything over USB at correct offsets: bootloader, partition table, `ota_data_initial` (so the bootloader picks `ota_0` immediately), recovery into `factory`, main app into `ota_0`, and the LittleFS web UI into `storage`. `ota_1` is left blank for the first OTA to populate.

### Build manually (without dev.ps1)

Each app builds with `idf.py` like a normal ESP-IDF project:

```powershell
cd apps\main
idf.py build

cd ..\recovery
idf.py build
```

After any `sdkconfig.defaults` change, delete the live `sdkconfig` so the new defaults are picked up.

---

## Releases & OTA

The full design is in [`ota_design.md`](ota_design.md). High-level model:

- **Channels:** `stable` and `beta` published online (separate Cloudflare Worker routes). `dev` is local-only via `dev.ps1`.
- **One binary per version.** Beta and stable use the byte-identical binary for the same `vX.Y.Z` tag — promotion is a pure pointer-move on the `latest-stable` floating tag, not a rebuild.
- **App + UI ship as a pair.** Each release publishes 4 assets: `manifest.json` (declares both SHA-256 hashes), `app.bin`, `storage.bin` (LittleFS UI image), and `recovery.bin` (used only for manual `flash_factory.ps1` reflash, not browser OTA).
- **Browser is the only outbound proxy.** ESP32 stays a SoftAP; the user's phone/laptop fetches signed manifests + blobs from the Cloudflare Worker and POSTs them to the device. CORS is added by the Worker (GitHub Releases doesn't ship CORS headers).

### Cutting a release

1. Bump the root `VERSION` file (e.g. `0.2.1` → `0.2.2`), commit, push.
2. *(only if recovery code changed)* Bump `CONFIG_APP_PROJECT_VER` in `apps/recovery/sdkconfig.defaults` independently.
3. **Actions → release-beta → Run workflow.** Builds both apps, publishes `v$VERSION` as prerelease, moves `latest-beta` floating tag.
4. Test on a device: channel = beta → Check for updates → Install. Validates the new bytes.
5. **Actions → release-promote → Run workflow** with `tag = v$VERSION`. Flips the source release out of prerelease and moves `latest-stable` to the same bytes (no rebuild).
6. Bump `VERSION` to the next line (e.g. `0.2.3`) so subsequent betas don't collide with the released stable.

### Dev-channel version marker

Local builds on `main` carry `CONFIG_APP_PROJECT_VER="0.1.1-dev"` so it's obvious when the device is running an uncommitted/unstamped binary. CI workflows rewrite this line at build time from the tag, so `-dev` never leaks into published releases. Every `idf.py build` updates the `build_date`/`build_time` fields in `/api/version`, surfaced as a "Built" row in the Updates panel — that's how you confirm a flash actually took.

---

## Web UI

Two separate UIs, one per app:

- **Main app:** [`apps/main/web_ui/`](apps/main/web_ui/) — `index.html`, `style.css`, `app.js`, `updates.js`. Compressed by `compress.py` at build time and packed into the `storage` LittleFS partition. Served from `http://10.71.79.1/`.
- **Recovery app:** [`apps/recovery/main/recovery.html`](apps/recovery/main/recovery.html) — single embedded HTML page (no LittleFS dependency), served from the same address when recovery is the running app.

After editing main-app web files:

```powershell
.\dev.ps1 flash    # picks up storage.bin changes via OTA
```

On first boot with a blank or corrupted storage partition, LittleFS formats automatically. The browser will show a placeholder page until the storage partition is flashed.

---

## Network Configuration

The SoftAP uses a fixed address scheme so HTTP connections to cameras are predictable without DNS:

| Item | Value |
|------|-------|
| AP IP | `10.71.79.1` |
| Subnet | `10.71.79.0/24` |
| DHCP pool | `10.71.79.2` – `10.71.79.50` |
| SSID | `HERO-RC-XXXXXX` (last 3 MAC bytes) |
| Auth | Open (no password) |
| MAC OUI | `d8:96:85` (GoPro WiFi Remote OUI) |
| Channel | 11 (2462 MHz), HT20 |

The OUI spoof causes GoPro cameras to treat this device as a known remote type during pairing. SSID/channel/auth are shared between main and recovery so reconnect-and-reload survives a recovery round-trip.

---

## Radio Coexistence

The firmware uses Espressif's software coexistence (`CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE`). Additional runtime measures:

- WiFi AP fixed to **channel 11** (2462 MHz) — clear of BLE advertising channels 37 (2402 MHz), 38 (2426 MHz), 39 (2480 MHz).
- WiFi bandwidth forced to **HT20** to avoid the upper half of a 40 MHz channel overlapping BLE.
- WiFi power-save disabled (`WIFI_PS_NONE`) so the radio stays active during BLE scan windows.

---

## HTTP API

The web UI is served from `http://10.71.79.1/`. All API endpoints return `application/json`.

### System

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/version` | `{app, ui, recovery, channel, running_partition, mode, build_date, build_time, ota_base_url, ota_repo_path}` |
| GET | `/api/logging-state` | RaceCapture logging state (`logging` / `not_logging` / `unknown`) |
| GET | `/api/utc` | UTC timestamp (TZ-applied); `valid` for any anchor incl. NVS-restored, `session_synced` for live-source-this-boot |
| GET | `/api/auto-control` | Whether cameras follow CAN logging state automatically |
| POST | `/api/auto-control` | Set auto-control on/off |
| POST | `/api/reboot` | Restart the ESP32 |

### Cameras

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/paired-cameras` | All configured camera slots with connection + recording status |
| POST | `/api/shutter` | Start or stop recording (all cameras or a specific slot) |
| POST | `/api/remove-camera` | Remove a paired camera (compacts slot array) |
| POST | `/api/reorder-cameras` | Reorder camera slots (cameras must be disconnected first) |
| GET | `/api/cameras` | BLE scan results (GoPro cameras advertising `0xFEA6`) |
| POST | `/api/scan` | Start BLE discovery scan |
| POST | `/api/scan-cancel` | Cancel BLE discovery scan |
| POST | `/api/pair` | Initiate BLE pairing with a discovered camera |
| GET | `/api/pair/status` | Poll BLE pairing progress |
| POST | `/api/pair/cancel` | Abort an in-progress BLE pairing |

### RC-emulation (Hero 4)

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/rc/discovered` | SoftAP stations matching the GoPro OUI allow-list, not yet registered as RC cameras |
| POST | `/api/rc/add` | Register a SoftAP station as an RC-emulation (Hero 4) camera |

### Settings

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/settings/timezone` | Current UTC offset in whole hours |
| POST | `/api/settings/timezone` | Set UTC offset (`−12` to `+14`) |
| POST | `/api/settings/datetime` | Set system time manually (only when no live source has won this boot session) |

### OTA

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/api/ota/upload-app` | Stream new app image into inactive OTA slot (`X-Sha256`, `X-Size` headers; SHA-skip if NVS-recorded hash matches) |
| POST | `/api/ota/upload-ui` | Stream new `storage.bin` LittleFS image |
| POST | `/api/ota/commit` | Set boot partition + reboot. Returns `{rebooting, boot_partition}`; `rebooting:false` for UI-only updates |
| POST | `/api/ota/reboot-recovery` | Switch boot to factory + reboot (lands in recovery web UI) |
| GET | `/api/ota/channel` | `{current, available[]}` |
| POST | `/api/ota/channel` | Persist new channel into shared NVS |

Recovery exposes a subset (`/api/version`, `/api/ota/upload-app`, `upload-ui`, `commit`, `boot-main`, `channel`). Full request/response contracts: [`web_ui_spec.md`](web_ui_spec.md).

---

## License

See [LICENSE](LICENSE).
