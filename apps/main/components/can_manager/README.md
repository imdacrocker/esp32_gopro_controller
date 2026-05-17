# can_manager

Manages the TWAI (CAN) bus interface. Receives logging commands and GPS timestamps from a RaceCapture data logger; broadcasts camera status back at 5 Hz.

---

## Responsibilities

- **0x600 logging command**: On every received frame, update `s_logging_state` and fire the `on_logging_state` callback with `LOGGING_STATE_LOGGING` or `LOGGING_STATE_NOT_LOGGING`. When `camera_manager_get_auto_control()` is true, also update `desired_recording` for all camera slots by calling `camera_manager_set_desired_recording_all()`. When auto-control is off, the bus state is still reported but the call into `camera_manager` is skipped so per-slot intent set via `/api/shutter` is preserved.
- **5 s watchdog**: If no `0x600` frame arrives within 5 seconds, revert `s_logging_state` to `LOGGING_STATE_UNKNOWN`. When auto-control is on, also call `camera_manager_set_desired_recording_all(DESIRED_RECORDING_UNKNOWN)` to suppress mismatch correction until the bus recovers; when auto-control is off the slot intents are left alone so manual recording state (and its mismatch-correction safety net) survives the dropout. The `on_logging_state` callback is **not** fired for the `UNKNOWN` transition.
- **0x602 GPS UTC timestamp**: Parse the 64-bit little-endian Unix epoch (ms). Fire `on_utc_acquired` exactly once per boot session, on the first valid live source — either a GPS frame (year > 2020) or a successful `can_manager_set_manual_utc_ms()` call. Subsequent frames update the stored epoch used for clock extrapolation. Each live update also calls `settimeofday()` so libc time APIs in other components return useful values.
- **0x601 camera status TX**: Transmit camera states for all 4 slots at 5 Hz via a periodic `esp_timer`. Slot values read from `camera_manager_get_slot_can_state()`.
- **Timezone offset**: Persist a UTC-to-local hour offset in NVS, loaded at init, applied when setting camera date/time.
- **UTC persistence across boots**: Save the current best estimate of UTC to NVS — immediately on first session sync, and every 5 minutes thereafter. On `can_manager_init()` the saved value is restored into the in-memory anchor and pushed to the system clock, but is **not** treated as a live sync (so camera `SetDateTime` is still deferred until a real GPS / web-UI sync occurs).

---

## Dependencies

```
REQUIRES: camera_manager, esp_driver_twai, esp_timer, freertos, nvs_flash
```

**Precondition:** `camera_manager_init()` must be called before `can_manager_init()`.  
`can_manager_register_callbacks()` must be called before `can_manager_init()`.

---

## Source Files

| File | Responsibility |
|------|---------------|
| `include/can_manager.h` | Public API: types, callback typedefs, init, state queries, timezone offset, manual UTC entry |
| `can_manager.c` | TWAI node setup, ISR RX callback, RX task, TX timer, watchdog, UTC anchor + system-clock sync, NVS persistence (tz offset + last UTC, periodic save) |

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

/* Timezone offset in NVS — clamped to IANA range [−12, +14] */
void   can_manager_set_tz_offset(int8_t hours);
int8_t can_manager_get_tz_offset(void);
```

### Callback Types

```c
/* Every received frame — for bus sniffing / development */
typedef void (*can_rx_frame_cb_t)(uint32_t id, const uint8_t *data,
                                   uint8_t len, void *arg);

/* Every 0x600 frame; never called with LOGGING_STATE_UNKNOWN */
typedef void (*can_logging_state_cb_t)(can_logging_state_t state, void *arg);

/* Exactly once per boot session, on the first live UTC source — either a
 * valid 0x602 frame or a successful can_manager_set_manual_utc_ms() call.
 * NVS-restored UTC at boot does NOT fire this callback. */
typedef void (*can_utc_acquired_cb_t)(uint64_t utc_ms, void *arg);
```

All callbacks are invoked from the RX task context. Implementations must not block indefinitely.

---

## CAN Protocol

All frames use standard 11-bit IDs at 1 Mbps.

| ID | Direction | Description |
|----|-----------|-------------|
| `0x600` | RaceCapture → ESP32 | Byte 0: `isLogging` flag (non-zero = logging) |
| `0x601` | ESP32 → RaceCapture | Bytes 0–3: `CAMERA_CAN_STATE_*` for Cam 1, Cam 2, Cam 3, Cam 4 respectively, transmitted at 5 Hz |
| `0x602` | RaceCapture → ESP32 | 64-bit little-endian Unix epoch in milliseconds (GPS UTC, 25 Hz once locked) |

### 0x601 Camera State Values

```
CAMERA_CAN_STATE_UNDEFINED    = 0   Slot not configured
CAMERA_CAN_STATE_DISCONNECTED = 1   Camera not found or connection lost
CAMERA_CAN_STATE_IDLE         = 2   Connected, not recording
CAMERA_CAN_STATE_RECORDING    = 3   Connected and actively recording
```

---

## Hardware

| Parameter | Value |
|-----------|-------|
| TX GPIO | 7 |
| RX GPIO | 6 |
| Baud rate | 1 Mbps |
| TX queue depth | 8 frames |
| Termination | Hardware (120 Ω solder jumpers, enabled by default) |

---

## Threading Model

| Context | Role |
|---------|------|
| TWAI ISR (`on_rx_done`) | Reads frame via `twai_node_receive_from_isr()`, enqueues to `s_rx_queue` |
| `can_rx` task — priority 5, core 1 | Dequeues frames, dispatches handlers, fires callbacks |
| `esp_timer` (`can_tx`) | Builds and transmits 0x601 at 5 Hz |
| `esp_timer` (`can_watchdog`) | One-shot 5 s; resets to `UNKNOWN` if no 0x600 arrives |

`s_logging_state` is a `volatile` enum (single-word, aligned — atomic on Xtensa LX7). The UTC timestamp struct is multi-field and protected by a mutex.
