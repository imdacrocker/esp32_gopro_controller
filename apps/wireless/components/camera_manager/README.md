# camera_manager

Manages up to four camera slots: per-slot state (BLE and WiFi connection status, recording intent), NVS persistence, driver vtable dispatch, and the periodic mismatch-correction loop that keeps each camera's actual recording state aligned with the operator's intent.

`camera_manager` is the hub that infrastructure components (`ble_core`, `wifi_manager`) and protocol drivers (`open_gopro_ble`, `gopro_wifi_rc`, `can_manager`) all wire into. It has no direct knowledge of GoPro-specific protocols — behavioral branching by model lives in `gopro/gopro_model.h` and is called only by the driver components.

---

## Responsibilities

- Load per-camera slot records (`cam_N/camera`) from NVS on boot.
- Accept driver registrations from protocol components and assign each driver to the slots whose model it owns.
- Track coarse BLE and WiFi connection status for each slot.
- Dispatch `start_recording()` / `stop_recording()` immediately when `desired_recording` transitions, and run a per-slot periodic mismatch-correction timer that re-issues commands when the driver's cached status diverges from the operator's desired state.
- Persist `last_ip` and other slot fields to NVS on update.
- Provide slot lookup and state query functions for HTTP handlers and `can_manager`.
- Provide `is_known_addr` and `has_disconnected_cameras` predicates for `ble_core`'s scan gate.

---

## Dependencies

```
REQUIRES: cam_core, bt (ble_addr_t type), nvs_flash, esp_timer, freertos
```

**Precondition:** `nvs_flash_init()` must be called before `camera_manager_init()`.

---

## Source Files

| File | Responsibility |
|------|---------------|
| `include/camera_manager.h` | Wireless-specific API: slot info, BLE/WiFi status enums, pair-attempt types, all wireless `camera_manager_*` functions. Pulls in `cam_core.h` for the shared types/vtable. Stays NimBLE-free. |
| `include/camera_manager_ble.h` | BLE-typed extensions (currently `camera_manager_is_known_ble_addr`). Include only from sites that need NimBLE types. |
| `camera_manager.c` | Init, NVS load/save, driver registration, slot lifecycle, mismatch timer |
| `pair_attempt.c` | Single in-flight pair-attempt state machine + watchdog (wireless-only) |

The shared, BLE-free pieces (`camera_types.h`, `mismatch.c`, `reorder_validate.c`, the `camera_driver_t` vtable, `camera_can_state_t`, the per-slot `cam_core_slot_t` record) now live in `components/cam_core/` — see [`components/cam_core/include/cam_core.h`](../../../components/cam_core/include/cam_core.h). The recording-intent engine that still lives in `camera_manager.c` will move into `cam_core` in Phase 3.3 of the multi-variant restructure (`docs/multi-variant-restructure-plan.md` §4).

---

## NVS Layout

Each slot occupies namespace `cam_N` (N = 0–3). Only `camera_manager` writes the `camera` key.

```
cam_0/camera   ← camera_nv_record_t (schema version 3)
cam_1/camera
...
```

**Schema version policy:** a blob with a mismatched `version` byte is discarded and the slot is left unconfigured. Re-pairing is required. No automatic migration. **Appending new fields at the end of `camera_nv_record_t` is not a version bump** — `load_slot_from_nvs()` zero-initializes the record before `nvs_get_blob`, so smaller legacy blobs leave any new trailing fields zero.

**Save-time validation:** `camera_manager_save_slot()` returns `ESP_ERR_INVALID_ARG` and refuses to write if `model == CAMERA_MODEL_UNKNOWN`.

---

## Driver Registration (§21.4)

Drivers call `camera_manager_register_driver()` from their own `_init()` functions — `camera_manager` never imports a driver header. This keeps the dependency arrows pointing one way:

```
open_gopro_ble   ──┐
gopro_wifi_rc    ──┼──► camera_manager
can_manager      ──┘
```

