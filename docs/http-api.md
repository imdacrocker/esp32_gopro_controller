# HTTP API

The web UI is served from `http://10.71.79.1/`. All API endpoints return `application/json`.

Full request/response contracts (bodies, error codes, state diagrams) are in [`design/web-ui.md`](design/web-ui.md). This page is the index.

---

## System

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/version` | `{app, ui, recovery, channel, running_partition, mode, build_date, build_time, ota_base_url, ota_repo_path}` |
| GET | `/api/logging-state` | RaceCapture logging state (`logging` / `not_logging` / `unknown`) |
| GET | `/api/utc` | UTC timestamp (TZ-applied); `valid` for any anchor incl. NVS-restored, `session_synced` for live-source-this-boot |
| GET | `/api/auto-control` | Whether cameras follow CAN logging state automatically |
| POST | `/api/auto-control` | Set auto-control on/off |
| POST | `/api/reboot` | Restart the ESP32 |

## Cameras

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

## RC-emulation (Hero 4)

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/rc/discovered` | SoftAP stations matching the GoPro OUI allow-list, not yet registered as RC cameras |
| POST | `/api/rc/add` | Register a SoftAP station as an RC-emulation (Hero 4) camera |

## Settings

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/settings/timezone` | Current UTC offset in whole hours |
| POST | `/api/settings/timezone` | Set UTC offset (`−12` to `+14`) |
| POST | `/api/settings/datetime` | Set system time manually (only when no live source has won this boot session) |
| GET | `/api/settings/can-bitrate` | Current CAN bus baud rate (bps) |
| POST | `/api/settings/can-bitrate` | Set CAN bus baud rate; persisted to NVS, applied on reboot |
| GET | `/api/settings/logging-enabled` | Whether the diagnostic log ring is capturing |
| POST | `/api/settings/logging-enabled` | `{ enabled: bool }` — flip the toggle. ON → OFF additionally clears the ring before responding. Persisted to NVS. |

## Diagnostic logs

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/logs/download` | Streams the current ring contents as `text/plain` with a header block (device MAC, firmware, captured-UTC, uptime, ring stats). `Content-Disposition: attachment; filename="gopro-ctrl-<MAC6>-<stamp>.txt"`. Chunked transfer — no large heap allocation. |
| POST | `/api/logs/clear` | Empties the ring. Returns `{ cleared_bytes: N }`. Lifetime counters preserved. |
| GET | `/api/logs/stats` | `{ capacity, used, bytes_written_total, lines_dropped_total, enabled }` |

Endpoints respond normally even when logging is disabled (ring will simply
be empty). The web UI hides the Download / Email / Clear buttons in the
disabled state — but the endpoints are not gated server-side.

See [`design/log-capture.md`](design/log-capture.md) for the full design.

## OTA

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/api/ota/upload-app` | Stream new app image into inactive OTA slot (`X-Sha256`, `X-Size` headers; SHA-skip if NVS-recorded hash matches) |
| POST | `/api/ota/upload-ui` | Stream new `storage.bin` LittleFS image |
| POST | `/api/ota/commit` | Set boot partition + reboot. Returns `{rebooting, boot_partition}`; `rebooting:false` for UI-only updates |
| POST | `/api/ota/reboot-recovery` | Switch boot to factory + reboot (lands in recovery web UI) |
| GET | `/api/ota/channel` | `{current, available[]}` |
| POST | `/api/ota/channel` | Persist new channel into shared NVS |

The recovery app exposes a subset: `/api/version`, `/api/ota/upload-app`, `upload-ui`, `commit`, `boot-main`, and `channel`.
