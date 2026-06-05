# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

User-facing changes (features, bug fixes, behavior changes) go in the dated
sections below. Each release section corresponds to a `vX.Y.Z` tag on `main`.

## [Unreleased]

### Added
- **Wired (USB) product variant** — a second sibling firmware for connecting
  a single Hero10+ camera over USB instead of BLE/WiFi-RC. The ESP32-S3 acts
  as a USB host; the camera presents as a CDC-NCM/RNDIS network gadget and
  the controller communicates via the Open GoPro HTTP API over the USB netif.
  The SoftAP is kept purely for the web UI and browser OTA — the USB link and
  the SoftAP are independent network interfaces with no routing between them.
  Delivers the same CAN-bus trigger → start/stop/status/datetime/sleep
  behaviour and the same SoftAP web UI as the wireless variant. See
  [`docs/design/wired-variant.md`](docs/design/wired-variant.md).
  - **`apps/wired/`** — new top-level IDF project (mirrors `apps/wireless`).
    `sdkconfig.defaults` enables USB OTG host mode, disables BLE/Bluetooth
    stacks, and sets `CONFIG_PRODUCT_VARIANT="wired"`.
  - **`usb_host_net` component** — CherryUSB host bring-up, CDC-NCM/RNDIS
    class, lwIP netif + DHCP client, camera-IP derivation (`.51` from lease),
    and link-up/down callbacks modelled on `wifi_manager`'s station API.
  - **`gopro_usb` component** — implements the `camera_driver_t` vtable over
    Open GoPro HTTP. Handles enable, start/stop recording, status polling,
    datetime sync, and sleep. Registers one slot with `cam_core`.
  - **`http_server_wired` component** — thin adapter that calls
    `http_server_core_start()` then mounts wired-only endpoints (single-camera
    status, manual start/stop).
  - **Single-camera web UI** (`apps/wired/web_ui/`) — trimmed variant of the
    wireless UI using the same shared `/api/*` contract; no pairing flows,
    no multi-slot management.

### Changed
- **`shutdown_manager` no longer depends on `open_gopro_ble`.**
  The BLE-terminate step is now routed through a new `terminate_link` vtable
  entry in `cam_core`; the BLE driver implements it, RC and USB leave it
  `NULL`. `shutdown_manager` now depends only on `cam_core` and compiles
  cleanly into the wired variant.
- **`gopro_model` promoted to a shared top-level component** at
  `components/gopro_model/`. Previously lived under
  `apps/wireless/components/gopro/`. Both the wireless gopro drivers and the
  new `gopro_usb` wired driver `REQUIRES gopro_model`.
- **`dev.ps1 -Product <variant>`** now accepts `wired` in addition to
  `wireless`, stamping `CONFIG_PRODUCT_VARIANT` and building the correct app.
- **`flash_factory.ps1`** updated to support the wired variant's flash layout
  and partition offsets.
- **CI matrix and release workflows** extended with a `wired` variant entry
  so the build, test, and release pipelines cover both apps.
  Build artefact is `esp32_gopro_canbus_wired.bin`.

### Internal
- `cam_core` gains a `terminate_link` vtable slot and a `cam_core_terminate_link()`
  helper; BLE driver registers its `ble_gap_terminate` implementation there.

## [1.2.0] - 2026-06-03

