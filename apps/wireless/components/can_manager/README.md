# can_manager

Manages the TWAI (CAN) bus interface. Receives logging commands, GPS timestamps, and shutdown requests from a RaceCapture data logger; broadcasts camera status back at 5 Hz.

The four CAN identifiers are user-configurable from the web UI (per channel `(ide, id)`). The defaults shipped from the factory — `0x600` / `0x601` / `0x602` / `0x603`, all standard 11-bit — are referenced throughout this README; substitute whatever the operator configured if those have been changed. See [`docs/design/can-id-configuration.md`](../../../../docs/design/can-id-configuration.md).

---

## Responsibilities

- **Logging command (default `0x600`)**: On every received frame, update `s_logging_state` and fire the `on_logging_state` callback with `LOGGING_STATE_LOGGING` or `LOGGING_STATE_NOT_LOGGING`. When `camera_manager_get_auto_control()` is true, also update `desired_recording` for all camera slots by calling `camera_manager_set_desired_recording_all()`. When auto-control is off, the bus state is still reported but the call into `camera_manager` is skipped so per-slot intent set via `/api/shutter` is preserved.
- **5 s watchdog**: If no logging-command frame arrives within 5 seconds, revert `s_logging_state` to `LOGGING_STATE_UNKNOWN`. When auto-control is on, also call `camera_manager_set_desired_recording_all(DESIRED_RECORDING_UNKNOWN)` to suppress mismatch correction until the bus recovers; when auto-control is off the slot intents are left alone so manual recording state (and its mismatch-correction safety net) survives the dropout. The `on_logging_state` callback is **not** fired for the `UNKNOWN` transition.
- **GPS UTC timestamp (default `0x602`)**: Parse the 64-bit little-endian Unix epoch (ms). Fire `on_utc_acquired` exactly once per boot session, on the first valid live source — either a GPS frame (year > 2020) or a successful `can_manager_set_manual_utc_ms()` call. Subsequent frames update the stored epoch used for clock extrapolation. Each live update also calls `settimeofday()` so libc time APIs in other components return useful values.
- **Shutdown request (default `0x603`)**: On every received frame where byte 0 is non-zero, fire the `on_shutdown_request` callback. Idempotent — repeated frames during `SHUTTING_DOWN`/`SHUTDOWN_COMPLETE` are de-duped by the shutdown_manager. See [`docs/design/shutdown.md`](../../../../docs/design/shutdown.md).
- **Camera status TX (default `0x601`)**: Transmit camera states for all 4 slots at 5 Hz via a periodic `esp_timer`. Slot values read from `camera_manager_get_slot_can_state()`. The TX header's `ide` flag follows the configured channel — extended-ID configurations transmit 29-bit frames.
- **Per-channel ID configuration**: Boot-time-frozen `s_channels[CAN_CH_COUNT]` array loaded from NVS, with defaults substituted for missing keys. `can_manager_set_channel()` only writes NVS; the running dispatch table and TX header are not mutated mid-session, so changes apply only after reboot.
- **Bitrate**: Persist a CAN bitrate in NVS, loaded at init, applied to the TWAI driver. Allowed values: 50k / 100k / 125k / 250k / 500k / 1M bps. Applies on next boot.
- **Timezone offset**: Persist a UTC-to-local hour offset in NVS, loaded at init, applied when setting camera date/time.
- **UTC persistence across boots**: Save the current best estimate of UTC to NVS — immediately on first session sync, and every 5 minutes thereafter. On `can_manager_init()` the saved value is restored into the in-memory anchor and pushed to the system clock, but is **not** treated as a live sync (so camera `SetDateTime` is still deferred until a real GPS / web-UI sync occurs).

---

## Dependencies

```
REQUIRES: camera_manager, shutdown_manager, esp_driver_twai, esp_timer, freertos, nvs_flash
```

**Precondition:** `camera_manager_init()` and `shutdown_manager_init()` must be called before `can_manager_init()`.  
`can_manager_register_callbacks()` must be called before `can_manager_init()`.

---

## Source Files

| File | Responsibility |
|------|---------------|
| `include/can_manager.h` | Public API: types, callback typedefs, init, state queries, bitrate + channel config, timezone offset, manual UTC entry |
| `can_manager.c` | TWAI node setup, ISR RX callback, RX task with 4-entry channel-lookup dispatch, TX timer, watchdog, UTC anchor + system-clock sync, NVS persistence (tz offset, last UTC, bitrate, per-channel IDs) |

---

## Public API

Header: `include/can_manager.h`

```c
/* Callback registration — call before can_manager_init() */
void can_manager_register_callbacks(const can_manager_callbacks_t *cbs);

/* Start TWAI node, RX task, TX timer, and watchdog */
void can_manager_init(void);

/* Current logging state — safe to call from any task */
can_logging_state_t can_manager_get_logging_state(void);

/* Estimated current UTC in ms; returns false only when no anchor exists at all
 * (no GPS, no manual set, and no NVS-restored value).  Anchor sources include
 * the NVS-restored boot value, so this can return true before any live sync. */
bool can_manager_get_utc_ms(uint64_t *out_ms);

/* True only after a live source — GPS frame or manual web-UI set — has won
 * this boot session.  Used by camera drivers to gate SetDateTime; an
 * NVS-restored anchor never counts as session-synced. */
bool can_manager_utc_is_session_synced(void);

/* Manually set UTC from the web UI.  Returns ESP_ERR_INVALID_STATE if a live
 * source has already won this session (NVS-restored values do not block). */
esp_err_t can_manager_set_manual_utc_ms(uint64_t utc_ms);

/* CAN bitrate (bps).  set validates against the allowed list; persists to
 * NVS, applies on next boot. */
uint32_t  can_manager_get_bitrate(void);
esp_err_t can_manager_set_bitrate(uint32_t bitrate_bps);

/* Per-channel CAN identifier (ide + id).  set validates range + IDE but does
 * NOT check cross-channel collisions — that's the API handler's job.
 * Persists to NVS, applies on next boot.  See docs/design/can-id-configuration.md. */
esp_err_t can_manager_get_channel(can_channel_id_t ch, can_channel_t *out);
esp_err_t can_manager_set_channel(can_channel_id_t ch, can_channel_t value);
can_channel_t can_manager_channel_default(can_channel_id_t ch);

/* Timezone offset in NVS — clamped to IANA range [−12, +14] */
void   can_manager_set_tz_offset(int8_t hours);
int8_t can_manager_get_tz_offset(void);
```

