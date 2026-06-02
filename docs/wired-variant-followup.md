# Wired (USB) variant — follow-up notes

Scratchpad for the **future** wired/USB GoPro controller variant. This
document is **not** an active plan — the restructure work that makes a wired
variant cheap to add lives in `docs/multi-variant-restructure-plan.md`.
Preserved below are the design breadcrumbs accumulated before the wired
product was scoped out, so the eventual wired effort starts from context
rather than from scratch.

---

## Motivating concept

A second sibling product alongside the existing wireless controller:

| Product | Camera transport | Trigger | UI / OTA |
|---------|------------------|---------|----------|
| **wireless** (current) | BLE (Hero9+) + WiFi-RC (Hero2–Hero8), 4 cameras | CAN bus | SoftAP web UI + OTA |
| **wired** (future) | **USB** to a single Hero10+ (controller powers + controls it) | CAN bus | SoftAP web UI + OTA |

The wired product is mostly "the same controller with a different camera
driver and no BLE." After the restructure lands, the wired app is largely a
thin `apps/wired/main/main.c` (mirrors wireless's `app_main` minus BLE init,
plus USB host bring-up) plus two new variant-specific components
(`usb_host_net`, `gopro/gopro_usb`).

---

## What is already known about USB control

Open GoPro **wired control** is, under the hood, **IP networking over USB**:

- A Hero10+ connected over USB-C enumerates as a **USB network gadget**
  (CDC-ECM / NCM / RNDIS class). The camera runs a DHCP server and exposes
  the **same Open GoPro HTTP API** (port 8080) at a fixed per-serial IP
  (`172.2X.1XX.51`). You enable it with a control request
  (`GET /gopro/camera/control/wired_usb?p=1`) then issue the same
  `/gopro/...` start/stop/keep-alive/datetime calls.
- **Implication:** the *command layer* is essentially the existing Open
  GoPro HTTP path. The new part is bringing up a network interface over USB
  on the ESP32-S3 acting as USB Host.

### USB host networking — proven via CherryUSB

USB host → GoPro has been demonstrated on the ESP32-S3 using the
**[CherryUSB](https://github.com/cherry-embedded/CherryUSB)** stack — a
portable USB host/device stack with CDC-ECM/NCM/RNDIS host classes and an
ESP32-S3 port, used **instead of** ESP-IDF's native `usb_host` lib (whose
NCM/ECM *host* support is not first-class). This is the seed of the future
`usb_host_net` component.

### Camera-side scope

- **Hero10+ only.** Hero9 is explicitly excluded — confirmed broken: a
  camera firmware bug breaks USB shutter-start.
- A `gopro_model_supports_usb_control()` predicate on the existing
  `gopro_model.h` (Hero10+ allowlist) is the natural place to encode this.

---

## Open questions to close before building the wired app

These items remain unresolved and would be the first thing to nail down when
the wired effort kicks off.

### USB networking specifics
- [ ] Which network class the target Hero presents over CherryUSB (ECM vs
      NCM vs RNDIS).
- [ ] CherryUSB version/commit pinned for the build.
- [ ] ESP32-S3 board used for the PoC.
- [ ] How CherryUSB is vendored into the IDF build (managed component vs
      in-tree).

### lwIP integration
- [ ] CherryUSB host netif → DHCP client lease → TCP path works end to end.
- [ ] Timings captured: enumerate → link up → first HTTP command.
- [ ] Attach/detach (unplug/replug) behavior matches `wifi_manager`'s
      station-callback model.

### End-to-end HTTP over the link
- [ ] `GET` the camera info JSON.
- [ ] Enable wired control (`GET /gopro/camera/control/wired_usb?p=1`).
- [ ] One start + one stop via the standard Open GoPro endpoints.

### Power path
- [x] **Prototyping:** external powered USB hub (hub supplies VBUS;
      ESP32-S3 is host for data). Keeps the board's power budget out of the
      loop while firmware is built.
- [ ] **Production power path** — open. Decide: source 5 V VBUS on-board at
      the GoPro's worst-case draw (recording + charging can pull >1 A) vs
      an integrated powered-hub / external supply. Capture the BOM/hardware
      decision here when the prototype matures.

---

## Sketch of the wired app shape (once the restructure has landed)

### `apps/wired` skeleton (no camera yet)
- New ESP-IDF project mirroring `apps/wireless`'s CMake/sdkconfig, minus
  BLE (`CONFIG_BT_*` off → frees flash + RAM, and removes WiFi/BLE coex
  entirely since wired is WiFi-SoftAP-only).
- `app_main`: NVS → netif → SoftAP → `http_server` → mark-OTA-valid →
  **USB host bring-up** → CAN. (BLE + WiFi-RC steps removed.)
- Boots to the SoftAP web UI + OTA with a stub/no camera.

### `usb_host_net` component (NEW)
- Vendor CherryUSB into the wired app (managed component or in-tree).
  USB Host init, device-attach detection, CDC-ECM/NCM/RNDIS class bring-up,
  lwIP netif + DHCP client, link up/down events surfaced like
  `wifi_manager`'s station callbacks.

### `gopro/gopro_usb` driver (NEW)
- Implement `camera_driver_t` over the USB netif: enable wired control,
  start/stop/`get_recording_status`, datetime sync, sleep. Register against
  `cam_core` exactly as the BLE/RC drivers register against
  `camera_manager` today.
- "Discovery" = USB attach + camera-control-enable handshake → mark the
  single target ready (`*_on_camera_ready`). Unplug → not-ready.

### Wired web UI (shared UI, variant-gated)
- Keep one shared web UI. In the wired build, gate off the multi-camera
  list + pairing/add-camera/reorder; surface a single-camera panel
  (plugged / ready / recording + manual start/stop). All shared settings
  (CAN IDs, UTC, OTA channel, logs) stay common.
- Drive the difference off the product variant (a `/api/version`-style
  capability flag the UI reads, so the same `storage.bin` could in
  principle serve both — confirm whether to ship one UI image or
  build-time-trim per product).

---

## Partitions & memory (re-confirm when wired comes online)

- The shared `partitions.csv` should fit wired comfortably: no NimBLE
  frees significant flash + RAM, which the USB host + lwIP stack consume.
  Re-confirm with a real build's `size` output before committing.
- Confirm USB host + WiFi SoftAP coexistence has no surprising RAM
  pressure (no radio coex needed — wired has no BLE).

---

## Wired-specific open decisions

- [ ] **USB networking class** — record which class the target Hero
      presents over CherryUSB (ECM vs NCM vs RNDIS) and how CherryUSB is
      vendored into the IDF build.
- [ ] **Production power path** — on-board VBUS vs integrated/external
      supply, once the prototype matures.
- [ ] **Wired UI delivery** — one shared `storage.bin` with a runtime
      capability flag, or build-time-trimmed per product.