On registration, `camera_manager` immediately iterates all loaded slots and assigns the new driver to any whose model satisfies the `matches()` predicate. Driver assignment for slots loaded after registration (via `camera_manager_register_new()` or `camera_manager_set_model()`) happens at those call sites.

The `requires_ble` flag controls `has_disconnected_cameras()`: RC-emulation cameras never hold a BLE connection, so they must not be counted as "disconnected" for the BLE background scan gate.

---

## Recording State Machine (§13)

Each slot carries two independent pieces of state:

| Field | Source | Meaning |
|-------|--------|---------|
| `desired_recording` | CAN `0x600` frames or web UI | What the operator wants |
| `get_recording_status()` | Driver cache (non-blocking) | What the camera is actually doing |

**Two dispatch paths** keep the camera aligned with intent:

1. **Immediate dispatch** — `set_desired_recording_all()` / `_slot()` send `start_recording()` / `stop_recording()` directly when the new intent differs from the previous value, the slot is `WIFI_CAM_READY`, and the intent is not `UNKNOWN`. This is the happy path for shutter latency: the BLE/UDP write goes out as soon as the API call returns rather than waiting for the next poll tick. Idempotent calls (same intent as before) are a no-op so `can_manager`'s per-frame `0x600` updates don't hammer the cameras. `set_desired_recording_all` also dedups *broadcast* drivers — see "Broadcast drivers" below.
2. **Mismatch-correction poll** — fires every **2 s** while a slot is `WIFI_CAM_READY`. Catches slots that weren't ready when the API was called, slots whose real status diverges from the commanded state, and any case where the immediate dispatch was suppressed. On each tick:

```
desired == UNKNOWN                → no-op (boot state, no intent yet)
actual  == UNKNOWN                → no-op (camera state not yet known)
now < grace_until_us              → no-op (recent command still settling)
desired == START, actual == IDLE   → start_recording(); arm grace deadline
desired == STOP, actual == ACTIVE  → stop_recording(); arm grace deadline
```

Both paths arm `grace_until_us = now + RECORDING_STATUS_GRACE_MS` (10 s) after issuing a command. The deadline expires on its own — the poll does not reset it per tick. This is long enough that both BLE status notifications and the WiFi RC `st` poll have caught up before the next mismatch comparison runs, so we don't re-issue a Start to a camera that's already recording but hasn't yet told us. The per-cycle reset behaviour was replaced after observing late status updates from Hero4-class cameras under load.

`mismatch_step()` is a pure function in `mismatch.c` — no side effects, no ESP-IDF headers — and can be unit-tested on the host (§23.2). It still takes the grace-period state as a `bool`; the deadline-vs-now comparison happens at the call site. The immediate-dispatch path keys off the intent transition itself and does not consult cached status.

### Broadcast drivers

If a `camera_driver_t` sets `broadcasts_to_all = true`, `set_desired_recording_all()` calls the driver's `start_recording_all()` / `stop_recording_all()` **once per dispatch wave** (at the slot index of the first match it encounters), rather than calling the per-slot vtable entries for every slot owned by that driver. Other slots using the same driver still have their `desired_recording` and grace deadline updated — they just don't trigger another driver call, since the broadcast already covered them. Slot order in the dispatch list is preserved, so any non-broadcast (e.g. BLE) slots interleaved with broadcast (e.g. RC) slots fire in their natural order.

The mismatch poll and `set_desired_recording_slot` always go through the per-slot vtable entries (`start_recording` / `stop_recording`) regardless of `broadcasts_to_all`, so single-camera commands stay targeted and don't disturb peers.

---

## Slot Lifecycle

