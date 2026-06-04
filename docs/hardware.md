# Hardware

## Target

| Item | Detail |
|------|--------|
| SoC | ESP32-S3 |
| Flash | 8 MB |
| Partition table | Custom (`partitions.csv` at repo root) — both apps share it |
| CAN transceiver | TWAI-compatible (wired to ESP32-S3 TWAI peripheral) |

## Partition Layout

8 MB total. A/B updates enabled — the bootloader picks whichever OTA slot `otadata` last marked valid, and falls back to `factory` (recovery) on a bad image or if the 30 s rollback timer fires before `app_main` calls `esp_ota_mark_app_valid_cancel_rollback()`.

| Partition | Type | Offset | Size | Contents |
|---|---|---|---|---|
| `nvs` | data | `0x009000` | 24 KB | NVS (camera slots, OTA state, bonds) |
| `otadata` | data | `0x00F000` | 8 KB | which OTA slot is active |
| `phy_init` | data | `0x011000` | 4 KB | PHY init data |
| `factory` | app | `0x020000` | 768 KB | **recovery app** (always-present fallback) |
| `ota_0` | app | `0x0E0000` | 1.69 MB | wireless app slot A |
| `storage` | data | `0x290000` | 3 MB | LittleFS (web UI for wireless app) |
| `ota_1` | app | `0x590000` | 1.69 MB | wireless app slot B |
| *(free)* | | `0x740000` | 768 KB | reserved |

See [`design/ota.md`](design/ota.md) §3 for the full partition table rationale and §6 for the boot/upgrade flow.
