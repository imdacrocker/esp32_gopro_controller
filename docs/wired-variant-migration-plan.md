# Wired (USB) Variant — Migration Plan

Living document for splitting the firmware into **two sibling products** that
share the bulk of their code:

| Product | Camera transport | Trigger | UI / OTA |
|---------|------------------|---------|----------|
| **wireless** (today's `apps/main`) | BLE (Hero9+) + WiFi-RC (Hero2–Hero8) | CAN bus | SoftAP web UI + OTA |
| **wired** (new `apps/wired`) | **USB** to a single Hero10+ (controller powers + controls it) | CAN bus | SoftAP web UI + OTA |

Both keep the CAN trigger, the SoftAP web UI, OTA, and a recovery app. The
recovery app stays functionally identical between products — only its OTA
source changes.

Status legend: `[ ]` todo · `[~]` in progress / partial · `[x]` done.
Severity / risk tags as needed. Check items off as they land.

> This plan is intentionally **sequenced so each phase leaves `main`/`wireless`
> shippable**. Nothing here requires a flag-day rewrite — the wireless product
> keeps building and working at every step until the wired product comes online
> beside it.

---

## 0. Guiding principles

1. **No behavior change to the wireless product** until its code has been
   moved/renamed and re-verified. Restructure first, add the new product
   second.
2. **Share by extraction, not by copy.** Every line the two products share
   lives in top-level `components/`. Transport-specific code stays in each
   app's `components/`.
3. **The driver vtable is the seam.** `camera_driver_t`
   (`camera_manager/include/camera_manager.h:13`) is already the abstraction
   that decouples "what to record" from "how to talk to the camera." The wired
   USB path becomes a third driver behind the same vtable.
4. **De-risk the genuinely-new hardware/protocol work (USB) up front** so the
   large refactor isn't wasted if the USB approach needs to change.

---

## 1. Where we are today (grounding)

- **Monorepo / two ESP-IDF projects**: `apps/main` (the product) and
  `apps/recovery` (factory-partition failsafe). Shared code in top-level
  `components/` (`wifi_manager`, `ota_io`), pulled in via
  `EXTRA_COMPONENT_DIRS=../../components`.
- **Camera abstraction**: `camera_manager` owns a 4-slot table
  (`CAMERA_MAX_SLOTS 4`), NVS slot records, the `camera_driver_t` vtable, the
  recording-intent / mismatch-correction engine, and a BLE add-camera pairing
  state machine. Two drivers register against it:
  - `gopro/open_gopro_ble` — BLE control, Hero9+ (and Hero6/7/8 routed here).
  - `gopro/gopro_wifi_rc` — WiFi RC-emulation, Hero2–Hero5.
- **Trigger**: `can_manager` (TWAI) reads 0x600 logging / 0x602 UTC, transmits
  0x601 camera status, 0x603 shutdown. It is **transport-agnostic already** —
  it calls into `camera_manager_set_desired_recording_all()` and a UTC sync
  fan-out. Reusable wholesale.
- **UI / control plane**: `http_server` (esp_httpd + LittleFS `/www`) and
  `web_ui/` host all `/api/*` and the phone UI. `wifi_manager` raises the
  SoftAP. `log_ring`, `shutdown_manager` round out the main-app components.
- **OTA**: A/B `ota_0`/`ota_1` slots + `factory` recovery + `storage`
  LittleFS (`partitions.csv`). `ota_io` (shared) does streaming SHA-verified
  writes and boot-partition switching. A Cloudflare Worker proxies
  `latest-{stable,beta}` floating GitHub-release tags; the phone fetches the
  signed `manifest.json` + blobs and POSTs them to the device. Channel is a
  shared NVS key read by both apps.
- **Versioning / release**: a single root `VERSION` file drives `v$VERSION`;
  `release-beta.yml` builds **both** apps, stamps `CONFIG_APP_PROJECT_VER`,
  builds a merged `factory.bin`, publishes 5 assets, and moves the
  `latest-beta` tag. `release-promote.yml` is a pure pointer-move. Recovery
  version (`apps/recovery/sdkconfig.defaults:CONFIG_APP_PROJECT_VER`) is bumped
  independently.

### Boot wiring worth noting (`apps/main/main/main.c`)

`app_main` is a linear init list. For the wired product the BLE/WiFi-RC steps
drop out and a USB-host bring-up step is added; everything else (NVS, netif,
SoftAP, http_server, OTA-mark-valid, CAN) is identical. This is the clearest
illustration of how much is shared.

---

## 2. Technical foundations & remaining unknowns (Phase 0)

The hardest unknown — can the ESP32-S3 act as USB host and talk to a GoPro over
USB at all — has been **proven** (see §2.1). What remains is productionizing
that path and confirming the power story (§2.2).

### 2.1 — USB-host networking to the GoPro — PROVEN via CherryUSB

Open GoPro **wired control** is, under the hood, *IP networking over USB*:
- A Hero10+ connected over USB-C enumerates as a **USB network gadget**
  (CDC-ECM / NCM / RNDIS class). The camera runs a DHCP server and exposes the
  **same Open GoPro HTTP API** (port 8080) at a fixed per-serial IP
  (`172.2X.1XX.51`). You enable it with a control request
  (`GET /gopro/camera/control/wired_usb?p=1`) then issue the same
  `/gopro/...` start/stop/keep-alive/datetime calls.
- **Implication (good news):** the *command layer* is essentially the existing
  Open GoPro HTTP path. The new part is bringing up a network interface over
  USB on the ESP32-S3 acting as USB Host.

**Status: proven.** USB host → GoPro has been demonstrated on the ESP32-S3
using the **[CherryUSB](https://github.com/cherry-embedded/CherryUSB)** stack —
a portable USB host/device stack with CDC-ECM/NCM/RNDIS host classes and an
ESP32-S3 port, used **instead of** ESP-IDF's native `usb_host` lib (whose
NCM/ECM *host* support is not first-class). This removes the single biggest
risk that previously gated the whole effort: `usb_host_net` (§6.2) is built on
CherryUSB rather than invented from scratch.

Remaining Phase 0 follow-ups (productionizing the proven PoC):
- [x] **Proven:** ESP32-S3 USB Host enumerates a GoPro and brings up the USB
      link via CherryUSB.
- [ ] Record the specifics here so the build is reproducible: **which network
      class** the target Hero presents (ECM vs NCM vs RNDIS), CherryUSB
      version/commit, the ESP32-S3 board used for the PoC, and how CherryUSB is
      vendored into the IDF build (managed component vs in-tree).
- [ ] Confirm the integration with **lwIP** (CherryUSB host netif → DHCP client
      lease → TCP) and capture timings (enumerate → link up → first HTTP
      command), plus attach/detach (unplug/replug) behavior.
- [ ] End-to-end over the proven link: `GET` the camera info JSON, enable wired
      control, issue one start + one stop. (This is the seed of the
      `gopro_usb` driver, §6.3.)

### 2.2 — RISK: powering the camera

- [ ] Confirm the board/host port can **source 5 V VBUS** at the GoPro's draw
      (recording + charging can pull >1 A; spec the worst case). Hardware
      gate — may need an external 5 V supply / power path, not just the S3's
      USB. Capture findings + any BOM/hardware change here.

> **Phase 0 exit criterion:** a Hero10+ recording is started and stopped from
> an ESP32-S3 over USB (CherryUSB link proven), powered by the intended supply.
> The link half is already demonstrated; remaining gates are the HTTP
> start/stop over it and the power story (§2.2).

---

## 3. Target architecture

```
esp32_gopro_controller/
├── apps/
│   ├── wireless/         ← renamed from apps/main (BLE + WiFi-RC, 4 cameras)
│   ├── wired/            ← NEW (USB, single Hero10+)
│   └── recovery/         ← one source tree, built per-product (OTA source differs)
├── components/           ← SHARED (grows substantially)
│   ├── wifi_manager      (already shared)
│   ├── ota_io            (already shared)
│   ├── can_manager       ← moved up from apps/main/components
│   ├── cam_core          ← NEW: recording engine + driver vtable (BLE-free)
│   ├── http_server_core  ← shared httpd + shared /api/* (settings, ota, logs, system)
│   ├── log_ring          ← moved up
│   └── shutdown_manager  ← moved up
├── apps/wireless/components/   ← wireless-only
│   ├── ble_core, gopro/open_gopro_ble, gopro/gopro_wifi_rc
│   ├── camera_manager_wireless  (multi-slot + pairing, built on cam_core)
│   └── http_server_wireless     (pairing/camera-management /api/*)
├── apps/wired/components/      ← wired-only
│   ├── usb_host_net             (NEW: USB host + NCM/ECM netif)
│   ├── gopro/gopro_usb          (NEW: Open GoPro HTTP driver over USB netif)
│   └── (thin single-camera manager + trimmed wired /api/*)
└── partitions.csv, VERSION, dev.ps1, tools/, docs/ …
```

Both apps' `app_main` share the same skeleton; they differ only in which
camera transport they init.

---

## 4. Component disposition

| Component | Disposition | Notes |
|-----------|-------------|-------|
| `wifi_manager` | **shared** (unchanged) | SoftAP for UI/OTA in both. |
| `ota_io` | **shared** (unchanged) | A/B writer, channel NVS, boot switch. |
| `can_manager` | **move to `components/`** | Already transport-agnostic. Wired uses it verbatim. |
| `log_ring` | **move to `components/`** | Diagnostics, both apps. |
| `shutdown_manager` | **move to `components/`** | Drives the per-camera sleep/teardown; wired version iterates 1 target. Depends on `cam_core` vtable, not BLE. |
| `cam_core` (NEW) | **shared** | Recording intent + `mismatch_step` + grace timers + driver vtable + recording-status cache + CAN-state mapping + time-sync fan-out. **BLE-free** (see §5). |
| `http_server` | **split** → `http_server_core` (shared) + per-app handlers | Shared: `/api/version`, `/api/ota/*`, `/api/settings/*` (CAN/UTC/channel), `/api/logs`, `/api/system`. Wireless-only: `/api/pair/*`, `/api/cameras`, `/api/rc/*`, `/api/reorder-cameras`, `/api/repair-camera`. Wired: single-camera status + a couple of controls. |
| `web_ui` | **per-app, shared base** | Common shell (settings, OTA, logs); wireless keeps the multi-camera pairing UI; wired ships a single-camera panel. Factor common JS/CSS into a shared staged set later. |
| `camera_manager` | **decompose** (see §5) | Splits into `cam_core` (shared) + `camera_manager_wireless` (multi-slot, pairing, BLE/RC glue). |
| `ble_core`, `open_gopro_ble`, `gopro_wifi_rc` | **wireless-only** | Stay under `apps/wireless/components/`. |
| `usb_host_net` (NEW) | **wired-only** | **CherryUSB** host + CDC-ECM/NCM/RNDIS class → lwIP netif + DHCP client (proven, §2.1). |
| `gopro/gopro_usb` (NEW) | **wired-only** | Open GoPro HTTP commands over the USB netif. Reuses the HTTP-command shapes from `gopro_wifi_rc`/Open-GoPro where possible. |
| `gopro/gopro_model.h` | **shared header** | Already ESP-IDF-free. Add a `gopro_model_supports_usb_control()` predicate (Hero10+). |
| `recovery` | **one tree, per-product build** | See §8. |

---

## 5. Decomposing `camera_manager` (the heart of the refactor)

Today `camera_manager` does five jobs. The wired product needs only the first
two; the rest are wireless-only.

| Concern | → goes to | Used by wired? |
|---------|-----------|----------------|
| Recording intent + `mismatch_step` + grace timers + per-target poll dispatch over the vtable | **`cam_core`** | **Yes** |
| Driver vtable + recording-status cache + CAN-state translation (`camera_can_state_t`) + time-sync fan-out | **`cam_core`** | **Yes** |
| Multi-slot table (`CAMERA_MAX_SLOTS`), NVS slot records, slot compaction/reorder | `camera_manager_wireless` | No (1 fixed target) |
| BLE state transitions (`on_ble_connected/ready/disconnected`), `is_known_ble_addr`, `has_disconnected_cameras` | `camera_manager_wireless` | No |
| Pair-attempt state machine (`pair_attempt_*`), WiFi-association tracking | `camera_manager_wireless` | No |

### The critical sever: drop BLE from the camera abstraction header

`camera_manager/include/camera_manager.h:5` currently does
`#include "host/ble_hs.h"` (for `ble_addr_t` in `is_known_ble_addr` etc.). The
shared `cam_core` header **must not** pull in NimBLE. Step one of the
decomposition is to move every BLE-typed declaration into a separate
`camera_manager_ble.h` so the core vtable + types compile with no NimBLE/WiFi
dependency.

Good news already in the tree:
- `camera_types.h` is **already** pure (no ESP-IDF), and `mismatch.c` /
  `reorder_validate.c` are already pure-logic units with host tests. The
  recording engine's core math is portable today.
- `gopro_model.h` is already ESP-IDF-free.

`cam_core` parameterizes slot count: wireless builds with `N = 4`, wired with
`N = 1`. The wired "manager" is then a thin shim: one fixed target, "is a
camera present on USB?" replaces pairing, and slot persistence reduces to a
trivial record (model + name) or none.

Decomposition steps:
- [ ] Extract `camera_manager_ble.h` — move `ble_addr_t`-typed and pairing
      declarations out of the core header. Wireless builds keep working.
- [ ] Carve `cam_core` component: vtable (`camera_driver_t`), `camera_types`,
      recording-intent engine, mismatch poll/dispatch, status cache, CAN-state
      map, time-sync fan-out. Add host tests (extend `tests/host/`).
- [ ] Re-home the remaining multi-slot/BLE/RC/pairing logic into
      `camera_manager_wireless` built on top of `cam_core`. **Re-verify the
      wireless product is byte-for-byte behavior-identical** (CI build + a
      hardware smoke test of pair/record/stop).

---

## 6. The wired product

### 6.1 `apps/wired` skeleton (no camera yet)
- [ ] New ESP-IDF project mirroring `apps/wireless`'s CMake/sdkconfig, minus
      BLE (`CONFIG_BT_*` off → frees flash + RAM, and removes WiFi/BLE coex
      entirely since wired is WiFi-SoftAP-only).
- [ ] `app_main`: NVS → netif → SoftAP → `http_server` → mark-OTA-valid →
      **USB host bring-up** → CAN. (BLE + WiFi-RC steps removed.)
- [ ] Boots to the SoftAP web UI + OTA with a stub/no camera. **Shippable
      milestone** — proves the shared spine works in the second product.

### 6.2 `usb_host_net` (NEW — productionize the proven CherryUSB PoC)
- [ ] Vendor CherryUSB into the wired app (managed component or in-tree, per
      §2.1 decision). USB Host init, device-attach detection, CDC-ECM/NCM/RNDIS
      class bring-up, lwIP netif + DHCP client, link up/down events surfaced
      like `wifi_manager`'s station callbacks. Built on the Phase 0 PoC code.

### 6.3 `gopro/gopro_usb` driver (NEW)
- [ ] Implement `camera_driver_t` over the USB netif: enable wired control,
      start/stop/`get_recording_status`, datetime sync, sleep. Register against
      `cam_core` exactly as the BLE/RC drivers register against
      `camera_manager` today.
- [ ] "Discovery" = USB attach + camera-control-enable handshake → mark the
      single target ready (`*_on_camera_ready`). Unplug → not-ready.

### 6.4 Wired web UI
- [ ] Trim to: single-camera status (plugged / ready / recording), manual
      start/stop, and the shared settings (CAN IDs, UTC, OTA channel, logs).
      Drop pairing/add-camera/reorder entirely.

---

## 7. Partitions & memory

- [ ] Reuse the shared `partitions.csv` as-is. Wired's app is smaller (no
      NimBLE) so it fits `ota_0`/`ota_1` comfortably; USB-host + lwIP fit in
      the freed BLE budget. Re-confirm with a real build's `size` output before
      committing.
