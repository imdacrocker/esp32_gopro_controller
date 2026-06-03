# Multi-variant restructure plan

Living document for restructuring the firmware so that **adding new product
variants** (e.g. a future wired/USB GoPro controller, or any other transport)
is straightforward. Wireless is the only shipping product before and after
this work; the restructure stands on its own merit as cleanup of the existing
codebase.

| Product | Camera transport | Trigger | UI / OTA |
|---------|------------------|---------|----------|
| **wireless** (`apps/wireless`) | BLE (Hero9+) + WiFi-RC (Hero2–Hero8) | CAN bus | SoftAP web UI + OTA |

A future sibling app drops into `apps/` alongside `wireless`, pulls in shared
top-level `components/`, and adds its own variant-specific code. The
restructure is what makes that cheap. The wired/USB variant itself is **out
of scope** for this plan — see `docs/wired-variant-followup.md`.

Status legend: `[ ]` todo · `[~]` in progress / partial · `[x]` done.

---

## 0. Guiding principles

1. **No behavior change to the wireless product** through this restructure.
   It builds and works at every step.
2. **Share by extraction, not by copy.** Every line shared between current and
   future variants lives in top-level `components/`. Variant-specific code
   stays in each app's `components/`.
3. **The driver vtable is the seam.** `camera_driver_t`
   (`camera_manager/include/camera_manager.h:13`) already decouples "what to
   record" from "how to talk to the camera." Keep it that way and make the
   shared header BLE-free.
4. **Multi-variant readiness is structural.** Directory layout and the release
   pipeline accept a product dimension even though only one variant ships
   today — the seams are in place for future variants to drop in.

---

## 1. Where we are today (grounding)

- **Monorepo / two ESP-IDF projects**: `apps/wireless` (the product) and
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
  SoftAP. `log_ring`, `shutdown_manager` round out the wireless-app components.
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

### Boot wiring worth noting (`apps/wireless/main/main.c`)

`app_main` is a linear init list (NVS → netif → SoftAP → `http_server` →
mark-OTA-valid → BLE / WiFi-RC → CAN). Most of it is transport-agnostic and
shared by future variants; only the camera-transport steps swap out.

---

## 2. Target architecture (post-restructure)

```
esp32_gopro_controller/
├── apps/
│   ├── wireless/         ← renamed from apps/main (BLE + WiFi-RC, 4 cameras)
│   └── recovery/         ← one source tree, built per-variant
├── components/           ← SHARED (grows substantially)
│   ├── wifi_manager       (already shared)
│   ├── ota_io             (already shared)
│   ├── can_manager        ← moved up from apps/main/components
│   ├── cam_core           ← NEW: recording engine + driver vtable (BLE-free)
│   ├── http_server_core   ← shared httpd + shared /api/* (settings, ota, logs, system)
│   ├── log_ring           ← moved up
│   └── shutdown_manager   ← moved up
├── apps/wireless/components/   ← wireless-only
│   ├── ble_core, gopro/open_gopro_ble, gopro/gopro_wifi_rc
│   ├── camera_manager_wireless  (multi-slot + pairing, built on cam_core)
│   └── http_server_wireless     (pairing/camera-management /api/*)
└── partitions.csv, VERSION, dev.ps1, tools/, docs/ …
```

A future sibling (e.g. `apps/wired/`) drops in alongside `wireless`, pulls in
the shared top-level components, and adds its own transport-specific
components under `apps/<variant>/components/`. The restructure is complete
when that drop-in costs nothing beyond writing the variant-specific code.

---

## 3. Component disposition

| Component | Disposition | Notes |
|-----------|-------------|-------|
| `wifi_manager` | **shared** (unchanged) | SoftAP for UI/OTA in all variants. |
| `ota_io` | **shared** (unchanged) | A/B writer, channel NVS, boot switch. |
| `can_manager` | **move to `components/`** | Already transport-agnostic. Reusable verbatim. |
| `log_ring` | **move to `components/`** | Diagnostics. |
| `shutdown_manager` | **move to `components/`** | Drives the per-camera sleep/teardown. Depends on `cam_core` vtable, not BLE. |
| `cam_core` (NEW) | **shared** | Recording intent + `mismatch_step` + grace timers + driver vtable + recording-status cache + CAN-state mapping + time-sync fan-out. **BLE-free** (see §4). |
| `http_server` | **split** → `http_server_core` (shared) + per-app handlers | Shared: `/api/version`, `/api/ota/*`, `/api/settings/*` (CAN/UTC/channel), `/api/logs`, `/api/system`. Wireless-only: `/api/pair/*`, `/api/cameras`, `/api/rc/*`, `/api/reorder-cameras`, `/api/repair-camera`. |
| `web_ui` | **stays one tree** | Future variants gate off multi-camera affordances via a build-time or runtime variant flag. No UI work in this scope. |
| `camera_manager` | **decompose** (see §4) | Splits into `cam_core` (shared) + `camera_manager_wireless` (multi-slot, pairing, BLE/RC glue). |
| `ble_core`, `open_gopro_ble`, `gopro_wifi_rc` | **wireless-only** | Stay under `apps/wireless/components/`. |
| `gopro/gopro_model.h` | **shared header** | Already ESP-IDF-free. Stays as-is. |
| `recovery` | **one tree, per-variant build** | See §5. |