### Internal
- **Bumped revovery app to 1.1.0** to reflect major change to release channels.
- **Multi-variant restructure (phases 1–4).** The firmware tree is now
  organised around a "product variant" axis so a future sibling controller
  (e.g. a wired/USB variant) can drop into `apps/<variant>/` and reuse
  every shared seam. Wireless is the only shipping variant before and
  after this work; no user-visible behavior change. Full plan:
  [`docs/multi-variant-restructure-plan.md`](docs/multi-variant-restructure-plan.md).
  - **Renamed** `apps/main → apps/wireless` (history preserved via
    `git mv`). Project name, `sdkconfig.defaults`, CI matrix, release
    YAML paths, and `factory.bin` merge offsets follow the new name.
    Build artefact is now `esp32_gopro_canbus_wireless.bin`.
  - **Lifted shared components** to top-level `components/`:
    `can_manager`, `log_ring`, `shutdown_manager`. Split the HTTP server
    into `http_server_core` (shared `/api/*` — settings, OTA, logs,
    system) and `http_server_wireless` (per-app handlers — pairing,
    cameras, RC, repair).
  - **Decomposed `camera_manager`** into a new shared
    `components/cam_core/` (recording-intent engine, driver vtable,
    `mismatch_step`, grace timers, recording-status cache, CAN-state
    map, time-sync fan-out — all BLE-free) plus
    `apps/wireless/components/camera_manager_wireless/` (the multi-slot
    table, NVS slot records, BLE pairing state machine, WiFi-RC glue).
    The shared `cam_core` header no longer pulls in NimBLE — BLE-typed
    declarations moved into a separate `camera_manager_ble.h`. Slot
    count is parameterised on `cam_core` so a future single-target
    variant can build with `N = 1`.
  - **Variant-aware release pipeline.** New `CONFIG_PRODUCT_VARIANT`
    Kconfig (default `"wireless"`), surfaced via `/api/version`'s new
    `product` field. The wireless and recovery update UIs read the
    field and compose a variant-aware OTA route:
    `latest-<channel>-<product>/manifest.json`. `release-beta`,
    `release-promote`, `release-dev`, and `ci` workflows are now
    parameterised over a `variant: [wireless]` matrix — each step
    stamps `CONFIG_PRODUCT_VARIANT` into the variant app and the shared
    recovery app, so one recovery source tree produces per-variant
    images. Immutable per-variant tag `v$VERSION-<variant>`; floating
    tags `latest-{beta,stable,dev}-<variant>`. Promote takes the
    unsuffixed base `v$VERSION` and the matrix appends the variant.
  - **Cloudflare Worker** drives the Launchpad TOML and a friendly
    `/<variant>/latest-<channel>/manifest.json` rewrite off a single
    `SUPPORTED_VARIANTS` list — adding a future variant is a one-line
    append. `launchpad.toml` has one `[app]` section per variant.
  - **`dev.ps1 -Product <variant>`** stamps the slug into both
    `apps/<Product>/sdkconfig.defaults` and
    `apps/recovery/sdkconfig.defaults` before `idf.py build`. `-App`
    still toggles main-vs-recovery.
  - **`make_manifest.py --product`** threads the slug into
    `manifest.product`.
- **Field-device migration note.** Existing devices that poll the
  unsuffixed `latest-stable/manifest.json` / `latest-beta/manifest.json`
  will 404 on auto-update once releases switch to the variant-suffixed
  tags. New firmware (this release onward) polls the suffixed routes.
  Recovery's manual upload flow remains a fallback for devices stuck on
  the old URL shape. The Cloudflare Worker must be redeployed
  (`cd tools/firmware-proxy && wrangler deploy`) close to the first
  variant-aware `release-beta` run.

## [1.1.0] - 2026-05-27

### Added
- **User-configurable CAN identifiers.** The four CAN frame IDs the
  controller uses are now editable from the **CAN-BUS Settings** sub-modal:
  Logging Command (RX, default `0x600`), Camera Status (TX, default
  `0x601`), GPS UTC (RX, default `0x602`), and Shutdown Request (RX,
  default `0x603`). Each channel independently picks **Standard 11-bit**
  or **Extended 29-bit** IDE and a hex identifier (entered as `0x600`).
  Live client-side validation enforces the allowed range
  (`0x008–0x7FF` std, `0x008–0x1FFFFFFF` ext) and rejects collisions
  across the four channels. A **Reset to defaults** button restores the
  factory IDs in one click. Settings persist to NVS and apply on the next
  reboot — the running dispatch table and TX header keep boot-time values
  until then. See [`docs/design/can-id-configuration.md`](docs/design/can-id-configuration.md).
- **Dev release channel.** A third OTA stream, less formal than stable
  or beta, intended for shipping work-in-progress builds to remote
  testers. A new **release-dev** GitHub Action (manual trigger from the
  Actions tab, optional branch input) builds the selected ref and
  force-moves a floating `latest-dev` release containing `manifest.json`,
  `app.bin`, `storage.bin`, and `recovery.bin`. No VERSION bump is
  required — the build is stamped `dev-<shortsha>-<utc>`. There is no
  immutable per-build tag, no promotion path to beta or stable, and no
  device-side version check: recovery installs whatever the current
  `latest-dev` manifest points at. The dev channel is **recovery-only** —
  the main app's Settings → Updates dropdown does not list it, so testers
  must boot into recovery (Advanced → Restart to Recovery) and pick
  **Dev** from the Channel selector. Includes a `--channel dev` flag on
  `tools/release/make_manifest.py` that bypasses the semver enforcement
  used for stable/beta manifests. No Cloudflare Worker changes — the
  proxy is a transparent pass-through and routes `latest-dev` URLs
  automatically.