```
camera_manager_init()
  → load NVS records; all BLE/WiFi statuses = NONE

camera_manager_register_new(mac)    [called by open_gopro_ble on first encrypted connection]
  → allocates slot; model = UNKNOWN; no NVS write

camera_manager_set_model(slot, model)
  → assigns matching registered driver; open_gopro_ble then calls camera_manager_save_slot()

camera_manager_on_ble_connected(slot, conn_handle)  → CAM_BLE_CONNECTED
camera_manager_on_ble_ready(slot)                   → CAM_BLE_READY

camera_manager_on_camera_ready(slot)                 → WIFI_CAM_READY + start mismatch timer
  (called by the driver after its readiness sequence completes —
   for BLE-control: GetHardwareInfo + SetCameraControlStatus;
   for RC-emulation: first UDP datagram received from the camera.)

camera_manager_on_wifi_disconnected(slot)            → WIFI_CAM_NONE  + stop mismatch timer
  (RC-emulation only — BLE-control cameras use BLE link supervision instead.)

camera_manager_remove_slot(slot)
  → stop timer, teardown driver, erase NVS, compact slot array, notify drivers of new indices
```

The `WIFI_CAM_READY` enum name is retained for both transports — historical from when only RC-emulation existed. For BLE-control cameras it simply means "ready for recording commands".

---

## Public API

Header: `include/camera_manager.h` (wireless-specific) — re-exports the cam_core types via `#include "cam_core.h"`.  
Shared types: [`components/cam_core/include/camera_types.h`](../../../components/cam_core/include/camera_types.h) (model IDs, recording/intent/mismatch enums).  
Shared vtable + per-slot state: [`components/cam_core/include/cam_core.h`](../../../components/cam_core/include/cam_core.h).

### Init

```c
void camera_manager_init(void);
```

Load all `cam_N/camera` NVS records. Must be called before any other function and before any driver `_init()`.

---

### Driver Registration

```c
esp_err_t camera_manager_register_driver(
    const camera_driver_t *driver,
    camera_model_match_fn   matches,
    camera_ctx_create_fn    create_ctx,
    bool                    requires_ble
);
```

Register a driver. `matches(model)` is called for each slot to decide ownership. `create_ctx(slot)` allocates a per-slot driver context. `requires_ble` controls `has_disconnected_cameras()`. Returns `ESP_ERR_NO_MEM` if the registration table (4 entries) is full.

---

### Slot Lookup

```c
int camera_manager_find_by_mac(const uint8_t mac[6]);
```

Linear search of configured slots by raw 6-byte MAC. Returns slot index or `-1`. Used by both WiFi callbacks (station MAC) and BLE callbacks (BLE address bytes).

For BLE-control cameras the stored `mac` is the BLE peer MAC; for RC-emulation cameras it is the WiFi MAC (Hero 4 has no BLE radio). The two families don't overlap, so a single field is sufficient.

```c
int camera_manager_register_new(const uint8_t mac[6]);
```

Allocate a new slot for an unknown camera. Does not write NVS. Returns slot index or `-1` if all four slots are occupied.

---

### BLE State Transitions

```c
void camera_manager_on_ble_connected(int slot, uint16_t conn_handle);
void camera_manager_on_ble_ready(int slot);
void camera_manager_on_ble_disconnected_by_handle(uint16_t conn_handle);
```

`on_ble_disconnected_by_handle` locates the slot by handle — call it **after** any per-slot BLE cleanup that needs the handle (e.g. stopping timers) but before the higher layer forgets the handle.

---

### Slot Field Updates

```c
void camera_manager_set_model(int slot, camera_model_t model);
void camera_manager_set_name(int slot, const char *name);
```

---

### Camera-Ready Transition

```c
void camera_manager_on_camera_ready(int slot);
void camera_manager_on_wifi_disconnected(int slot);
void camera_manager_on_station_ip(const uint8_t mac[6], uint32_t ip);
```

`on_camera_ready` sets `WIFI_CAM_READY` and starts the mismatch poll timer. Called by the driver after its own readiness sequence completes:

