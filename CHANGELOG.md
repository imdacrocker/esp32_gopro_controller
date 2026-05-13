# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

User-facing changes (features, bug fixes, behavior changes) go in the dated
sections below. Each release section corresponds to a `vX.Y.Z` tag on `main`.

## [Unreleased]

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

[Unreleased]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.2...HEAD
[1.0.2]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.1...v1.0.2
[1.0.1]: https://github.com/imdacrocker/esp32_gopro_controller/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/imdacrocker/esp32_gopro_controller/releases/tag/v1.0.0
