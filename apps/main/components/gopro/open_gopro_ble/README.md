# open_gopro_ble

The complete BLE control driver for Hero 9+ GoPro cameras. It owns discovery, pairing, GATT setup, the post-connection readiness sequence, the periodic recording-status poll, and all recording commands. It is the only component that registers callbacks with `ble_core` and the only component that writes GoPro BLE characteristics.

All recording control travels over BLE — there is no HTTPS path. The BLE connection is held open continuously; loss of link is the same signal as "camera went away".

---

## Responsibilities

- **Discovery** — filter BLE advertisements by the GoPro service UUID (`0xFEA6`); maintain a 10-entry list for the web UI (`GET /api/cameras`).
- **Pairing** — register new cameras into `camera_manager` on first bond; reconnect known cameras from NVS bond store.
- **GATT setup** — discover all characteristics across the full handle range, then subscribe CCCDs for all notify/indicate characteristics.
- **Readiness sequence** (V1-style):
  1. `GetHardwareInfo` (TLV 0x3C) poll until status = 0 (up to 10× at 3 s intervals). Parse the positional LV body (model number, model name, firmware, serial, AP SSID, AP MAC); the model number is handed to `camera_manager` as `camera_model_t`.
  2. `SetCameraControlStatus(EXTERNAL)` — protobuf Feature 0xF1 / Action 0x69 to GP-0072. Wait for `ResponseGeneric` on Action 0xE9, with a 3 s timeout. Connection sequence advances on either ack or timeout.
  3. `camera_manager_on_camera_ready(slot)` — slot transitions to `WIFI_CAM_READY` and the mismatch-correction timer arms.
  4. `SetDateTime` (best-effort; deferred via `datetime_pending_utc` if UTC isn't session-synced yet).
  5. Start the 5 s `GetStatusValue` poll.
- **Recording control** — `SetShutter` TLV 0x01 to GP-0072 (`{0x03, 0x01, 0x01, 0x00|0x01}`).
- **Recording status poll** — `GetStatusValue` TLV 0x13 on GP-0076 every 5 s, requesting status ID 10 (`Encoding`; 0 = idle, 1 = recording). Cached value is exposed via `camera_driver_t.get_recording_status()`.
- **UTC sync** — `open_gopro_ble_sync_time_all()` sends `SetDateTime` to every connected slot when UTC becomes session-synced (called from `on_utc_acquired`). `SetDateTime` itself is gated on `can_manager_utc_is_session_synced()`, so an NVS-restored boot value cannot push stale time to a camera.
- **BLE keepalive** — send TLV cmd `0x5B` with value `0x42` (`{0x03, 0x5B, 0x01, 0x42}`) to GP-0074 every 3 s to maintain the link supervision timer and prevent camera auto-sleep.

---

## Dependencies

```
REQUIRES: bt, nvs_flash, esp_timer, freertos, camera_manager, can_manager, ble_core
```

**Precondition:** `camera_manager_init()` must be called before `open_gopro_ble_init()`.
`open_gopro_ble_init()` must be called before `ble_core_init()` — it registers the BLE callbacks that `ble_core` will use once the host task starts, and the `camera_driver_t` that `camera_manager` will dispatch to.

---

## Source Files

| File | Responsibility |
|------|---------------|
| `include/open_gopro_ble.h` | Public API (lifecycle, discovery, time sync) |
| `include/open_gopro_ble_spec.h` | All raw protocol constants: GoPro GATT UUIDs, command IDs, GPBS header format, protobuf field tags, response field offsets, poll intervals, retry caps |
| `open_gopro_ble_internal.h` | Private shared types (`gopro_gatt_handles_t`, `gopro_ble_ctx_t`, `gopro_channel_t`) and internal function declarations |
| `driver.c` | Per-slot context table, discovery list with UUID filter, `camera_driver_t` vtable, driver registration, `open_gopro_ble_init()` |
| `pairing.c` | `on_connected` / `on_encrypted` / `on_disconnected` callbacks; explicit MTU exchange (Hero 13 does not initiate one) |
| `gatt.c` | Full-handle-range service/characteristic discovery, sequential CCCD subscription state machine |
| `readiness.c` | `GetHardwareInfo` retry loop, hardware-info LV parser, `SetCameraControlStatus(EXTERNAL)` handshake with timeout, drives the post-readiness sequence |
| `control.c` | `SetDateTime`, `SetCameraControlStatus`, `SetShutter` packet builders; 3 s keepalive timer |
| `status.c` | 5 s `GetStatusValue` poll timer; status response parser updating cached `camera_recording_status_t` |
| `query.c` | GPBS packet reassembler (general / ext-13 / ext-16 headers, continuation packets); 3-channel response dispatch |
| `notify.c` | `on_notify_rx` callback — maps `attr_handle` to `gopro_channel_t`, feeds `gopro_query_feed()` |

---

## Public API

Header: `include/open_gopro_ble.h`

```c
/* Lifecycle */
void open_gopro_ble_init(void);

/* Discovery */
void open_gopro_ble_start_discovery(void);
void open_gopro_ble_stop_discovery(void);
int  open_gopro_ble_get_discovered(gopro_device_t *out, int max_count);

/* Connection */
void open_gopro_ble_connect_by_addr(const ble_addr_t *addr);

/* UTC sync — called by main.c on the first live UTC source this session
 * (GPS frame or web-UI manual set).  Sends SetDateTime to every connected
 * slot and clears any pending datetime_pending_utc flag.  No-op for
 * individual slots when UTC is not session-synced (so an NVS-restored boot
 * value cannot push stale time to a camera). */
void open_gopro_ble_sync_time_all(void);
```

The recording commands (`start_recording` / `stop_recording`) and status query (`get_recording_status`) are exposed via the `camera_driver_t` vtable, not as public functions.

---

## GATT Channel Map

All GoPro characteristics use the 128-bit base UUID `b5f9XXXX-aa8d-11e3-9046-0002a5d5c51b`.

| Handle name | UUID suffix | Direction | Channel |
|------------|-------------|-----------|---------|
| `cmd_write` | `0072` | Write | TLV commands + COMMAND-feature protobuf (`SetShutter`, `SetDateTime`, `SetCameraControlStatus`, `GetHardwareInfo`) |
| `cmd_resp_notify` | `0073` | Notify | Command responses (TLV + protobuf) |
| `settings_write` | `0074` | Write | Keepalive (cmd 0x5B, value 0x42) |
| `settings_resp_notify` | `0075` | Notify | Settings responses (acknowledged but not acted on) |
| `query_write` | `0076` | Write | TLV queries (`GetStatusValue`) |
| `query_resp_notify` | `0077` | Notify | Query responses (status updates) |
| `nw_mgmt_write` | `0091` | Write | Network-management protobuf (`RequestPairingFinish`) |
| `nw_mgmt_resp_notify` | `0092` | Notify | Network-management responses |
| `wifi_ap_state_indicate` | `0005` | Indicate | WiFi AP state push (0 = off, 1 = on); logged at debug level, no action taken |

CCCDs are subscribed sequentially after characteristic discovery. GoPro cameras do not persist CCCD state across connections — subscriptions are re-sent on every reconnection.

Once the final CCCD ACK arrives, `gatt.c` calls `ble_core_resume_background_scan()` before kicking off the readiness sequence. This lets `ble_core` look for additional paired cameras while the lighter GetHardwareInfo poll is still running for this slot, rather than waiting until the whole readiness sequence is done. With only one configured camera the call is a no-op (`has_disconnected_cameras()` returns false).

---

## Connection State Machine

```
on_connected(conn_handle, addr)
  known camera  → record conn_handle, camera_manager_on_ble_connected()
  unknown       → deferred until on_encrypted

on_encrypted(conn_handle, addr)
  known camera  → camera_manager_on_ble_connected()
  new camera    → camera_manager_register_new(); if slots full → disconnect
  → ble_gattc_exchange_mtu()  (Hero 13 doesn't initiate one itself)
  → gopro_gatt_start_discovery()

GATT discovery complete
  → subscribe CCCDs sequentially (all 128-bit notify/indicate chrs)
  → ble_core_resume_background_scan()   (lets a second paired camera connect
                                          while this slot finishes readiness)
  → gopro_readiness_start()

GetHardwareInfo poll  (up to 10 retries × 3 s)
  success → parse model + log info → camera_manager_set_model() + save_slot()
          → camera_manager_on_ble_ready()
          → start keepalive timer
          → SetCameraControlStatus(EXTERNAL) → wait for 0xE9 response (3 s timeout)
                ack or timeout →
                  camera_manager_on_camera_ready()
                  SetDateTime (best-effort; deferred via datetime_pending_utc
                               if UTC not session-synced)
                  start 5 s GetStatusValue poll
  failure → retry; after 10 failures → ble_gap_terminate()

on_disconnected
  → cancel readiness timer
  → cancel SetCameraControlStatus timeout
  → stop keepalive timer
  → stop status poll timer
  → free reassembly state
  → clear gopro_gatt_handles_t and conn_handle, reset cached_status
  → camera_manager_on_ble_disconnected_by_handle()
```

---

## Recording Commands

### SetShutter (TLV 0x01)

```
[0x03, 0x01, 0x01, value]
  byte 0 : GPBS general header, payload length 3
  byte 1 : command ID 0x01 (SetShutter)
  byte 2 : param length 1
  byte 3 : 0x00 = stop, 0x01 = start
```

Written to GP-0072. The driver vtable `start_recording` / `stop_recording` set `cached_status` optimistically (`ACTIVE` / `IDLE`) immediately after the write returns; the next status poll cycle confirms or corrects it.

### GetStatusValue (TLV 0x13)

```
Request:  [0x02, 0x13, 0x0A]   (header len=2, cmd 0x13, status_id 10)
Response: [cmd_id, status, 0x0A, 0x01, value]
                            └── id ┘ └─len─┘ └─v─┘
```

Status ID 10 is `Encoding` (0 = idle, 1 = recording). Polled every 5 s on `query_write` (GP-0076); response arrives on `query_resp_notify` (GP-0077). The poll fires once immediately on start so `cached_status` updates within ~half an MTU latency rather than waiting the full 5 s.

> Earlier revisions polled status ID 8, but per the OpenGoPro spec that is `Busy` (transient camera-busy state during menu transitions / settings writes), not the recording flag. On Hero10 it stayed 0 while recording, causing the mismatch poll to re-issue `SetShutter(ON)` and the UI to flap between recording/not-recording states.

### SetCameraControlStatus(EXTERNAL) (Protobuf)

```
[0x04, 0xF1, 0x69, 0x08, 0x02]
  byte 0 : GPBS general header, payload length 4
  byte 1 : feature ID 0xF1 (COMMAND)
  byte 2 : action ID 0x69 (RequestSetCameraControlStatus)
  byte 3 : protobuf field-1 tag (varint)
  byte 4 : EnumCameraControlStatus.EXTERNAL (2)
```

Sent once per connection after `GetHardwareInfo` succeeds. Declares to the camera that an external controller is driving it (suppresses some on-screen UI). Response is `ResponseGeneric` on Feature 0xF1 / Action 0xE9; the connection sequence proceeds on ack OR after a 3 s timeout — a silent camera should not stall setup.

### SetDateTime (TLV 0x0D)

```
[0x0C, 0x0D,
 0x01, 0x04, year_hi, year_lo, month, day,        ← param 1: date
 0x02, 0x03, hour, minute, second]                ← param 2: time
```

Best-effort. Internally gated on `can_manager_utc_is_session_synced()`. If UTC isn't session-synced when readiness completes, the driver sets `datetime_pending_utc` on the slot's context and `open_gopro_ble_sync_time_all()` consumes the flag when UTC becomes available.

---

## GPBS Packet Reassembly

GoPro cameras may fragment long responses across multiple ATT notifications. `query.c` maintains a per-slot, per-channel reassembly buffer (3 channels × `CAMERA_MAX_SLOTS`).

**Start packet header:**

| Header type | Byte 0 | Extra bytes | Max payload |
|------------|--------|-------------|-------------|
| General | `0b000LLLLL` (L = length) | — | 31 B |
| Extended 13-bit | `0b001HHHHH` | 1 (low 8 bits) | 8191 B |
| Extended 16-bit | `0b010_____` | 2 (16-bit length) | 65535 B |

**Continuation packet:** bit 7 set; low 7 bits = sequence number (starts at 0).

Maximum reassembled response: 512 bytes (covers all known responses including `GetHardwareInfo` at ~88 bytes and small status responses).

---

## NVS Layout

This component does not own any NVS key. The slot record (`cam_N/camera`) is owned by `camera_manager`; bond storage is owned by NimBLE.

---

## Threading

All BLE operations (callbacks, GATT writes, response dispatch) run on the **NimBLE host task** (core 1). All `esp_timer` callbacks (readiness poll, keepalive, status poll, SetCameraControlStatus timeout) run on the **esp_timer task** (core 0). GATT writes from timer callbacks go through `ble_core_gatt_write()` which posts work back to the host task.

Driver vtable entries (`start_recording`, `stop_recording`, `get_recording_status`) may be called from any task (mismatch poll, web UI, manual override). `cached_status` is a single-enum read/write on Xtensa LX7, atomic by virtue of size; no lock is held.

---

## Protocol Notes

- **MTU exchange.** Hero11 / earlier initiate the MTU exchange themselves shortly after encryption; **Hero13 (firmware H24.x) does not.** `pairing.c` calls `ble_gattc_exchange_mtu()` explicitly — without that, the default 23-byte MTU truncates protobuf packets.
- **Channel matters for protobuf feature IDs.** Each feature lives on a fixed pair of characteristics. We use only Feature 0xF1 (COMMAND) for `SetCameraControlStatus`; sending the right action_id on the wrong channel returns nothing.
- **Pre-Hero9 BLE control** (Hero 5, Hero 7) is reportedly functional but not officially supported by Open GoPro. The capability helper `gopro_model_uses_ble_control()` only enumerates the officially-supported list. Older models can be added once verified on hardware.
