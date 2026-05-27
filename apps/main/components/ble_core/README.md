# ble_core

Manages the NimBLE host stack for the GoPro controller. Handles background reconnect scanning, user-initiated discovery, connection lifecycle, SMP encryption/bonding, GATT notifications, and bond management. Higher-level protocol drivers (e.g., `open_gopro_ble`) sit on top and never call NimBLE directly.

---

## Responsibilities

- Start the NimBLE host task on core 1 and signal callers when the stack is ready (`on_sync`).
- Run a passive background scan that auto-connects to known (previously bonded) cameras.
- Run a user-initiated discovery scan that forwards all advertisements to the caller.
- Manage connection lifecycle: connect → initiate encryption → notify caller.
- Handle bonding: Just Works pairing, NVS persistence, stale-bond detection and purge.
- Receive GATT notifications and forward them to the caller.
- Post GATT Write Requests (write-with-response) from any task safely onto the NimBLE event queue.

---

## Dependencies

```
REQUIRES: bt, nvs_flash
```

**Precondition:** `nvs_flash_init()` must be called before `ble_core_init()`.

`ble_core_register_callbacks()` must be called before `ble_core_init()` if any callbacks are needed.

`ble_core_purge_unknown_bonds()` must be called before `ble_core_init()` (NimBLE bond store is not safe to modify after the host starts).

---

## Source Files

| File | Responsibility |
|------|---------------|
| `ble_init.c` | NimBLE init, SMP config, host task, `on_sync`/`on_reset` |
| `ble_scan.c` | Background/discovery scan, connect-by-address, scan state machine |
| `ble_connect.c` | GAP event handler (connect, disconnect, encrypt, notify, repeat-pairing), bond purge/remove |
| `ble_gatt_write.c` | Async GATT Write Request (write-with-response) |
| `ble_core_internal.h` | Shared state and internal function declarations |

---

## Public API

Header: `include/ble_core.h`

### Callbacks

All callbacks fire on the **NimBLE host task**. They must not block indefinitely.

```c
// Advertisement received during discovery scan
typedef void (*ble_core_on_disc_cb_t)(
    ble_addr_t addr, int8_t rssi,
    const uint8_t *data, int len
);

// Link established (pre-encryption)
typedef void (*ble_core_on_connected_cb_t)(uint16_t conn_handle, ble_addr_t addr);

// Encryption established or resumed from stored bond
typedef void (*ble_core_on_encrypted_cb_t)(uint16_t conn_handle, ble_addr_t addr);

// Connection terminated
typedef void (*ble_core_on_disconnected_cb_t)(
    uint16_t conn_handle, ble_addr_t addr, uint8_t reason
);

// GATT notification received
typedef void (*ble_core_on_notify_rx_cb_t)(
    uint16_t conn_handle, uint16_t attr_handle,
    const uint8_t *data, uint16_t len
);

// Return true if addr is a paired (known) camera — gates auto-reconnect
typedef bool (*ble_core_is_known_addr_cb_t)(ble_addr_t addr);

// Return true if any paired camera is currently disconnected — gates background scan
typedef bool (*ble_core_has_disconnected_cameras_cb_t)(void);
```

### Callback Registration

```c
typedef struct {
    ble_core_on_disc_cb_t                   on_disc;
    ble_core_on_connected_cb_t              on_connected;
    ble_core_on_encrypted_cb_t              on_encrypted;
    ble_core_on_disconnected_cb_t           on_disconnected;
    ble_core_on_notify_rx_cb_t              on_notify_rx;
    ble_core_is_known_addr_cb_t             is_known_addr;
    ble_core_has_disconnected_cameras_cb_t  has_disconnected_cameras;
} ble_core_callbacks_t;

void ble_core_register_callbacks(const ble_core_callbacks_t *cbs);
```

Register the callback table. All fields are optional; pass `NULL` for any callback that is not needed. Must be called before `ble_core_init()`.

---

### Functions

---

#### `ble_core_init`

```c
void ble_core_init(void);
```

Initialise the NimBLE port, configure SMP (Just Works, bonding enabled, full key distribution), initialise the GAP service, set the device name to `"ESP32 Controller"`, and spawn the NimBLE host task on core 1.

