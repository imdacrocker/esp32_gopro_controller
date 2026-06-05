# Wired (USB) variant — integration design

Status: **active plan** (supersedes the scratchpad in
`docs/wired-variant-followup.md`, which remains as the source of the original
design breadcrumbs and open hardware questions).

This document describes how the wired proof-of-concept in `apps/wired`
(currently a single-file USB-host + Open-GoPro-HTTP sketch) is folded into
the monorepo's shared-component architecture so it becomes a first-class
product variant alongside `apps/wireless`.

---

## 1. Goal

A second sibling product:

| Product | Camera transport | Trigger | UI / OTA |
|---------|------------------|---------|----------|
| **wireless** | BLE (Hero9+) + WiFi-RC (Hero2–Hero8), 4 cameras | CAN bus | SoftAP web UI + OTA |
| **wired** | **USB** to a single Hero10+ (CDC-NCM/RNDIS network gadget) | CAN bus | **SoftAP** web UI + OTA |

The wired controller is "the same controller with a different camera driver
and no BLE/WiFi-RC." It keeps the **WiFi SoftAP** purely for the web UI and
browser OTA (decision recorded 2026-06-04). The SoftAP and the USB link are
two independent network interfaces:

- **USB netif** — the ESP32-S3 is a USB *host*; the Hero is a USB network
  *gadget* running its own DHCP *server*. The ESP is a DHCP *client* here and
  derives the camera IP (`.51`) from its lease.
- **WiFi SoftAP netif** — the ESP runs a SoftAP with its own DHCP *server*
  for the phone/laptop that loads the web UI. No station/camera traffic.

There is **no BLE and no WiFi-RC** in the wired variant, so there is also no
radio coex concern (the wireless app's WiFi-before-BLE ordering dance does
not apply).

---

## 2. Architecture — what is reused vs. new

The repo splits each subsystem into a **shared core** component (under
`/components`, transport-agnostic) and a **variant adapter** component (under
`apps/<variant>/components`). The wired variant reuses every shared core
component unchanged and adds two small wired-specific components.

### Reused as-is (shared `/components`)

| Component | Role in wired |
|-----------|---------------|
| `cam_core` | Recording-intent engine + `camera_driver_t` vtable + CAN-state translation. The USB driver registers a single slot here. |
| `can_manager` | 0x600 logging / 0x602 UTC / 0x603 shutdown RX, 0x601 status TX. Identical. |
| `wifi_manager` | SoftAP bring-up + DHCP server for the UI. Station callbacks are **not** wired (no cameras join the AP). |
| `http_server_core` | LittleFS mount, httpd, shared `/api/*` (version, ota, settings, logs, shutdown, system). |
| `log_ring` | 64 KB diagnostic ring. Identical. |
| `shutdown_manager` | Operator/CAN shutdown. The USB driver's `sleep` vtable entry participates. |
| `ota_io` | OTA writer / boot helpers. Identical. |

### New, wired-specific (`apps/wired/components/`)

| Component | Mirrors (wireless) | Responsibility |
|-----------|--------------------|----------------|
| `http_server_wired` | `http_server_wireless` | Calls `http_server_core_start()`, then registers wired-only endpoints (single-camera status, manual start/stop). |
| `usb_host_net` | (none — replaces `ble_core`/station plumbing) | CherryUSB host bring-up, device attach/detach, CDC-NCM/RNDIS class, lwIP netif + DHCP client, camera-IP derivation. Surfaces link-up/down + camera-IP via callbacks modeled on `wifi_manager`'s station callbacks. |
| `gopro/gopro_usb` | `gopro_wifi_rc` / `open_gopro_ble` | Implements `camera_driver_t` over the Open GoPro HTTP path. Enables wired control, start/stop/status/datetime/sleep. Registers one slot with `cam_core`. |

`gopro_model.h` (the `gopro_model_uses_usb_control()` Hero10+ allowlist) lives
in the shared `components/gopro_model/` component; both the wireless gopro
drivers and the wired `gopro_usb` driver `REQUIRES gopro_model`.

---

## 3. Boot composition (`apps/wired/main/main.c`)

Mirrors the wireless `app_main` minus BLE/RC, plus USB host bring-up. **Key
difference:** the wireless app calls `cam_core_init()` *inside*
`camera_manager_init()`; the wired app has no camera_manager_wireless, so it
must call `cam_core_init()` **directly** (before `shutdown_manager_init()` and
`can_manager_init()`, both of which depend on cam_core being up).

```
app_main():
  log_ring_init()                         # before everything — capture pre-NVS boot logs
  nvs_flash_init() (+erase/retry)
  log_ring_load_persisted_enabled()
  esp_netif_init()
  esp_event_loop_create_default()

  cam_core_init()                         # DIRECT call (no camera_manager in wired)
  shutdown_manager_init()                 # after cam_core, before can callbacks

  wifi_manager_init()                     # SoftAP for the UI (no set_callbacks)
  wifi_manager_wait_for_ap_ready()

  http_server_wired_init()                # http_server_core_start + wired endpoints
  mark_ota_valid()                        # httpd up = healthy enough; disarm rollback

  usb_host_net_init()                     # CherryUSB host; fires link/camera-IP callbacks
  gopro_usb_init()                        # registers the single USB camera slot w/ cam_core

  can_manager_register_callbacks(...)     # on_utc_acquired -> gopro_usb_sync_time; on_shutdown_request
  can_manager_init()
```

