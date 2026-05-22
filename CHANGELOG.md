# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

User-facing changes (features, bug fixes, behavior changes) go in the dated
sections below. Each release section corresponds to a `vX.Y.Z` tag on `main`.

## [Unreleased]

### Added
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

[Unreleased]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.6...HEAD
[1.0.6]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.5...v1.0.6
[1.0.5]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.4...v1.0.5
[1.0.4]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.3...v1.0.4
[1.0.3]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.2...v1.0.3
[1.0.2]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.1...v1.0.2
[1.0.1]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/imdacrocker/esp32_gopro_controller/releases/tag/v1.0.0