### Changed
- **Modals dismiss only via Done/Back/Cancel or Esc.** Previously,
  tapping outside a Settings, CAN-BUS, Updates, Advanced, Power, Manage
  Cameras, or Add Camera modal dismissed it — easy to do by accident on
  a touchscreen mounted in a vehicle. Click-outside is removed; the close
  button on each modal and the **Esc** key are the only ways out.
  Disconnect / shutdown-complete / pair-in-progress overlays are
  unaffected (still non-dismissible, or use their own Cancel button).
- **CAN-BUS settings API consolidated.** `GET /api/settings/can-bitrate`
  and `POST /api/settings/can-bitrate` are replaced by
  `GET /api/settings/can` and `POST /api/settings/can`, which return /
  accept the full CAN-bus configuration (bitrate **and** the four
  channel `(ide, id)` pairs) in a single shape. POSTs accept partial
  bodies — missing fields fall back to current values, the merged set is
  validated as a unit, and persists atomically. The combined endpoint
  preserves the existing apply-on-reboot semantics for bitrate.
- **Settings menu reorganised in the web UI.** The Settings top-sheet is now
  a navigation hub rather than a single flat panel — long-form controls
  live in dedicated sub-modals reached the same Settings→sub-modal→Settings
round-trip pattern already used by Advanced Settings.
- **Power moved out of Settings** into its own top-sheet modal opened by
  a new power-icon button placed immediately right of the gear icon in
  the page header. The modal contains the existing **Reboot** (orange)
  and **Shut Down** (red) buttons, unchanged in behaviour. Removes the
  Reboot/Shut Down buttons from the bottom of the Settings modal.
- **CAN-BUS Settings sub-modal.** Added a "CAN-BUS Settings" entry button
  in Settings; tapping it opens a sub-modal containing the CAN Baud Rate
  selector, the four configurable channel rows (see Added above), a
  Reset-to-defaults button, and the orange "Reboot to apply" hint. The
  bitrate is loaded on sub-modal open instead of on Settings open.
- **Updates sub-modal.** Added an "Updates" entry button in Settings;
  tapping it opens a sub-modal containing the Channel selector, the
  "Check for updates" button, and the existing `#upd-result` panel.
  `updates.js` now refreshes the version / channel state when the
  Updates button is clicked instead of when the gear icon is clicked.
- **Settings modal body now contains:** Time Zone (inline) → CAN-BUS
  Settings → Advanced Settings → Updates → About. The "Device" and
  "Updates" section headings are dropped — every long-form group now
  lives in its own sub-modal, so the headings became redundant.
- DOM IDs that other code relies on (`can-bitrate-select`,
  `upd-channel-select`, `upd-check-btn`, `upd-result`, `reboot-btn`,
  `shutdown-btn`) are unchanged — only their parent overlay changed.
  `#can-bitrate-hint` was renamed to `#can-settings-hint` as part of
  the CAN identifier work above so the same orange hint covers any
  change to the now-configurable channel IDs as well.
- **HTTP server enables LRU socket purge.** When all 8 sockets are in use
  and a 9th client connects, ESP-IDF now evicts the least-recently-used
  socket. Mitigates slow-trickle slowloris-style stalls that could
  otherwise hold every socket and lock the UI out of the controller.
- **NVS initialization failure is now fatal at boot.** A retried
  `nvs_flash_init()` after `nvs_flash_erase()` previously ignored its
  return code; the app would proceed with NVS effectively unavailable
  and every persistence operation would silently fail. Wrapped in
  `ESP_ERROR_CHECK` so the device panics loudly instead.
- **Recovery firmware bumped to 1.0.1.** Enables
  `CONFIG_OTA_ALLOW_DEV_CHANNEL=y` so the recovery web UI's channel
  dropdown and `POST /api/ota/channel` allowlist include `"dev"` in
  addition to `"stable"` and `"beta"`. The equivalent flag in the main
  app remains off, keeping dev out of the main UI. Existing devices
  must reflash recovery (or install a stable/beta build that bundles
  the updated recovery image) before the Dev option appears.

