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
| `camera_manager.c` | Init, NVS load/save, driver registration table, BLE/WiFi state transitions, slot lifecycle (`register_new`, `remove_slot`, `reorder_slots`), and thin delegations of the recording-intent / can-state / sleep / teardown surface to cam_core. |
| `pair_attempt.c` | Single in-flight pair-attempt state machine + watchdog (wireless-only) |

The shared, BLE-free pieces — `camera_types.h`, `mismatch.c`, `reorder_validate.c`, the `camera_driver_t` vtable, `camera_can_state_t`, the per-slot `cam_core_slot_t` record, **the recording-intent engine, mismatch poll, broadcast dispatch, CAN-state translation, and the global auto-control flag** — now live in `components/cam_core/` (see [`components/cam_core/include/cam_core.h`](../../../components/cam_core/include/cam_core.h)). See `docs/multi-variant-restructure-plan.md` §4 for the migration plan.

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

**The recording engine now lives in `components/cam_core/`** (see `cam_core.c`). The wireless `camera_manager_set_desired_recording_*`, `_get_auto_control`, `_get_slot_can_state`, `_invoke_sleep`, and `_teardown_slot` entry points are thin wrappers around the corresponding `cam_core_*` APIs. Two dispatch paths (immediate dispatch on intent transition + 2 s mismatch poll while READY), broadcast-driver dedup, the 10 s post-shutter grace window, and `mismatch_step()` all keep their pre-split semantics.

Per-slot state owned by cam_core: `driver`, `driver_ctx`, `desired`, `grace_until_us`, `poll_timer`, `ready`, `requires_ble`. The wireless slot struct embeds a `cam_core_slot_t` and never reads or writes those fields directly — every access goes through cam_core (which holds its own mutex). The wireless mutex covers only the wireless-specific fields (`name`, `mac`, `model`, `ble_status`, `wifi_status`, `ip_addr`, `last_ip`, `wifi_associated`, `is_configured`, `first_pair_complete`, `ble_handle`).

Wireless `on_camera_ready` / `on_wifi_disconnected` / `on_camera_unresponsive` / `on_ble_disconnected_by_handle` mirror the universal "fully ready" flag into cam_core via `cam_core_slot_set_ready(idx, bool)`, which manages the mismatch poll timer.

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

Two mutexes, never held in opposite order:

- **Wireless `s_mutex`** (this file): recursive FreeRTOS mutex guarding the wireless-only slot fields — `name`, `mac`, `model`, `ble_status`, `wifi_status`, `ip_addr`, `last_ip`, `wifi_associated`, `is_configured`, `first_pair_complete`, `ble_handle`. Also held while iterating the driver-registration table.
- **cam_core mutex** (in `components/cam_core/cam_core.c`): recursive FreeRTOS mutex guarding the slot registry and every `cam_core_slot_t` field — `driver`, `driver_ctx`, `desired`, `grace_until_us`, `poll_timer`, `ready`, `requires_ble`. Driver vtable methods (`start_recording`, `stop_recording`, `get_recording_status`, `teardown`, `on_wifi_disconnected`, `update_slot_index`, `sleep`) are invoked with the cam_core mutex held — they must be non-blocking and must NOT call back into wireless code while holding any other lock.

**Lock ordering**: wireless → cam_core (one-way). Wireless code may hold `s_mutex` while calling cam_core APIs; cam_core never calls wireless functions, so there's no BA path. Driver methods (called from cam_core) post work to their own task queues and return quickly.

**Timer lifecycle**: `cam_core_slot_set_ready(idx, false)` and `cam_core_teardown_slot(idx)` delete the per-slot mismatch poll timer **outside** the cam_core mutex — `esp_timer_delete` blocks until any in-flight callback returns, and the callback itself takes the cam_core mutex. Holding the mutex during delete would deadlock. Wireless code that calls these helpers must therefore not hold `s_mutex` either, or it would block the cam_core teardown path indirectly.

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