- [ ] Confirm USB host + WiFi SoftAP coexistence has no surprising RAM
      pressure (no radio coex needed — wired has no BLE).

---

## 8. Recovery: one tree, per-product OTA source

The recovery app is tiny and product-agnostic in code. "Functionally the same,
different OTA source" maps cleanly to a **compile-time product selector**:

- [ ] Add `CONFIG_PRODUCT_VARIANT` (`wireless` | `wired`) to recovery (and the
      main apps), defaulting per app. Recovery uses it only to choose the OTA
      manifest route (e.g. `…/<variant>/latest-<channel>/manifest.json`).
- [ ] Build recovery twice (once per product) in CI; each product's
      `factory.bin` bundles the recovery built for that variant. The shared NVS
      `ota/channel` key is unchanged; the **product dimension lives in the
      Worker route**, not in NVS.

This keeps recovery a single source tree (one place to fix bugs) while each
shipped image pulls from its own product stream.

---

## 9. Versioning & release — the open question

Recommendation up top, then the trade-offs so you can confirm before Phase 8.

### Recommended: **Option A — one version line, product dimension in the OTA route**
- Keep a single root `VERSION` and a single `vX.Y.Z` per release. Each release
  builds **both** products at that version and publishes per-product assets to
  per-product floating tags: `latest-stable-wireless`, `latest-stable-wired`
  (and `-beta`). The Worker routes `/<product>/…` to the right tag; recovery
  selects its product's route via `CONFIG_PRODUCT_VARIANT`.