### Fixed
- **Second camera now reliably completes first-pair when two cameras are
  paired in quick succession.** Previously, if both cameras' BLE-readiness
  pipelines completed close together (e.g. auto-reconnect at boot, or
  first-pairing two new cameras through the UI in rapid succession), the
  second pair-complete request was logged with a warning and silently
  dropped. The second camera was then marked BLE-ready but never went
  through SSID/password read, STA-join, or the HTTP wireless/pair/complete
  call — leaving it half-paired until reboot. Pair-complete now queues
  the deferred slot under a mutex and runs it when the in-flight one
  finishes.
- **Removing a camera while it's recording no longer crashes the device.**
  The mismatch-poll timer used to release the slot lock before calling
  into the driver's start_recording / stop_recording, opening a window in
  which a concurrent remove-camera could free the driver context. The
  timer now holds the lock through the dispatch.
- **Recovery's "Boot to Main" picks the correct (newer) OTA slot across
  month-name boundaries.** The previous date comparator sorted the
  ESP-IDF `__DATE__` strings lexically, so e.g. "Nov 30 2024" was reported
  as newer than "Jan 15 2025" (because 'J' < 'N'). On continuous-
  development builds the bug fired on roughly half of month transitions,
  causing recovery to boot the older image after a fresh OTA. Replaced
  with a proper "MMM DD YYYY" / "hh:mm:ss" parser; covered by a 17-case
  host unit test.
- **CAN configuration updates over the web UI are robust to TCP
  segmentation.** The HTTP body reader did a single `recv()` and assumed
  the whole body arrived in one chunk. On a congested SoftAP the ~512-byte
  CAN config POST could split across segments and the JSON parse would
  reject the truncated body. The reader now loops until the full
  `Content-Length` is received.
- **GoPro notifications are discovered correctly across firmware versions
  with non-standard GATT layouts.** The BLE driver previously assumed the
  CCCD descriptor sat immediately after each characteristic's value
  handle. Older Hero firmwares can legally place other descriptors first
  (notably the writable Characteristic User Description), in which case
  the 2-byte CCCD enable would succeed against the wrong descriptor and
  notifications would silently never arrive. Discovery now walks the
  descriptors per spec and writes to the actual 0x2902.
- **Background BLE keepalive no longer fires one stale UDP packet after
  a WiFi RC camera disconnects.** The keepalive timer's
  `esp_timer_stop()` returned before any in-flight callback drained;
  switched to `stop+delete` so disarm waits for the callback. `last_ip`
  is also cleared on disassociate so a SoftAP-reassigned IP can't route
  back to the dead slot.
- **CAN bus-off recovery no longer drops the recovery event under
  sustained bus errors.** The ISR-to-recovery-task queue depth was 4,
  one short of what a back-to-back ACTIVE→WARNING→PASSIVE→BUS_OFF cycle
  during in-progress recovery can require. Doubled to 8.

### Internal
- New host-test binaries (`tests/host/`) cover the pure-logic pieces
  extracted during this round: `test_reorder_validate` (17 cases for
  slot-reorder permutation validation), `test_app_date_compare`
  (17 cases for the OTA date/time comparator). `CAMERA_MAX_SLOTS`
  moved from `camera_manager.h` to `camera_types.h` so host tests can
  reach it without pulling in ESP-IDF deps.
    

## [1.0.9] - 2026-05-27

### Added
- **System shutdown (operator + CAN-triggered).** A new **Shut Down** button
  in Settings (below Reboot, in red) puts every paired camera to sleep,
  tears down the BLE / WiFi RC links, and parks the controller in a
  "pending reboot" state. Confirmation dialog reads *"Shut down controller?
  All cameras will be put to sleep and disconnected. You MUST reboot to
  reconnect."* Once the per-slot sequence finishes, every modal dismisses
  and the page is replaced with a full-screen **"Shut down complete! OK to
  power off"** overlay carrying an orange **REBOOT** button. The reboot
  button POSTs `/api/reboot` and then polls `/api/version` until the
  device is back before reloading, so the page actually refreshes once the
  ESP comes online again. Refreshing the page mid-shutdown lands on the
  correct screen (overlay if complete, polling if still shutting down).