Returns immediately. The stack is not yet usable — wait for the `on_sync` callback before calling any scan or connect functions. `on_sync` fires `start_scan_if_needed()` automatically, so background scanning begins without any additional call.

---

#### `ble_core_start_discovery`

```c
void ble_core_start_discovery(int timeout_ms);
```

Start a user-initiated discovery scan. Every advertisement received during `timeout_ms` is forwarded to `on_disc`. Cancels any running background scan first.

Async — posts to the NimBLE event queue. Returns `void`; if the alloc fails the scan is not started (logged as an error).

---

#### `ble_core_stop_discovery`

```c
void ble_core_stop_discovery(void);
```

Cancel a running discovery scan and resume background scanning if cameras are disconnected. Safe to call even if no discovery scan is running.

Async — posts to the NimBLE event queue.

---

#### `ble_core_connect_by_addr`

```c
esp_err_t ble_core_connect_by_addr(const ble_addr_t *addr);
```

Initiate a connection to `addr`. Cancels any running scan first.

Async — posts to the NimBLE event queue. Returns `ESP_ERR_NO_MEM` if the event could not be allocated; returns `ESP_OK` otherwise (the connection attempt may still fail asynchronously, in which case `on_disconnected` or a GAP connect-failed event fires).

---

#### `ble_core_gatt_write`

```c
esp_err_t ble_core_gatt_write(
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    uint16_t len
);
```

Send a GATT Write Request (ATT Write-with-Response) to the attribute at `attr_handle` on the connection identified by `conn_handle`. The OpenGoPro spec lists the command/settings/query/net-mgmt characteristics as plain "Write"; Hero13 and later silently drop ATT Write Commands on those handles, so the request form is required. The ATT-level write response only confirms receipt — application-level responses still arrive asynchronously via GATT notifications.

Safe to call from any task (HTTP handler, `camera_manager` timers, CAN RX task). Async — copies `data` into a heap buffer and posts to the NimBLE event queue. Returns `ESP_ERR_NO_MEM` if allocation fails.

---

#### `ble_core_purge_unknown_bonds`

```c
void ble_core_purge_unknown_bonds(const ble_addr_t keep[], int keep_count);
```

Delete any NVS bond whose address is **not** present in `keep[]`. Call once at startup (before `ble_core_init()`) to remove stale bonds when cameras are removed from `camera_manager`.

**Not thread-safe.** Must be called before `ble_core_init()`.

---

#### `ble_core_remove_bond`

```c
void ble_core_remove_bond(const ble_addr_t *addr);
```

Terminate any active connection to `addr` and delete its NVS bond. Safe to call from any task after `ble_core_init()`.

Async — posts to the NimBLE event queue.

---

#### `ble_core_resume_background_scan`

```c
void ble_core_resume_background_scan(void);
```

Re-enter the background reconnect-scan state. No-op if a connect attempt is already in progress, a discovery scan is running, or `has_disconnected_cameras()` returns false.

A successful first-camera connection does not, on its own, restart the background scan — the GAP `CONNECT` event simply clears `s_connecting`. Higher-level drivers must call this once the first camera's heavy GATT traffic (CCCD subscription) has settled so that additional configured cameras can be discovered without contending with the in-progress pairing sequence. `open_gopro_ble` invokes it at the end of CCCD subscription.

**Must be called from the NimBLE host task** — typically from inside a NimBLE callback. Not posted through the event queue; the caller is responsible for being on the right task.

---

## Scan State Machine

Two scan modes run exclusively:

```
             ┌──────────────────────────────────┐
             │        idle (no scan)             │
             │  (no disconnected cameras, or     │
             │   connecting/discovering)         │
             └──┬──────────────────────────────┬─┘
                │ has_disconnected_cameras()    │ ble_core_start_discovery()
                ▼                              ▼
  ┌─────────────────────────┐    ┌──────────────────────────┐
  │    background scan      │    │     discovery scan        │
  │  passive, no timeout    │◄───│  passive, timeout_ms      │
  │  SW filter: is_known_   │    │  all ads → on_disc        │
  │  addr() gates connect   │    └────────────┬─────────────┘
  └──────────┬──────────────┘                 │ stop / timeout
             │ known addr seen                 ▼
             ▼                     start_scan_if_needed()
  ┌─────────────────────────┐
  │      connecting         │
  │  scan cancelled         │
  │  ble_gap_connect()      │
  └──────────┬──────────────┘
             │ success / failure
             ▼
    on_connected / restart background scan
```