- **BLE-control**: after `GetHardwareInfo` returns success and `SetCameraControlStatus(EXTERNAL)` is acked (or its 3 s timeout fires).
- **RC-emulation**: on the first UDP datagram received from the camera (keepalive ACK, `st`, `SH`, or `cv` reply).  Identification (model + display name) is settled separately and asynchronously by the `cv` response — see `gopro_wifi_rc/README.md` for details.

`on_wifi_disconnected` stops the timer and resets `wifi_status` to `WIFI_CAM_NONE`. RC-emulation only — BLE-control cameras don't use the SoftAP, so connection loss surfaces as a BLE disconnect instead.

`on_station_ip` updates `last_ip` and the `wifi_associated` flag for any configured slot whose `mac` matches (called from the wifi_manager DHCP callback). RC-emulation only.

---

### NVS

```c
esp_err_t camera_manager_save_slot(int slot);
```

Serialize the slot to `cam_N/camera`. Returns `ESP_ERR_INVALID_ARG` if `model == CAMERA_MODEL_UNKNOWN`.

---

### Queries

```c
uint32_t           camera_manager_get_last_ip(int slot);
camera_model_t     camera_manager_get_model(int slot);
int                camera_manager_get_slot_count(void);
int                camera_manager_get_configured_count(void);
esp_err_t          camera_manager_get_slot_info(int slot, camera_slot_info_t *out);
camera_can_state_t camera_manager_get_slot_can_state(int slot);
```

`get_slot_count` is an **exclusive iteration bound** (highest configured slot index + 1), not a camera count: a partial NVS load can leave unconfigured gaps below it, so callers iterating slots must skip entries where `is_configured` is false. Use `get_configured_count` when you need the actual number of cameras.

`get_slot_info` copies all display-relevant state into `camera_slot_info_t` (used by HTTP handlers). `is_recording` is derived inline: `wifi_status == READY && driver->get_recording_status() == ACTIVE`.

`get_slot_can_state` translates slot state to the four `CAMERA_CAN_STATE_*` values used in the CAN `0x601` broadcast. Called by `can_manager` when building the payload.

---

### Recording Intent

```c
void camera_manager_set_desired_recording_all(desired_recording_t intent);
void camera_manager_set_desired_recording_slot(int slot, desired_recording_t intent);
bool camera_manager_get_auto_control(void);
void camera_manager_set_auto_control(bool enabled);
```

`set_desired_recording_all` is called by `can_manager` on every received `0x600` frame (idempotent by design). `set_desired_recording_slot` is used by HTTP handlers for manual per-camera control when `auto_control == false`.

Both functions dispatch `start_recording()` / `stop_recording()` synchronously on a real intent transition when the slot is `WIFI_CAM_READY`. Repeated calls with the same intent are no-ops. Slots that aren't ready (or that were called with `DESIRED_RECORDING_UNKNOWN`) are picked up by the mismatch poll once they become ready. See **Recording State Machine** above for the full two-path model.

---

### Slot Removal

```c
esp_err_t camera_manager_remove_slot(int slot);
```

Stop the mismatch timer, call `driver->teardown()`, erase NVS for the slot, compact the array (slots above shift down by one), update NVS for compacted slots, and notify each driver of its new index via `driver->update_slot_index()`.

**CAN output changes immediately** after compaction — byte positions in the `0x601` frame shift.

---

### ble_core Callbacks

```c
/* camera_manager.h */
bool camera_manager_has_disconnected_cameras(void);

/* camera_manager_ble.h (BLE-typed; include separately) */
bool camera_manager_is_known_ble_addr(ble_addr_t addr);
```

Pass these as function pointers when constructing `ble_core_callbacks_t` in `open_gopro_ble_init()`. `is_known_addr` gates auto-reconnect in the background scan handler. `has_disconnected_cameras` gates whether the background scan is started at all — it only counts slots registered with `requires_ble == true`, so RC-emulation cameras (which never use BLE) do not keep the scanner running permanently.

`camera_manager_is_known_ble_addr` lives in the sibling `camera_manager_ble.h` header so that `camera_manager.h` itself stays free of NimBLE includes (step toward the BLE-free shared `cam_core` component — see `docs/multi-variant-restructure-plan.md` §4).