- **CAN-bus shutdown trigger** on new ID **`0x603`** (RaceCapture → ESP32).
  Byte 0 non-zero requests shutdown; idempotent. Lets vehicle ignition
  logic power the rig down without touching the web UI.
- **Per-camera sleep behaviour during shutdown.** If a camera is recording,
  the controller issues stop-recording first and waits up to 1.5 s for
  confirmation, then sends the model-appropriate sleep command:
  - Hero9 / Hero10 / Hero11 / Hero12 / Hero13 — BLE TLV `0x05` on GP-0072.
  - Hero7 / Hero8 — same TLV, best-effort (firmware may reject; falls
    back to auto-sleep once keepalive stops).
  - Hero4 / Hero5 / Hero6 (any WiFi RC slot) — HTTP `GET
    /gp/gpControl/command/system/sleep`. Verified on Hero4 + Hero5;
    sent unconditionally to every RC slot.
  - Any model without a usable sleep path — keepalives just stop and the
    camera auto-sleeps on its own inactivity timer.
  Per-slot 5 s deadline; unresponsive cameras don't block shutdown from
  completing. BLE-control links are explicitly terminated after the
  sleep TLV so the camera doesn't wait through its own supervision
  timeout.
- **`POST /api/shutdown`** — kicks off the sequence. Idempotent; returns
  the current state.
- **`GET /api/shutdown`** — `{state, failed_slots[]}` for the web UI to
  poll. `state` is `idle` / `shutting_down` / `complete`.

### Changed
- **Action endpoints return `503 Service Unavailable` during shutdown.**
  Shutter, pair, scan, remove-camera, reorder-cameras, repair-camera,
  pair/cancel, rc/add, all `/api/settings/*` POSTs, and
  `POST /api/auto-control` are gated. Read-only GETs and `POST /api/reboot`
  stay open so the UI can observe state and the user keeps an escape
  hatch.
- **CAN frame handling under shutdown.** `0x600` (logging-state) frames are
  dropped silently while the state machine is not `idle` — otherwise the
  next RaceCapture frame would re-issue recording intent and undo the
  per-slot stop-recording step. `0x601` (camera status TX, 5 Hz) stops
  transmitting once the state reaches `complete`; RaceCapture-side
  observers can treat CAN silence on `0x601` as the shutdown indicator.
- **BLE background reconnect is suppressed during shutdown.** `ble_core`
  honours a new `is_shutdown_active` callback and skips the scan / connect
  loop while it returns true. Any camera-initiated reconnect that races
  the teardown is terminated by `open_gopro_ble`'s on-connect callback.

## [1.0.8] - 2026-05-26

### Added
- **Disconnected overlay in the web UI.** When the page can't reach the
  controller for ~3 consecutive requests (~3–6 s), the UI hides every modal
  and replaces the screen with a full-screen white panel: a no-Wi-Fi icon,
  "DISCONNECTED" in bold, and a blue RELOAD button. A background probe of
  `GET /api/version` runs once a second; the first 2xx response triggers
  `location.reload()` automatically, so the page comes back on its own as
  soon as the device is reachable again. RELOAD lets the user force a
  reload at any time. All UI timers are paused while disconnected so no
  stale renders or background requests fire.
- **Hero6 / Hero7 / Hero8 BLE pairing support** with a new one-shot WiFi
  pair-complete handshake at first-pair. These cameras accept BLE
  pairing at the SMP level but do not register the controller as a
  paired app until it hits the legacy
  `GET http://10.5.5.9/gp/gpControl/command/wireless/pair/complete?success=1&deviceName=<name>`
  HTTP endpoint on the camera's own WiFi AP. The new
  `apps/main/components/gopro/open_gopro_ble/pair_complete.c`
  orchestration reads the camera's SSID/password over BLE, briefly
  switches the ESP32 radio to STA mode to issue the HTTP call, and
  restores the SoftAP. Verified on Hero7 (firmware HD7.01.01.90.71,
  both stock and GoPro Labs). Hero6 and Hero8 are listed on the
  assumption they share the legacy pairing model — adjust the
  `gopro_model_needs_wifi_pair_complete()` predicate if hardware
  testing proves otherwise.