- **Pros:** one source of truth; shared components (the majority of the code)
  are always released in lockstep, so a `cam_core`/`can_manager`/`ota_io` fix
  can never be "released for wireless but not wired." Smallest mental model.
- **Cons:** a wired-only change still advances the wireless version number
  (cosmetic). CI does ~2× the build work per release.

### Option B — independent version lines (`VERSION.wireless`, `VERSION.wired`)
- Each product cut on its own cadence with its own semver.
- **Pros:** version reflects per-product change history exactly.
- **Cons:** two release pipelines; every shared-component change must be cut
  twice and you must reason about skew between them; `min_recovery_version`
  bookkeeping doubles. More room for the two to drift.

### Option C — hybrid (shared MAJOR.MINOR for the platform, per-product suffix)
- e.g. `1.4.x+wired.N`. Captures "same platform, different product build."
  More machinery than is justified today; revisit only if cadences truly
  diverge.

**Proposed:** start with **Option A** (lockstep, one `VERSION`), because shared
code dominates and lockstep is the safest default while the two products are
young. Re-evaluate toward B only once their release cadences clearly diverge.
> ⮕ **Decision to confirm before Phase 8.**

Release-pipeline work once decided:
- [ ] Parameterize `release-beta.yml` / `release-promote.yml` over a product
      matrix (`wireless`, `wired`); per-product `factory.bin` + `manifest.json`
      + floating tags.