---

## Key Types

### `camera_driver_t`

```c
struct camera_driver {
    /* Per-slot — used by mismatch poll and set_desired_recording_slot, and by
     * set_desired_recording_all when broadcasts_to_all is false. */
    esp_err_t                 (*start_recording)(void *ctx);
    esp_err_t                 (*stop_recording)(void *ctx);
    camera_recording_status_t (*get_recording_status)(void *ctx); // non-blocking cache read
    void                      (*teardown)(void *ctx);              // nullable
    void                      (*update_slot_index)(void *ctx, int new_slot); // nullable
    void                      (*on_wifi_disconnected)(void *ctx);  // nullable; RC-emulation only

    /* Broadcast — only used by set_desired_recording_all when this flag is
     * true.  start_recording_all/stop_recording_all fire once per dispatch
     * wave; subsequent slots using the same driver have their intent + grace
     * deadline updated but skip the call. */
    bool                       broadcasts_to_all;
    esp_err_t                (*start_recording_all)(void);
    esp_err_t                (*stop_recording_all)(void);
};
```

`get_recording_status` must be non-blocking — it is called from the mismatch timer callback while the slot mutex is held.

`on_wifi_disconnected` is set to `NULL` by the BLE-control driver since those cameras never associate to the SoftAP; the `gopro_wifi_rc` driver also leaves it `NULL` and tracks SoftAP station events through its own public API. The hook is retained for future drivers that may need it.

### `camera_slot_info_t`

```c
typedef struct {
    int               index;
    char              name[32];
    camera_model_t    model;
    uint8_t           mac[6];
    bool              is_configured;
    cam_ble_status_t  ble_status;
    wifi_cam_status_t wifi_status;
    bool              is_recording;
    desired_recording_t desired_recording;
    uint32_t          ip_addr;
} camera_slot_info_t;
```

### `camera_can_state_t`

| Value | Meaning |
|-------|---------|
| `CAMERA_CAN_STATE_UNDEFINED` (0) | Slot not configured |
| `CAMERA_CAN_STATE_DISCONNECTED` (1) | Configured but not on network |
| `CAMERA_CAN_STATE_IDLE` (2) | Connected, not recording |
| `CAMERA_CAN_STATE_RECORDING` (3) | Connected and actively recording |

Values map directly to RaceCapture direct-CAN channel bytes and must not be reordered.

---

## Threading

A single FreeRTOS mutex (`s_mutex`) guards the slot array. Both the mismatch timer callback and the immediate-dispatch path in `set_desired_recording_*` acquire the mutex briefly to read/update state and arm the grace deadline, then release the mutex before issuing `start_recording()` / `stop_recording()` (or `start_recording_all()` / `stop_recording_all()`) to avoid holding the lock during a potentially slow driver call.

`stop_poll_timer()` is always called **before** acquiring the mutex (in `on_wifi_disconnected` and `remove_slot`) because the timer callback itself acquires the mutex — holding it while stopping the timer would deadlock if a callback fired concurrently.

---

## Error Handling

| Scenario | Behaviour |
|----------|-----------|
| NVS schema version mismatch | Slot discarded; re-pairing required; logged as warning |
| NVS read error (non-not-found) | Slot left unconfigured; logged as warning |
| NVS write error | Logged as error; in-RAM state unaffected |
| `save_slot` with `model == UNKNOWN` | Returns `ESP_ERR_INVALID_ARG`; logged as error |
| `register_driver` table full | Returns `ESP_ERR_NO_MEM`; logged as error |
| No driver found for a loaded slot's model | Logged as warning; slot operational but recording commands are no-ops |
| Poll timer create failure | Logged as error; slot stays at `WIFI_CAM_READY` but no mismatch correction runs |
| `remove_slot` on out-of-range index | Returns `ESP_ERR_INVALID_ARG` |