---

## 4. Decomposing `camera_manager` (the heart of the refactor)

Today `camera_manager` does five jobs. The split is along the line of "what
*every* variant needs" vs. "what the wireless transport needs specifically."

| Concern | → goes to | Shared? |
|---------|-----------|---------|
| Recording intent + `mismatch_step` + grace timers + per-target poll dispatch over the vtable | **`cam_core`** | **Yes** |
| Driver vtable + recording-status cache + CAN-state translation (`camera_can_state_t`) + time-sync fan-out | **`cam_core`** | **Yes** |
| Multi-slot table (`CAMERA_MAX_SLOTS`), NVS slot records, slot compaction/reorder | `camera_manager_wireless` | No |
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

`cam_core` parameterizes slot count so a future single-target variant can
build with `N = 1`. Wireless continues with `N = 4`.

Decomposition steps (all under Phase 3):
- Extract `camera_manager_ble.h` — move `ble_addr_t`-typed and pairing
  declarations out of the core header. Wireless builds keep working.
- Carve `cam_core` component: vtable (`camera_driver_t`), `camera_types`,
  recording-intent engine, mismatch poll/dispatch, status cache, CAN-state
  map, time-sync fan-out. Add host tests (extend `tests/host/`).
- Re-home the remaining multi-slot/BLE/RC/pairing logic into
  `camera_manager_wireless` built on top of `cam_core`. **Re-verify the
  wireless product is byte-for-byte behavior-identical** (CI build + a
  hardware smoke test of pair/record/stop).

---

## 5. Recovery: variant-aware build

The recovery app is tiny and variant-agnostic in code. Add a **compile-time
variant selector** (`CONFIG_PRODUCT_VARIANT`) — users don't normally switch a
device between variants, so there's no need for a runtime/NVS field.
"Functionally the same, different OTA source" maps cleanly to this.

- Add `CONFIG_PRODUCT_VARIANT` (today: `wireless`) to recovery (and the main
  app), defaulting per app. Recovery uses it only to choose the OTA manifest
  route (e.g. `…/<variant>/latest-<channel>/manifest.json`).
- CI builds recovery per variant; today the variant matrix is `[wireless]`,
  which is the point — the seam exists for future variants to drop in. The
  shared NVS `ota/channel` key is unchanged; the variant dimension lives in
  the Worker route, not in NVS.

---

## 6. Versioning & release: variant-aware pipeline

**Decided: Option A** — one root `VERSION`, every variant built per release,
per-variant floating tags (`latest-{stable,beta}-<variant>`), variant
dimension in the OTA route. Implement the machinery now even with one variant
in the matrix so future variants drop in.

Why Option A vs alternatives (independent version lines per variant; hybrid
shared-platform + per-variant suffix): shared code dominates and lockstep is
the safest default while the products are young. Revisit if cadences clearly
diverge.

Release-pipeline work (Phase 4):
- Parameterize `release-beta.yml` / `release-promote.yml` over a variant
  matrix; per-variant `factory.bin` + `manifest.json` + floating tags.
  Today's matrix is `[wireless]` — degenerate but the machinery accepts more.
- Extend the Cloudflare Worker + `launchpad.toml` with per-variant routes and
  Launchpad entries.
- `dev.ps1`: add a `-Product <variant>` switch alongside `-App` (artifact
  names, USB offsets, OTA flow). After Phase 1 the wireless app's paths are
  `esp32_gopro_canbus_wireless.bin` and `apps/wireless`; Phase 4 generalizes
  this to a variant matrix.
- `make_manifest.py`: thread a `--product` through asset naming if needed.

---

## 7. Phase checklist

Each phase is a reviewable PR-sized chunk with zero user-visible behavior
change. Phases run sequentially; the wireless product is shippable after
each.

- [x] **Phase 1 — Rename.** `git mv apps/main → apps/wireless` to preserve
      history. Update `project()` name, `sdkconfig.defaults`,
      `idf_component.yml`, CI `matrix.app`, `release-*.yml` paths,
      `factory.bin` merge offsets/paths, `dev.ps1`, and docs. The release
      YAML and `dev.ps1` touch-ups here are only what's needed to keep CI
      green post-rename; full variant-matrix parameterization waits for
      Phase 4. Build + smoke test. *No behavior change.*