- **5 GHz fail-fast for Hero6/7/8** via BLE status 76 (`WirelessBand`).
  The ESP32-S3 radio is 2.4 GHz only — if the camera reports 5 GHz, the
  orchestration fails immediately with a precise error message
  (`"Camera on 5 GHz; set Wi-Fi Band to 2.4 GHz, re-pair"`) instead of
  thrashing through STA join attempts. Surfaced through the existing
  `/api/pair/status` polling flow.
- **`wifi_manager_sta_join()` / `wifi_manager_sta_leave()`** API in
  `components/wifi_manager/` — blocking AP-down → STA-join → AP-up
  cycle for the pair-complete dance. Connected SoftAP clients see a
  brief link drop and reconnect automatically when the AP comes back up.
- **`POST /api/repair-camera`** endpoint — clears `first_pair_complete`
  on a slot so the legacy WiFi pair-complete handshake re-runs on the
  next BLE reconnect. For use after the user runs Reset Connections on
  the camera and the camera-side app entry is wiped.
- **`CONFIG_DEVICE_IDENTITY_NAME`** Kconfig option (default
  `"ESP32 Controller"`, under "GoPro Controller — Identity") —
  centralizes the controller's display name across NimBLE GAP, GoPro
  RequestPairingFinish, and the wireless/pair/complete URL. Override
  via `idf.py menuconfig` or `sdkconfig.defaults`.
- **`pair_attempt_reset_watchdog(timeout_ms)`** — long-running
  post-BLE phases (currently just pair-complete) can extend their
  deadline beyond the default 20 s BLE-setup watchdog. The watchdog
  still catches genuine hangs.
- **Hero8 model enum** (`CAMERA_MODEL_GOPRO_HERO8_BLACK = 50`) and
  `"HERO8 Black"` mapping in `gopro_model_from_name()`.

### Changed
- **Camera pairing flow redesigned in the web UI.** The Manage Cameras
  modal now ends in a single "Add Camera" button (the previous separate
  "Add a new Bluetooth camera" and "Add a new Wifi RC Camera" buttons
  are gone). Tapping it opens a new "Add New" bottom sheet that lists
  the supported camera models — Hero3, Hero4, Hero7, Hero9, Hero10,
  Hero11, Hero12, Hero13 — as a scrollable, mobile-friendly picker.
  Selecting a model slides left to a model-specific instructions screen
  with a Back arrow to return to the list:
  - **Hero3 / Hero4** → "pair with a new WiFi RC" instructions plus the
    SoftAP connected-devices list (Add buttons), polled every 3 s.
  - **Hero7** → "pair with the GoPro App" instructions with an extra
    reminder to switch the camera Wi-Fi to 2.4 GHz manually after Reset
    Connections, and a warning that the controller's STA cycle may drop
    the user's browser session mid-pair. Same BLE Scan button as the
    other newer cameras.
  - **Hero9 / Hero10 / Hero11 / Hero12 / Hero13** → standard "pair with
    the GoPro App" instructions with a Scan button.
  Add (RC) and Pair (BLE) both hand off to the same existing
  pair-progress overlay — only the entry funnel changed.
- **Advanced Settings modal: "Done" relabelled to "Back"** to match the
  actual behaviour (closing Advanced returns to the Settings modal).
- **System Time display now ticks every second locally.** Previously the
  seconds digit only advanced when the 2 s `/api/utc` poll returned, so it
  jumped by 2 with visible jitter. The browser now extrapolates wall-clock
  time from the last poll's `epoch_ms` + a monotonic `performance.now()`
  delta and re-renders once per second. The `/api/utc` poll cadence is
  unchanged — only the display tick is local.
