# Architecture

How the ESP32 GoPro Controller is put together: repo layout, components, boot order, core affinity, network, and radio coexistence.

For deeper detail on individual subsystems, see the design docs under [`design/`](design/).

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
└── docs/
    ├── architecture.md             — this file
    ├── hardware.md
    ├── development.md
    ├── releases.md
    ├── http-api.md
    └── design/
        ├── camera-manager.md       — camera lifecycle / driver vtable design
        ├── ota.md                  — OTA system source of truth
        └── web-ui.md               — HTTP API contract + UI specification
```

The two apps are independent ESP-IDF projects with their own `CMakeLists.txt` and `sdkconfig.defaults`. Each uses `EXTRA_COMPONENT_DIRS=../../components` to pick up the top-level shared components.

---

## Main app

```
┌─────────────────────────────────────────────────────────────────┐
│                          app_main                               │
│  log_ring → NVS → netif → event loop → components → AP →        │
│             http_server                                         │
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

## Recovery app

A stripped-down ESP-IDF project that lives in the `factory` partition. Same `wifi_manager`-based SoftAP, embedded HTML page (no LittleFS dependency), accepts manual `.bin` uploads via web form, can also auto-update from the cloud using the same `latest-{channel}/manifest.json` flow as the main app. No camera control, no CAN, no BLE. Reachable by:

- bootloader fallback (bad main app or rollback fire),
- `POST /api/ota/reboot-recovery` from the main app,
- or USB reflash via `tools/flash_factory.ps1`.

## Core affinity

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
| [`wifi_manager`](../components/wifi_manager/README.md) | SoftAP, DHCP, MAC spoofing, station tracking. Both apps. |
| `ota_io` | OTA partition writer, storage writer, SHA-skip NVS, channel get/set. Both apps. |

### Main-app only (`apps/main/components/`)

| Component | Description |
|-----------|-------------|
| [`ble_core`](../apps/main/components/ble_core/README.md) | NimBLE scan/connect/encrypt/notify/bond management |
| [`camera_manager`](../apps/main/components/camera_manager) | Slot lifecycle, NVS records, driver vtable, mismatch correction |
| [`gopro/gopro_model.h`](../apps/main/components/gopro/gopro_model.h) | GoPro model capability helpers (`uses_rc_emulation`, `uses_ble_control`) |
| [`open_gopro_ble`](../apps/main/components/gopro/open_gopro_ble/README.md) | Discovery, pairing, BLE control driver for Hero 9+ |
| [`gopro_wifi_rc`](../apps/main/components/gopro/gopro_wifi_rc/README.md) | RC-emulation driver over WiFi (Hero 4) |
| [`can_manager`](../apps/main/components/can_manager/README.md) | TWAI node, four configurable channels (logging RX / camera-status TX / GPS UTC RX / shutdown RX — defaults 0x600/0x601/0x602/0x603), GPS UTC, NVS persistence (bitrate + channel IDs + tz + last UTC) |
| [`http_server`](../apps/main/components/http_server) | esp_httpd, LittleFS web UI, all `/api/` handlers, soft recovery fallback when LittleFS is empty |
| [`log_ring`](../apps/main/components/log_ring) | In-RAM diagnostic log ring buffer for user reports; vprintf hook, NVS-persisted enable toggle (default OFF). See [`design/log-capture.md`](design/log-capture.md) |
| [`shutdown_manager`](../apps/main/components/shutdown_manager) | Operator + CAN-0x603 triggered system shutdown. Owns the `IDLE → SHUTTING_DOWN → COMPLETE` state machine, spawns one per-slot task that issues stop-recording + sleep + BLE-terminate + teardown (5 s deadline per slot, slow cameras don't block completion). Exposes `shutdown_manager_is_active()` as the gate consulted by `can_manager` (drop 0x600, stop 0x601 TX), `ble_core` (suppress reconnects), and every action HTTP POST (503). See [`design/shutdown.md`](design/shutdown.md). |

### Recovery-app only (`apps/recovery/components/`)

| Component | Description |
|-----------|-------------|
| `recovery_http` | esp_httpd, embedded HTML page, manual + cloud OTA upload handlers, `/api/version`, `/api/ota/channel` |

---

## Boot Sequence (main app)

```c
app_main()
 0. log_ring_init()                          ← installs vprintf hook BEFORE NVS
 1. nvs_flash_init()
 1a. log_ring_load_persisted_enabled()       ← applies NVS toggle (default OFF)
 2. esp_netif_init()
 3. esp_event_loop_create_default()
 4. camera_manager_init()
 4a. shutdown_manager_init()                 ← before any subsystem that consults its gate
 5. gopro_wifi_rc_init()
 6. open_gopro_ble_init()                    ← registers is_shutdown_active w/ ble_core
 7. wifi_manager_set_callbacks(...)          ← before wifi_manager_init() so no events are lost
 8. wifi_manager_init()                      ← raises the SoftAP
 9. wifi_manager_wait_for_ap_ready()         ← AP beacon on-air before BLE engages coex
10. http_server_init()                       ← also: reboots to factory if /www/index.html is missing
11. mark_ota_valid()                         ← esp_ota_mark_app_valid_cancel_rollback(), httpd up = "healthy enough"
12. ble_core_init()                          ← starts NimBLE host; deferred until after the AP beacon
13. can_manager_register_callbacks(...)      ← on_shutdown_request → shutdown_manager_on_can_request
14. can_manager_init()
```

WiFi is brought fully up (`wifi_manager_init()` + `wait_for_ap_ready()`) *before* `ble_core_init()` starts the NimBLE controller: the ESP32 shares one antenna between WiFi and BLE, so giving the AP a clear window to get its beacon on air avoids the coex scheduler starving WiFi during BLE bring-up. The rollback is disarmed synchronously the moment `http_server_init()` returns — a listening HTTP server is taken as sufficient signal that the new app is healthy enough that the bootloader shouldn't revert to factory. (There is no soak timer; see [`design/ota.md`](design/ota.md) §11.)

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