Notes:
- No `on_station_*` callbacks (no cameras on the SoftAP).
- `on_gps_utc_acquired` calls only the USB driver's time-sync entry.
- USB bring-up happens *after* httpd so the UI is reachable even with no
  camera plugged in.

---

## 4. Phased delivery

Each phase is independently buildable + flashable.

### Phase 0 — Wire the build to the shared tree *(this pass)*
- `apps/wired/CMakeLists.txt`: add
  `list(APPEND EXTRA_COMPONENT_DIRS "../../components")` and
  `list(APPEND EXTRA_COMPONENT_DIRS "components")` (for wired-local adapters);
  add the web-UI LittleFS staging block (copied from wireless).
- `apps/wired/main/idf_component.yml`: add `joltwallet/littlefs` + `cjson`.
- Add `apps/wired/web_ui/` (initial copy of wireless's).
- **Decouple `shutdown_manager` from `open_gopro_ble`** (prerequisite — the
  shared component declared a wireless-only dependency that broke the wired
  build). Added a nullable `terminate_link(void *ctx)` entry to
  `camera_driver_t`, a `cam_core_invoke_terminate_link(idx)` wrapper, and
  pointed the BLE driver's vtable at a new `drv_terminate_link`.
  `shutdown_manager` now depends only on `cam_core`. This was the documented
  follow-up from the multi-variant restructure.
- Outcome: build still compiles the PoC; shared components are now visible.

### Phase 1 — Boot skeleton on shared infra, no camera *(this pass)*
- Rewrite `apps/wired/main/main.c` to the §3 composition, **minus**
  `usb_host_net_init()` / `gopro_usb_init()` (stubbed/absent this phase).
- Create `apps/wired/components/http_server_wired` (initially just forwards to
  `http_server_core_start()`, registers nothing extra).
- Outcome: wired board boots to SoftAP web UI + OTA + CAN status TX/RX with
  **no camera**. Proves the shared-component wiring end to end.

### Phase 2 — `usb_host_net` component *(done)*
- Lifted the CherryUSB host bring-up + camera-IP derivation out of the PoC
  `main.c` into `apps/wired/components/usb_host_net`, exposing a
  `wifi_manager`-style `usb_host_net_link_cb_t(up, camera_ip)` callback plus
  `usb_host_net_is_up()` / `usb_host_net_camera_ip()` accessors.
- **Key realization:** the esp_netif + DHCP-client + USB↔lwIP frame pump is
  all done *inside* CherryUSB (`platform/idf/usbh_net.c`, compiled because
  `CONFIG_CHERRYUSB_HOST_CDC_NCM/_CDC_RNDIS=y` → `cherryusb.cmake` adds it and
  the Kconfig selects `USBHOST_PLATFORM_CDC_*`). The class connect handler
  calls `usbh_cdc_ncm_run()`/`usbh_rndis_run()`, which create an `eth`-type
  netif and start DHCP — so the camera lease arrives as `IP_EVENT_ETH_GOT_IP`.
  `usb_host_net` therefore owns only: `usbh_initialize()`, the IP-event →
  camera-IP (`.51`) derivation (link up), and `USBH_EVENT_DEVICE_DISCONNECTED`
  (link down).
- `main.c` calls `usb_host_net_init(on_usb_link)` after httpd; `on_usb_link`
  currently just logs (Phase 3 forwards to the driver).

### Phase 3 — `gopro_usb` driver *(done)*
- `apps/wired/components/gopro_usb` implements `camera_driver_t` over the Open
  GoPro HTTP API and registers a single `cam_core` slot (slot 0) directly
  (`cam_core_register_slot` + `cam_core_slot_attach_driver`) — no
  `camera_manager` in the wired variant.
- Threading mirrors `gopro_wifi_rc`: vtable `start`/`stop` post to a queue and
  set an optimistic cached status; `get_recording_status` returns the cache;
  all blocking HTTP + `cam_core_slot_set_ready` run on a dedicated worker task.
  A 3 s timer drives `keep_alive` + a `/gopro/camera/state` poll that refreshes
  the cache (status field `"10"` = encoding).
- Link lifecycle: `on_usb_link(true,ip)` → worker enables wired control
  (`/gopro/camera/control/wired_usb?p=1`), reads `/gopro/camera/info` to log +
  gate the model, probes state, then `cam_core_slot_set_ready(true)`.
  `on_usb_link(false)` → `set_ready(false)`.
- **CAN + UI drive the camera for free:** the shared `can_manager` already maps
  the 0x600 logging frame to `cam_core_set_desired_all(START/STOP)` (gated by
  auto-control), and `cam_core_get_can_state(0)` feeds the 0x601 status TX. No
  wired-specific CAN wiring beyond `can_manager_init()`.
- DateTime sync: `gopro_usb_sync_time_all()` (from `on_gps_utc_acquired`)
  reads the CAN UTC anchor + tz offset and issues `/gopro/camera/set_date_time`.
- **Deferred / TODO:**
  - `sleep` vtable is NULL — Open GoPro HTTP has no reliable power-off and the
    camera is bus-powered (loses power at system shutdown). `shutdown_manager`
    budgets a NULL sleep as a no-op.
  - ~~The USB-control model gate is a local predicate in `gopro_usb.c`; the
    canonical home is `gopro_model.h`.~~ **done:** `gopro_model.{h,c}` were
    promoted into the shared `components/gopro_model/` component and
    `gopro_usb.c` now calls the canonical `gopro_model_uses_usb_control()`.
  - Built but **not yet verified on hardware** — the HTTP endpoint set is the
    modern Open GoPro path (Hero10+), not the PoC's legacy `/gp/gpControl` one.

### Phase 4 — Wired web UI + endpoints *(done)*
- **Reused the shared UI contract rather than forking it.** `http_server_wired`
  now serves the *same* `/api/paired-cameras` (one-element array) and
  `/api/shutter` endpoints as the wireless variant
  (`apps/wired/components/http_server_wired/api_camera_wired.c`), sourced from
  `cam_core` slot 0 + `usb_host_net` + `gopro_usb_get_model_name()`. The
  existing `renderCameraCards` path therefore renders the USB camera unchanged.
- Status mapping: `cam_core_get_can_state(0)` → recording/idle; otherwise
  `usb_host_net_is_up()` → "connecting" (handshake in flight) vs "disconnected".
- UI gating: `app.js` probes `/api/version`; when `product === "wired"` it
  hides the "Add / Manage Cameras" bar (no pairing/scan/reorder) and relabels
  the manual control buttons to "Record"/"Stop". Everything else (auto-control
  toggle, per-camera shutter button, CAN/UTC/OTA/logs/power settings) is the
  shared UI as-is. The wired `storage.bin` ships its own copy of the web UI
  today, but the gating is product-driven so the assets could be unified later.
- Added `gopro_usb_get_model_name()` so the card shows the camera's
  self-reported model (from `/gopro/camera/info`).
- Pairing/scan/RC endpoints are intentionally NOT served on wired; they are
  unreachable from the UI once the manage bar is hidden.

---

## 5. Component contracts (wired-specific, for Phases 2–3)

### `usb_host_net` (sketch)
```c
typedef void (*usb_host_net_link_cb_t)(bool up, uint32_t camera_ip /*0 when down*/);
void usb_host_net_init(usb_host_net_link_cb_t on_link);
uint32_t usb_host_net_camera_ip(void);   /* 0 when no link */
```

### `gopro_usb` (sketch — mirrors gopro_wifi_rc's init + sync entries)
```c
void      gopro_usb_init(void);              /* registers driver + single slot */
void      gopro_usb_sync_time_all(void);     /* called from on_gps_utc_acquired */
/* internally implements camera_driver_t: start/stop/get_recording_status/sleep */
```

---

## 6. Open decisions (carried from the followup doc)

- **USB networking class** — confirm NCM vs RNDIS the target Hero presents;
  `sdkconfig.defaults` currently enables both host classes.
- **Production power path** — on-board VBUS vs integrated/external supply.
- **Wired UI delivery** — one shared `storage.bin` + runtime capability flag
  (current intent) vs build-time trim.
- **Partitions/RAM** — re-confirm with a real `size` build that USB host +
  SoftAP fit the shared `partitions.csv`.

---

## 7. Cleanup noted during integration

- ~~`apps/wired/sdkconfig.defaults` socket-budget comment still references
  "gopro_rc UDP" and "COHN TLS" sockets copied from wireless~~ — **done:**
  recomputed to `CONFIG_LWIP_MAX_SOCKETS=12` = 8 httpd + 3 httpd-internal + 1
  `gopro_usb` esp_http_client. The SoftAP DHCP server and USB-netif DHCP client
  use raw lwIP UDP PCBs (`udp_new`) and do NOT count against this limit.
- ~~**Promote `gopro_model.h`** (+ `gopro_model.c`'s `gopro_model_from_name`)
  into a shared component so the wired `gopro_usb` driver can use the canonical
  USB-control predicate instead of its local copy.~~ — **done:** now lives at
  `components/gopro_model/`; the canonical predicate is
  `gopro_model_uses_usb_control()`. Both variants' gopro components (and the
  `test_gopro_model` host test) consume it via the shared component.
- Decide whether `gopro_usb` should warm up the camera into a shooting mode on
  link-up (the PoC's legacy webcam-STOP + sub_mode dance) — only if Hero10+
  hardware testing shows the modern shutter path needs it.