### Callback Types

```c
/* Every received frame — for bus sniffing / development */
typedef void (*can_rx_frame_cb_t)(uint32_t id, const uint8_t *data,
                                   uint8_t len, void *arg);

/* Every logging-command frame; never called with LOGGING_STATE_UNKNOWN */
typedef void (*can_logging_state_cb_t)(can_logging_state_t state, void *arg);

/* Exactly once per boot session, on the first live UTC source — either a
 * valid GPS UTC frame or a successful can_manager_set_manual_utc_ms() call.
 * NVS-restored UTC at boot does NOT fire this callback. */
typedef void (*can_utc_acquired_cb_t)(uint64_t utc_ms, void *arg);

/* Every shutdown-request frame with byte0 != 0.  Idempotent — repeated
 * frames during SHUTTING_DOWN/SHUTDOWN_COMPLETE collapse into no-ops at
 * the shutdown_manager layer. */
typedef void (*can_shutdown_request_cb_t)(void *arg);
```

All callbacks are invoked from the RX task context. Implementations must not block indefinitely.

---

## CAN Protocol

All four CAN channels are independently configurable as Standard 11-bit or Extended 29-bit. The defaults below are what ships from the factory; user values are loaded from NVS at boot.

| Default ID | Direction | Description | Channel enum |
|----|-----------|-------------|---|
| `0x600` std | RaceCapture → ESP32 | Byte 0: `isLogging` flag (non-zero = logging) | `CAN_CH_LOGGING_CMD` |
| `0x601` std | ESP32 → RaceCapture | Bytes 0–3: `CAMERA_CAN_STATE_*` for Cam 1, Cam 2, Cam 3, Cam 4 respectively, transmitted at 5 Hz | `CAN_CH_CAM_STATUS` |
| `0x602` std | RaceCapture → ESP32 | 64-bit little-endian Unix epoch in milliseconds (GPS UTC, 25 Hz once locked) | `CAN_CH_GPS_UTC` |
| `0x603` std | RaceCapture → ESP32 | Byte 0 non-zero = request shutdown. Idempotent. | `CAN_CH_SHUTDOWN_REQ` |

RX dispatch matches the full `(id, ide)` tuple — a Standard `0x600` and an Extended `0x600` are distinct frames and reach distinct (or no) handlers. The bitrate is configurable independently; both bitrate and channel IDs apply on next boot.

### Camera Status (default `0x601`) State Values

```
CAMERA_CAN_STATE_UNDEFINED    = 0   Slot not configured
CAMERA_CAN_STATE_DISCONNECTED = 1   Camera not found or connection lost
CAMERA_CAN_STATE_IDLE         = 2   Connected, not recording
CAMERA_CAN_STATE_RECORDING    = 3   Connected and actively recording
```

---

## NVS Persistence

Namespace `can_mgr`:

| Key | Type | Purpose |
|---|---|---|
| `tz_off` | `i8` | UTC offset in whole hours, clamped to IANA range `[−12, +14]` |
| `last_utc` | `u64` | Most recent best-estimate UTC (ms) for cross-boot continuity |
| `bitrate` | `u32` | CAN bitrate in bps (50k / 100k / 125k / 250k / 500k / 1M) |
| `ch_logging` | `u32` | Packed channel: bit 31 = IDE flag (0=std, 1=ext), bits 0–28 = id |
| `ch_status` | `u32` | Same packing — camera status TX channel |
| `ch_utc` | `u32` | Same packing — GPS UTC RX channel |
| `ch_shut` | `u32` | Same packing — shutdown request RX channel |

All channel and bitrate keys apply on next boot; the running state is frozen for the boot session.

---

## Hardware

| Parameter | Value |
|-----------|-------|
| TX GPIO | 7 |
| RX GPIO | 6 |
| Default baud rate | 1 Mbps (configurable per "NVS Persistence" above) |
| TX queue depth | 8 frames |
| Termination | Hardware (120 Ω solder jumpers, enabled by default) |

---

## Threading Model

| Context | Role |
|---------|------|
| TWAI ISR (`on_rx_done`) | Reads frame via `twai_node_receive_from_isr()`, enqueues to `s_rx_queue` |
| `can_rx` task — priority 5, core 1 | Dequeues frames, dispatches handlers, fires callbacks |
| `esp_timer` (`can_tx`) | Builds and transmits the camera status channel at 5 Hz |
| `esp_timer` (`can_watchdog`) | One-shot 5 s; resets to `UNKNOWN` if no logging-command frame arrives |
| `esp_timer` (`can_utc_save`) | Periodic 5 min UTC save to NVS once the anchor is valid |
| `can_recov` task — priority 4, core 1 | Drains bus-state queue, calls `twai_node_recover()` on BUS_OFF entries |

`s_logging_state` is a `volatile` enum (single-word, aligned — atomic on Xtensa LX7). The UTC timestamp struct is multi-field and protected by a mutex.