- **Hero7 BLE control is enabled** (was deliberately frozen in prior
  releases because of the misdiagnosed "SetShutter rejected with
  status 0x02" symptom). The rejection was actually
  "controller not registered as a paired app" — same on stock and Labs
  firmware. The pair-complete handshake above resolves it.
- **Hero6/Hero7/Hero8 removed from `gopro_model_uses_rc_emulation()`** —
  these models are now BLE-only. Existing RC-paired slots for these
  models after upgrade have no matching driver; remove via the web UI
  and re-pair via BLE. Hero5 stays in RC-emulation pending hardware
  verification of its BLE path.
- **`gopro_model_is_frozen()` returns false for all models.** Predicate
  kept as a hook for future use.
- **`RequestPairingFinish` is skipped on `needs_wifi_pair_complete`
  cameras.** On Hero7 it dismisses the on-screen pairing prompt
  prematurely and interferes with the camera-side app registration that
  the HTTP handshake drives. Other models (Hero11 Mini / Hero12 /
  Hero13 / Max 2 / Lit Hero) still receive it.
- **Non-keepalive BLE settings responses now log at INFO** so
  diagnostic state from one-shot writes (e.g. setting 178 WirelessBand)
  is visible without enabling DEBUG. Keepalive responses (fire every
  3 s) stay at DEBUG.

### Fixed
- **Hero9 no longer gets stuck in a connect / "Pairing Not Supported"
  reconnect loop.** On a quick reconnect Hero9 is slow to respond to the
  link-layer encryption-start, which the controller used to receive as
  HCI status `0x08` (Connection Timeout, NimBLE-wrapped value 520). The
  `is_transient_enc_error()` allowlist did not include this code, so the
  encryption handler took the "key mismatch" branch and called
  `ble_gap_unpair()` on a perfectly valid LTK. From there every
  subsequent reconnect failed with SMP peer error `0x05` ("Pairing Not
  Supported") because the camera was no longer in pair-mode and the
  controller had no key to resume. The classifier is now inverted: only
  HCI `BLE_ERR_AUTH_FAIL` (MIC check failed) and `BLE_ERR_PINKEY_MISSING`
  (peer lost our LTK) trigger bond deletion. Everything else preserves
  the bond and lets the reconnect scan retry on the disconnect that
  follows.

## [1.0.7] - 2026-05-18

### Fixed
- **Hero10 (and other BLE-control cameras) no longer flap between
  recording / not-recording in the UI while actually recording, and the
  camera no longer beeps every ~12 s during a take.** The BLE recording
  status poll was querying status ID 8, which is `Busy` per the OpenGoPro
  spec (transient menu-transition / settings-write state), not the
  recording flag. On Hero10 it stayed `0` for the entire duration of a
  recording, so the mismatch-correction loop kept re-issuing
  `SetShutter(ON)` after the 10 s grace expired. The poll now uses status
  ID 10 (`Encoding`), which is the actual recording flag.
- **HTTP `ESP_ERR_HTTP_INCOMPLETE_DATA` with a 2xx status is now
  treated as success in the pair-complete flow.** Hero7 returns
  malformed chunked HTTP responses (bare CR without LF in places), but
  by the time the body is streaming the camera has already processed
  the request and registered us. Previously we tore the slot down on
  this error even though the camera-side registration succeeded.
- **`wifi_manager` now subscribes to `IP_EVENT_STA_GOT_IP`.** Previous
  registration covered only the AP-side `IP_EVENT_ASSIGNED_IP_TO_CLIENT`,
  so `sta_join()` reported DHCP timeout even though the STA actually
  received an IP. Surfaced once the pair-complete flow started
  exercising STA mode.

## [1.0.6] - 2026-05-16

### Fixed
- **Fresh installs from `factory.bin` (ESP Launchpad / `flash_factory.ps1`)
  now boot directly into the main app instead of recovery.** ESP-IDF
  emits `ota_data_initial.bin` as blank, which the bootloader interprets
  as "boot factory (= recovery)" when a factory partition is present.
  The release pipeline and local provisioning script now stamp it with
  a valid otadata entry that selects `ota_0` on first boot. Recovery
  remains reachable via the rollback fallback and the "Restart to
  Recovery" button.
- **OTA updates apply reliably on first reboot, even with a serial
  monitor attached.** The new image is marked valid as soon as the HTTP
  server is serving, rather than after a 30-second soak. The old soak
  window raced with the USB-UART reset triggered by `idf.py monitor`
  attaching after an OTA push — the new image would boot, then get
  rolled back to the previous version before the timer fired. The
  shortened semantics are acceptable given the project's threat model
  (closed CAN bus, no PII — see `docs/design/ota.md` §4).

## [1.0.5] - 2026-05-16

### Added
- **Diagnostic log capture for user reports.** A 64 KB in-RAM ring buffer
  collects all `ESP_LOG` output (DEBUG and above). Toggle "Enable Logging"
  in Settings → Advanced Settings to start capturing, then download the
  log as a text file or hand it to the **Email log** button — which kicks
  off a download and opens your mail app pre-filled to the maintainer with
  attach-the-file instructions and a privacy notice covering what the log
  contains. Off by default; persisted across reboots. See
  [`docs/design/log-capture.md`](docs/design/log-capture.md).
- **About dialog** in Settings showing main app version, build date/time,
  and recovery app version.
- **Soft recovery fallback.** If the LittleFS storage partition is wiped
  or corrupted (auto-reformatted empty), the main app now reboots into
  recovery automatically rather than serving a blank "no UI" page. The
  user can re-upload the web UI from recovery's embedded form without
  needing serial access.

### Fixed
- **Hero4 cameras (WiFi RC mode) now reconnect reliably after an ESP32
  reboot.** Previously the per-slot periodic keepalive was silently
  no-op'd after a reboot because the driver's cached IP wasn't seeded
  from the persisted slot record, and the WoL retry watchdog couldn't
  arm because its silence reference was never initialised. The slot
  would sit waiting until the camera happened to transmit on its own —
  often several minutes. Reconnect now typically completes within
  seconds of the camera re-associating.

### Changed
- **WoL retry watchdog now arms after 5 s of silence (was 10 s).** Halves
  the worst-case time before the driver starts sending wake-on-LAN
  bursts at an associated-but-unresponsive Hero4.
- **Settings menu reorganized.** Logging controls, Restart-to-Recovery,
  and other power-user controls moved into a new **Advanced Settings**
  modal. The "App version / Built / Recovery" rows moved into the new
  About dialog. The collapsing "Advanced" disclosure is gone — Advanced
  Settings is now a blue button. Opening Advanced Settings dismisses the
  Settings modal so the two don't stack; closing Advanced Settings returns
  you to Settings.
- Web UI static-asset reads now log a clear diagnostic to the serial
  console (with errno + free-heap state) when `fopen` or `fread` fails,
  rather than silently falling back to the "Web UI not flashed"
  placeholder. (The placeholder still serves for genuinely missing files.)

## [1.0.4] - 2026-05-16

### Fixed:

- Fixed a bug where the can_manager could override the requested camera status even if automatic logging was turned off

## [1.0.3] - 2026-05-13

### Fixed:

- CAN controller now automatically recovers from bus-off (e.g. when the device boots before the rest of the vehicle bus is powered up) instead of requiring a reboot.
- Second paired BLE camera now connects automatically while the first camera is still in use. Previously the background scan stayed dormant after the first camera came up, so the second slot remained disconnected until the first camera went away.
- Hero 9+ no longer floods the log with "notify on unregistered handle" warnings. The camera's WiFi AP state notifications (GP-0005) are now recognised and logged at debug level instead.

## [1.0.2] - 2026-05-12

### Fixed
- Fresh installs no longer fall back to the recovery app if you reboot
  from the web UI within 30 seconds of flashing the main firmware.

## [1.0.1] - 2026-05-12

### Added
- Settings UI exposes a CAN-bus baud-rate selector so installers can match
  their vehicle's bus speed without re-flashing.

## [1.0.0] - 2026-05-08

First public release.

### Added
- ESP32-S3 firmware that bridges a vehicle CAN bus to GoPro cameras over
  BLE (Open GoPro) and Wi-Fi (Hero4 RC emulation).
- Camera manager supporting multiple paired cameras with auto-reconnect.
- SoftAP web UI for pairing, status, settings, and OTA updates.
- Recovery app on the factory partition with a self-contained web UI for
  re-flashing the main app and storage partition from a browser.
- OTA delivery via GitHub Releases with floating `latest-beta` and
  `latest-stable` tags, proxied through a Cloudflare Worker.

[Unreleased]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.3.0...HEAD
[1.3.0]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.9...v1.1.0
[1.0.9]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.8...v1.0.9
[1.0.8]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.7...v1.0.8
[1.0.7]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.6...v1.0.7
[1.0.6]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.5...v1.0.6
[1.0.5]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.4...v1.0.5
[1.0.4]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.3...v1.0.4
[1.0.3]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.2...v1.0.3
[1.0.2]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.1...v1.0.2
[1.0.1]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/imdacrocker/esp32_gopro_controller/releases/tag/v1.0.0