- [x] **Phase 2 — Lift shared components.** Move `can_manager`, `log_ring`,
      `shutdown_manager` to top-level `components/`. Split `http_server` →
      `http_server_core` (shared) + `http_server_wireless` (per-app
      handlers). Wireless keeps building.
- [x] **Phase 3 — Decompose `camera_manager`** into `cam_core` (shared,
      BLE-free) + `camera_manager_wireless` (§4). Extend host tests.
      Re-verify wireless behavior-identical. *Largest phase.*
      Landed across five sub-commits:
      - 3.1 — split `camera_manager_ble.h` out of `camera_manager.h`
      - 3.2 — scaffold `components/cam_core/` (pure types + vtable)
      - 3.3 — move the recording engine, mismatch poll, broadcast
        dispatch, CAN-state translation, sleep/teardown into cam_core.c
      - 3.4 — repoint shared callers (`can_manager`, `shutdown_manager`,
        `http_server_core`) at `cam_core_*` directly; drop unused
        `camera_manager_*` wrappers
      - 3.5 — rename `apps/wireless/components/camera_manager` →
        `camera_manager_wireless` (history preserved via `git mv`)
      Hardware smoke (basic pair / record / stop on real hardware)
      verified post-3.5.  Host-test re-run still owed — blocked on local
      MSVC availability, worth a one-shot CI / MinGW pass since the
      sources moved.
- [x] **Phase 4 — Variant-aware release pipeline** (§5, §6). Landed:
      - `CONFIG_PRODUCT_VARIANT` Kconfig option (default `"wireless"`) in
        both `apps/wireless/main/Kconfig.projbuild` and
        `apps/recovery/main/Kconfig.projbuild`; surfaced via the new
        `product` field on `/api/version` (both apps) and consumed by the
        wireless + recovery update UIs to compose the variant-aware OTA
        route.
      - `release-beta.yml`, `release-promote.yml`, `release-dev.yml`,
        `ci.yml` parameterized over a `variant: [wireless]` matrix.
        Stamps `CONFIG_PRODUCT_VARIANT` into the variant app and the
        recovery app before each build so the same recovery source tree
        produces per-variant images. Immutable per-variant tag
        `v$VERSION-<variant>`; floating per-variant tags
        `latest-{beta,stable,dev}-<variant>`. Promote takes the unsuffixed
        base `v$VERSION` and the matrix appends the variant suffix.
      - `tools/firmware-proxy/src/index.js` accepts the friendly
        `/<variant>/latest-<channel>/manifest.json` route and rewrites it
        to the suffixed GitHub release path; `launchpad.toml` carries one
        `[app]` section per variant (generated from a single
        `SUPPORTED_VARIANTS` list in the Worker so adding a variant is a
        one-line append).
      - `dev.ps1 -Product <variant>` selects the variant; `-App` still
        toggles main-vs-recovery. Build stamps `CONFIG_PRODUCT_VARIANT`
        into both `apps/<variant>` and `apps/recovery` `sdkconfig.defaults`
        before invoking `idf.py`.
      - `tools/release/make_manifest.py --product <slug>` threads the
        variant into the emitted `product` field of `manifest.json`.
      - Docs touched: `docs/releases.md`, `docs/design/ota.md` §5/§6.

---

## 8. Decisions

Resolved:
- [x] **Versioning model** — **Option A**: one `VERSION`, every variant built
      per release, variant dimension in the OTA route + per-variant floating
      tags (§6).
- [x] **Recovery selector** — **compile-time `CONFIG_PRODUCT_VARIANT`**;
      users won't normally switch a device between variants, so no
      runtime/NVS field (§5).
- [x] **Web UI sharing (target shape)** — **one shared UI**, future
      variant-specific affordances gated off the product variant. No UI
      implementation work in this scope; documented as the target for when a
      second variant arrives.

---

## 9. Out of scope

The following are explicitly **not** part of this plan. They become follow-up
work that the restructure (this plan) enables:

- The wired/USB variant itself (`apps/wired` skeleton, `usb_host_net`
  component built on CherryUSB, `gopro_usb` driver, wired-specific UI
  gating). See `docs/wired-variant-followup.md`.
- USB hardware/PoC productionization and the production VBUS power path.
- Any other future sibling variant.

---

## 10. Notes / scratch

- The CAN trigger, time sync, OTA, recovery flow, SoftAP, and the
  intent→mismatch→dispatch engine are all candidates for sharing — the
  restructure's value is making that sharing explicit so future variants
  pick it up without copy-paste.
- Keep `docs/refactor-plan.md` (the separate bug/hardening effort)
  independent of this document.