- [ ] Extend the Cloudflare Worker + `launchpad.toml` with per-product routes
      and Launchpad entries.
- [ ] `dev.ps1`: add a `-Product wireless|wired` switch alongside `-App`
      (artifact names, USB offsets, OTA flow). Today it hardcodes
      `esp32_gopro_canbus_controller_v2.bin` and `apps/main`.
- [ ] `make_manifest.py`: thread a `--product` through asset naming if needed.

---

## 10. Phase checklist (suggested chunking)

Each phase is a reviewable PR-sized chunk. Phases 1–3 ship with **zero**
user-visible change; the wired product only appears from Phase 4.

- [~] **Phase 0 — Foundations & hardware gate** (§2). USB host link **proven
      via CherryUSB**; remaining: HTTP start/stop over the link + VBUS power
      (§2.2). *Hard gate — power half still open.*
- [ ] **Phase 1 — Rename** `apps/main` → `apps/wireless` (`git mv` to preserve
      history). Update `project()` name, `sdkconfig.defaults`,
      `idf_component.yml`, CI `matrix.app`, `release-*.yml` paths, the
      `factory.bin` merge offsets/paths, `dev.ps1`, and docs. Build + smoke
      test. *No behavior change.*
- [ ] **Phase 2 — Lift shared components** to top-level `components/`:
      `can_manager`, `log_ring`, `shutdown_manager`, and split
      `http_server_core` out of `http_server`. Wireless keeps building.