Background scan uses software filtering (`is_known_addr`) rather than the hardware whitelist. This is intentional: the hardware whitelist would prevent the scan from ever seeing new cameras during provisioning.

---

## Connection Lifecycle

```
ble_gap_connect()
     │
     ▼ BLE_GAP_EVENT_CONNECT (status == 0)
on_connected(conn_handle, addr)
     │
     ▼ ble_gap_security_initiate()
     │   ├─ bonded peer  →  resume encryption with stored LTK
     │   └─ new peer     →  SMP Just Works pairing
     │
     ▼ BLE_GAP_EVENT_ENC_CHANGE (status == 0)
on_encrypted(conn_handle, addr)
     │
     │ (normal operation — GATT writes, notifications)
     │
     ▼ BLE_GAP_EVENT_DISCONNECT
on_disconnected(conn_handle, addr, reason)
     │
     ▼ start_scan_if_needed()   (background scan resumes)
```

---

## Bond Management

| Scenario | Behaviour |
|----------|-----------|
| New camera | SMP Just Works pairing; bond stored in NVS |
| Reconnect | LTK retrieved from NVS; encryption resumed without re-pairing |
| Key mismatch (`BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL)` or `BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING)`) | Bond purged via `ble_gap_unpair()`; peer must re-pair |
| Any other encryption-change failure (link timeouts, transient SMP errors, controller hiccups) | Bond preserved; disconnect triggers reconnect scan |
| Camera has bond, ESP32 NVS erased (`BLE_GAP_EVENT_REPEAT_PAIRING`) | ESP32 purges camera's stale entry; retries fresh pairing |
| Bond not in `camera_manager` keep-list | Purged by `ble_core_purge_unknown_bonds()` at startup |

**SMP configuration** (set in `ble_init.c`):

| Parameter | Value |
|-----------|-------|
| IO capability | `BLE_HS_IO_NO_INPUT_OUTPUT` (Just Works) |
| Bonding | Enabled |
| Key distribution (initiator → responder) | ENC + ID |
| Key distribution (responder → initiator) | ENC + ID |

---

## Error Handling

| Scenario | Behaviour |
|----------|-----------|
| Memory allocation failure (`malloc`) | Returns `ESP_ERR_NO_MEM`; no event posted; logged as error |
| NimBLE stack reset (`on_reset`) | Logged as error with reason code |
| Connection attempt failed | Logged as warning; background scan resumed |
| Encryption failure (key mismatch — `BLE_ERR_AUTH_FAIL` / `BLE_ERR_PINKEY_MISSING`) | Bond purged; logged |
| Encryption failure (anything else) | Logged as warning; bond preserved; disconnect handles reconnect |
| GATT write error | Logged as error; no retry (caller's responsibility) |
| Bond enumeration error | Logged as error; `purge_unknown_bonds` returns without action |

---

## Configuration

All BLE configuration lives in [`sdkconfig.defaults`](../../sdkconfig.defaults). There is no `Kconfig` in this component.

| `sdkconfig.defaults` key | Value | Effect |
|--------------------------|-------|--------|
| `CONFIG_BT_NIMBLE_ENABLED` | `y` | Enable NimBLE host |
| `CONFIG_BT_NIMBLE_PINNED_TO_CORE` | `1` | NimBLE host task on core 1 |
| `CONFIG_BT_CONTROLLER_ENABLED` | `y` | Enable BT controller |
| `CONFIG_BT_CTRL_PINNED_TO_CORE` | `1` | BT controller on core 1 |
| `CONFIG_NIMBLE_MAX_CONNECTIONS` | `5` | 4 cameras + headroom |
| `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` | `517` | GoPro responses up to ~88 bytes |
| `CONFIG_BT_NIMBLE_NVS_PERSIST` | `y` | Bonds survive reboot |
| `CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE` | `y` | Software WiFi/BLE coexistence |

Runtime constants (in source):

| Constant | Value | Location |
|----------|-------|----------|
| Device name | `"ESP32 Controller"` | `ble_init.c` |
| Notification buffer | 512 bytes | `ble_connect.c`, `ble_gatt_write.c` |