- [ ] **Phase 3 — Decompose `camera_manager`** into `cam_core` (shared,
      BLE-free) + `camera_manager_wireless` (§5). Extend host tests. Re-verify
      wireless behavior-identical. *Largest phase.*
- [ ] **Phase 4 — `apps/wired` skeleton** (§6.1): SoftAP + web UI + OTA + CAN,
      stub camera. Shippable.
- [ ] **Phase 5 — `usb_host_net`** (§6.2): productionize the proven CherryUSB
      PoC into a reusable component.
- [ ] **Phase 6 — `gopro_usb` driver** (§6.3) on the `cam_core` vtable; end to
      end: CAN logging → USB start/stop on a Hero10+.
- [ ] **Phase 7 — Wired web UI trim** (§6.4) + per-product recovery build &
      `factory.bin` (§8) + partition re-confirm (§7).
- [ ] **Phase 8 — Versioning + release pipeline** (§9), per chosen option;
      Worker routes, CI matrix, `dev.ps1`, docs.

---

## 11. Open decisions to confirm

1. **Versioning model** — Option A (recommended) vs B vs C (§9).
2. **USB networking class** — which class the target Hero presents over
   CherryUSB (ECM vs NCM vs RNDIS), and how CherryUSB is vendored into the IDF
   build (§2.1). *Stack chosen (CherryUSB); class to be recorded from the PoC.*
3. **Power path** — does the board source VBUS or do we need an external 5 V
   supply / hardware revision? (§2.2)
4. **Wired camera scope** — strictly single camera, Hero10+ only? (Confirms
   `cam_core N=1` and a `gopro_model_supports_usb_control()` allowlist.)
5. **Web UI sharing** — one shared UI with build-time feature flags, or two UIs
   with a shared base? (Affects §6.4 / `web_ui` staging.)
6. **Recovery selector** — compile-time `CONFIG_PRODUCT_VARIANT` (recommended,
   §8) vs a runtime/NVS product field.

---

## 12. Notes / scratch

- The CAN trigger, time sync, OTA, recovery flow, SoftAP, and the
  intent→mismatch→dispatch engine are **all shared** — the wired product is
  mostly "the same controller with a different camera driver and no BLE." The
  refactor's value is making that sharing explicit rather than copy-pasting
  `apps/main`.
- Keep `docs/refactor-plan.md` (the separate bug/hardening effort) independent
  of this document.
</content>
</invoke>
