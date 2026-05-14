# ESP32 GoPro Controller — V2 Full Redesign

**Status:** Implemented
**Date:** 2026-04-26
**Last revision:** 2026-05-03 — COHN abandoned; BLE is the sole control transport for Hero 9+. See "COHN removal" notes throughout §15 and §18.

---

## 1. Goals

- **Manufacturer-agnostic camera manager.** The core manager layer knows only about slots, model IDs, connection states, and a driver vtable. No GoPro-specific concepts leak into it.
- **BLE-first control for Hero 9+.** Recording commands, datetime, recording-status polling, and keepalive all travel over BLE. No WiFi association is required for these cameras.
- **WiFi RC-emulation for Hero 4** is preserved unchanged: the camera joins the SoftAP in WiFi RC mode and is driven via UDP keepalive + HTTP/1.0.
- **Unified slot structure.** RC-emulation and BLE-control cameras share a single slot structure with no per-type divergence in the manager layer. All behavioral branching is gated on `camera_model_t` via named capability helpers (`gopro_model_uses_rc_emulation`, `gopro_model_uses_ble_control`).
- **Persisted slot identity.** Camera MAC, model, and last-known IP (RC only) live in NVS so cameras are immediately usable on every subsequent boot without re-pairing.
- **Clean component separation.** WiFi plumbing, HTTP/web UI, GoPro BLE control, and GoPro RC-emulation are independent components with explicit dependencies.
- Web UI served from a LittleFS data partition with pre-compressed assets, independently flashable from firmware.

---

## 2. Component Structure

```
components/
  camera_manager/         Manufacturer-agnostic: slots, NVS generic record,
                          coarse status enums, driver vtable dispatch
  can_manager/            Unchanged
  ble_core/               Generic BLE: scan, connect, bond, event callbacks
  wifi_manager/           SoftAP init, WiFi/IP event handling, DHCP station table
  http_server/            HTTP server, all /api/ handlers, serves web UI from LittleFS
  gopro/
    gopro_model.h         GoPro-specific camera_model_t values + capability helpers
    open_gopro_ble/       BLE control driver for Hero 9+; implements camera_driver_t
    gopro_wifi_rc/        WiFi Remote emulation + UDP driver for Hero 4;
                          implements camera_driver_t
```

### Dependency direction

```
http_server  →  camera_manager, ble_core, wifi_manager, gopro/*
gopro/*      →  camera_manager, ble_core, wifi_manager
camera_manager → ble_core (handle types only)
wifi_manager →  (no camera dependencies)
ble_core     →  (no camera dependencies)
can_manager  →  camera_manager
```

`wifi_manager` and `ble_core` are pure infrastructure — they know nothing about cameras or GoPro. `http_server` sits at the top of the dependency graph and is the only component that reaches across all others to service API requests.

### Responsibilities

**`camera_manager`** — Slot lifecycle, generic NVS record (`cam_N/camera`), tick timer, recording dispatch, state-change callbacks. Knows `camera_model_t` values but calls no GoPro capability helpers directly.

**`ble_core`** — Generic BLE scanning, connecting, bonding. Fires callbacks for connect/disconnect/data events. No GoPro awareness.

**`wifi_manager`** — SoftAP initialisation, WiFi and IP event handling, DHCP station table. Fires callbacks for station associate/connect/disconnect. No camera awareness.

**`http_server`** — HTTP server startup, all `/api/` endpoint handlers, serves `index.html` / `app.js.gz` / `style.css.gz` from LittleFS. Depends on all other components but is not depended upon by any.

**`open_gopro_ble`** — Implements `camera_driver_t` for Hero 9+ cameras. Owns BLE discovery, pairing, GATT setup, the V1-style readiness sequence (`GetHardwareInfo` → `SetCameraControlStatus(EXTERNAL)` → datetime + status poll), shutter commands (TLV `SetShutter`), the 5 s `GetStatusValue` poll, the 3 s BLE keepalive, and bond management. The BLE connection is the only control transport for these cameras.

**`gopro_wifi_rc`** — Implements `camera_driver_t` for Hero 3 / 4 / 5 / 7 cameras. The user puts the camera into WiFi RC mode and it joins our SoftAP. Keepalive, status poll, shutter, and identify all run over UDP on the shared 8383⇄8484 socket; shutter dispatch is broadcast to `255.255.255.255:8484` for "fire all" and unicast to a single `last_ip` for per-slot or mismatch corrections (§13.3a, §17.7). Identify is via the UDP `cv` opcode (§17.2.5). HTTP/1.0 is reserved for the optional date/time set on Hero4-class cameras (§17.2.6). These cameras have no BLE radio.

---

## 3. sdkconfig.defaults

The values below should be committed in `sdkconfig.defaults` so every developer's build is consistent. Anything not listed here uses the ESP-IDF default for the target chip. The full file is reproduced at the end of this section as a single copy-pasteable block.

### 3.1 Target and partition

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="8MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
```

The custom partition table reserves a 3 MB `storage` partition for the LittleFS web UI assets (§19.1).

### 3.2 BLE / NimBLE

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y

# Pin NimBLE host and BT controller to core 1 (§4.1)
CONFIG_BT_NIMBLE_PINNED_TO_CORE_1=y
CONFIG_BT_CTRL_PINNED_TO_CORE_1=y

# Connection capacity: 4 cameras + headroom
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=5

# Bonding storage in NVS
CONFIG_BT_NIMBLE_NVS_PERSIST=y

# ATT MTU — advertised when the camera initiates an MTU exchange (we never
# initiate one ourselves). A larger MTU shrinks ATT-level fragmentation, but
# note that GoPro also fragments long responses at the GPBS application
# layer (e.g. GetHardwareInfoRsp ~88 bytes), so reassembly is required
# regardless of the negotiated ATT MTU.
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517
```

### 3.3 WiFi

```
# Pin WiFi task to core 0, opposite NimBLE (§4.1)
CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y

# DHCP server
CONFIG_LWIP_DHCPS=y
CONFIG_LWIP_DHCPS_MAX_STATION_NUM=8

```

### 3.4 HTTP / HTTPS

```
# Web UI server
# HTTPD_TASK_STACK_SIZE and HTTPD_MAX_OPEN_SOCKETS removed in IDF v6 — set via httpd_config_t in http_server_init()
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
```

(No HTTPS-client TLS flags are needed: `gopro_wifi_rc` uses plain HTTP/1.0 to Hero 4 cameras on the SoftAP, and Hero 9+ control runs entirely over BLE.)

### 3.5 esp_timer

```
# Pin esp_timer task to core 0 alongside WiFi (§4.1)
CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0=y
CONFIG_ESP_TIMER_TASK_STACK_SIZE=4096
```

### 3.6 Logging

```
CONFIG_LOG_DEFAULT_LEVEL_INFO=y       # production-quiet at boot
CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y    # ceiling for runtime override (§22.6)
CONFIG_LOG_TIMESTAMP_SOURCE_RTOS=y
CONFIG_LOG_COLORS=y                   # ANSI colors over USB serial
```

### 3.7 Coexistence

```
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
```

Enabled by default when both BLE and WiFi are compiled in. Documented here so it does not get accidentally disabled.

### 3.8 Verification at boot

A short utility in `app_main()` (or the first component init) should log the actual core affinity and channel choice on every boot, so a misconfigured sdkconfig is visible immediately rather than as mysterious latency:

```c
ESP_LOGI(TAG, "boot: NimBLE core=%d, WiFi core=%d, channel=%d",
         CONFIG_BT_NIMBLE_PINNED_TO_CORE,
         CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0 ? 0 : 1,
         AP_CHANNEL);
```

`CONFIG_ESP_WIFI_TASK_CORE_ID` is a Kconfig `choice` name and does not generate a C integer macro. Use the boolean option directly. `CONFIG_BT_NIMBLE_PINNED_TO_CORE` is an explicit `int` config and resolves to 0 or 1 directly. `AP_CHANNEL` is `#define`d in `main.c` (temporarily) and will move to `wifi_manager.h`.

If those numbers ever surprise you on a new build, the `sdkconfig` overrode the defaults — investigate before chasing radio symptoms.

---

## 4. BLE / WiFi Coexistence and Core Pinning

The ESP32-S3 has two cores and a single 2.4 GHz radio shared between BLE and WiFi via TDMA arbitration in the BT controller. We are using NimBLE. The principle: pin WiFi to the **opposite** core from NimBLE so the two stacks do not contend for CPU cycles. They will still contend for the radio via TDMA, but that contention is unavoidable and is managed by the BT controller.

### 4.1 Task pinning — pin WiFi opposite NimBLE

NimBLE is pinned to core 1 in `sdkconfig` (§3.2). Everything else follows from that:

| Task | Configuration | Core | Rationale |
|---|---|---|---|
| NimBLE host | `CONFIG_BT_NIMBLE_PINNED_TO_CORE_1=y` | 1 | The reference point — all other pinning is relative to this |
| BT controller | `CONFIG_BT_CTRL_PINNED_TO_CORE_1=y` | 1 | Same core as NimBLE host; the controller posts events to it |
| WiFi task | `CONFIG_ESP_WIFI_TASK_CORE_ID_0=y` | 0 | Opposite NimBLE — no CPU contention with BLE |
| `esp_timer` task | `CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0=y` | 0 | Most timers fire WiFi/HTTP work; keep them adjacent |
| TWAI ISR | (default) | 0 | CAN ISR is light; default is fine |
| `can_manager` RX task | `xTaskCreatePinnedToCore` core 1 | 1 | Pure CPU parser; pin away from WiFi (sits next to BLE — CAN load is light) |
| `gopro_wifi_rc` work + shutter tasks | core 0 | 0 | Talk to UDP + HTTP via WiFi; keep them adjacent |

Net effect: BLE work runs on core 1, WiFi work runs on core 0. They contend for the radio at the controller layer but not for CPU.

If at some point we move NimBLE to core 0, every WiFi-related row above flips to core 1. The pinning rule is "WiFi opposite NimBLE", not literally "WiFi on core 0".

### 4.2 SoftAP channel choice

BLE advertising channels are 37 (2402 MHz), 38 (2426 MHz), and 39 (2480 MHz). The SoftAP channel should overlap none of them.

| Channel | Center freq | Overlap with BLE adv |
|---|---|---|
| 1 | 2412 MHz | Adv 37 (2402) — close |
| 6 | 2437 MHz | Adv 38 (2426) — close |
| 11 | 2462 MHz | None — clear of all three primary advs |

**Use channel 11.** This is the §11.2 default. Channel 11 also avoids the worst of BT classic SCO collisions if a phone is ever paired to the controller (unlikely, but cheap insurance).

### 4.3 Concurrency hot-paths

The biggest BLE+WiFi co-traffic windows are the BLE GATT discovery / readiness sequence happening while the SoftAP is busy with an RC camera DHCP exchange or the web UI is making an HTTP request. Symptoms of coexistence pressure look like BLE notification timeouts mid-discovery or DHCP requests that never receive a response. If observed in testing:

1. Verify task pinning is actually applied (`uxTaskGetSystemState()` reports the core for each task).
2. Check `CONFIG_BT_CTRL_COEX_PHY_CODED_TX_RX_TIME_LIMIT` and related coex tuning — but don't reach for these speculatively.
3. Consider serialising BLE pairing: only one camera is allowed to be doing GATT discovery / readiness at a time. The current `ble_core` background-scan model already serialises connection attempts; this would extend serialisation across the readiness sequence.

The radio is shared; 100% reliability under simultaneous BLE+WiFi heavy traffic is not achievable. Design for graceful degradation — every state machine that depends on BLE or WiFi should tolerate single-message drops and recover on the next event.

---

## 5. Model Identification

Camera model is the single source of truth for all behavioral branching. `camera_model_t` is defined in `camera_manager` and is manufacturer-agnostic in the sense that it holds values from all manufacturers. GoPro-specific capability helpers live in `gopro/gopro_model.h` and are only called by GoPro components — never by `camera_manager` directly.

### 5.1 `camera_model_t`

```c
typedef enum {
    CAMERA_MODEL_UNKNOWN           = 0,   /* Unset — invalid for NVS storage            */

    /* GoPro RC-emulation (IDs match GoPro's official `model_number` field as
       returned by gpControl info JSON; cross-referenced against
       goprowifihack/CameraCodenames.md.  Cameras self-identify via the UDP
       `cv` opcode at pair time — see §17.2.5.  Hero3-class cameras that
       don't answer cv stay at HERO_LEGACY_RC and require manual selection
       in the UI for any model-specific behaviour).  */
    CAMERA_MODEL_GOPRO_HERO2            = 1,
    CAMERA_MODEL_GOPRO_HERO3_WHITE      = 2,
    CAMERA_MODEL_GOPRO_HERO3_SILVER     = 3,
    CAMERA_MODEL_GOPRO_HERO3_BLACK      = 4,
    CAMERA_MODEL_GOPRO_HERO3PLUS_SILVER = 10,
    CAMERA_MODEL_GOPRO_HERO3PLUS_BLACK  = 11,
    CAMERA_MODEL_GOPRO_HERO4_SILVER     = 12,
    CAMERA_MODEL_GOPRO_HERO4_BLACK      = 13,
    CAMERA_MODEL_GOPRO_HEROPLUS_LCD     = 15,
    CAMERA_MODEL_GOPRO_HERO4_SESSION    = 16,
    CAMERA_MODEL_GOPRO_HEROPLUS         = 17,
    CAMERA_MODEL_GOPRO_HERO5_BLACK      = 19,
    CAMERA_MODEL_GOPRO_HERO5_SESSION    = 21,
    CAMERA_MODEL_GOPRO_HERO6_BLACK      = 24,
    CAMERA_MODEL_GOPRO_HERO_2018        = 34,
    CAMERA_MODEL_GOPRO_HERO_LEGACY_RC   = 999, /* sentinel — cv probe didn't resolve */

    /* GoPro BLE-control (IDs verified against OpenGoPro GetHardwareInfo responses)       */
    CAMERA_MODEL_GOPRO_HERO7_BLACK  = 30,
    CAMERA_MODEL_GOPRO_HERO9_BLACK  = 55,
    CAMERA_MODEL_GOPRO_HERO10_BLACK = 57,
    CAMERA_MODEL_GOPRO_HERO11_BLACK = 58,
    CAMERA_MODEL_GOPRO_HERO11_MINI  = 60,
    CAMERA_MODEL_GOPRO_HERO12_BLACK = 62,
    CAMERA_MODEL_GOPRO_MAX2         = 64,
    CAMERA_MODEL_GOPRO_HERO13_BLACK = 65,
    CAMERA_MODEL_GOPRO_LIT_HERO     = 70,

    /* Future manufacturer blocks: 1000+                                                */
} camera_model_t;
```

GoPro RC-emulation IDs match the official `model_number` field returned by the camera's gpControl info JSON. GoPro BLE-control IDs are verified against real `GetHardwareInfoRsp` payloads. Future manufacturers use the 1000+ block to avoid collisions.

### 5.2 GoPro capability helpers (`gopro/gopro_model.h`)

These helpers live in the GoPro component tree and are called only by GoPro components. `camera_manager` never calls them.

Each helper enumerates every known model explicitly rather than using numeric ranges. This is intentional: a new model ID must be consciously added to each applicable helper — there is no silent auto-inclusion based on a range. Adding a new manufacturer never requires touching these helpers, because 1000+ values will never match any GoPro check.

```c
/** True if the model is any known GoPro camera (RC-emulation or BLE-control).           */
static inline bool gopro_model_is_gopro(camera_model_t model) {
    return gopro_model_uses_rc_emulation(model) || gopro_model_uses_ble_control(model);
}

/** Camera connects by emulating a GoPro WiFi Remote AP (Hero 4).                         */
static inline bool gopro_model_uses_rc_emulation(camera_model_t model) {
    return model == CAMERA_MODEL_GOPRO_HERO4_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO4_SILVER;
}

/** Camera is controlled over BLE; never associates to the SoftAP (Hero 9+).             */
static inline bool gopro_model_uses_ble_control(camera_model_t model) {
    return model == CAMERA_MODEL_GOPRO_HERO9_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO10_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO11_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO11_MINI
        || model == CAMERA_MODEL_GOPRO_HERO12_BLACK
        || model == CAMERA_MODEL_GOPRO_MAX2
        || model == CAMERA_MODEL_GOPRO_HERO13_BLACK
        || model == CAMERA_MODEL_GOPRO_LIT_HERO;
}

/** Keepalive, status, shutter, and identify (`cv`) all use UDP for RC-emulation
 *  cameras; HTTP is reserved for date/time only.                                        */
static inline bool gopro_model_uses_udp_keepalive(camera_model_t model) {
    return gopro_model_uses_rc_emulation(model);
}

/** Model cannot be read from the camera — must be selected by the user at pairing.     */
static inline bool gopro_model_requires_manual_id(camera_model_t model) {
    return gopro_model_uses_rc_emulation(model);
}
```

`gopro_model_requires_manual_id` and `gopro_model_uses_rc_emulation` share the same implementation today but are intentionally separate — the questions they answer may diverge as more models are added.

Pre-Hero 9 BLE control (Hero 5, Hero 7) is reportedly functional but not officially supported by Open GoPro. `gopro_model_uses_ble_control()` enumerates only the officially-supported list; older models can be added once verified on hardware.

---

## 6. NVS Layout

Each camera occupies namespace `cam_N` (N = slot index). `camera_manager` is the only owner — there is currently no driver-owned key in this namespace.

### 6.1 Generic record — `cam_N/camera` (owned by `camera_manager`)

```c
typedef struct {
    uint8_t       version;       /* Schema version — currently 1                        */
    char          name[32];      /* Human-readable camera name                          */
    camera_model_t model;        /* Must not be CAMERA_MODEL_UNKNOWN at save time       */
    uint8_t       mac[6];        /* BLE peer MAC (BLE-control) / WiFi MAC (RC)          */
    uint32_t      last_ip;       /* RC only: last DHCP-assigned IP. Zero for BLE.       */
    bool          is_configured;
} camera_nv_record_t;
```

The `mac` field stores whichever radio identifies the camera on its primary transport: BLE peer MAC for Hero 9+ (BLE-control), WiFi MAC for Hero 4 (RC). The two families never overlap, so a single field is sufficient. `last_ip` is only meaningful for RC cameras (BLE-control cameras don't get an IP).

**Schema versioning policy:** A blob whose `version` does not match the current schema is discarded; the slot is left unconfigured and re-pairing is required. Firmware upgrades do not automatically invalidate NVS — only a version bump does. **Appending new fields at the end of the struct is not a version bump** — `load_slot_from_nvs()` zero-initializes the record before `nvs_get_blob()`, so smaller legacy blobs leave any new trailing fields zero.

**Save-time validation:** `camera_manager_save_slot()` returns `ESP_ERR_INVALID_ARG` and logs an error if `model == CAMERA_MODEL_UNKNOWN`.

---

## 7. In-RAM Structure

### 7.1 `camera_slot_t`

```c
typedef struct {
    /* --- Persisted fields (mirrored from cam_N/camera on load) --- */
    char           name[32];
    camera_model_t model;
    uint8_t        mac[6];        /* BLE peer MAC (BLE-control) or WiFi MAC (RC)        */
    uint32_t       last_ip;       /* RC only: last DHCP-assigned IP                     */
    bool           is_configured;

    /* --- BLE state (coarse — detail lives in open_gopro_ble) --- */
    cam_ble_status_t ble_status;
    uint16_t         ble_handle; /* BLE_HS_CONN_HANDLE_NONE when not connected          */

    /* --- WiFi / network state (RC-emulation only) --- */
    wifi_cam_status_t wifi_status;
    uint32_t          ip_addr;   /* Current DHCP IP; may differ from last_ip            */
    bool              wifi_associated; /* true between SoftAP join/disassociate events  */

    /* --- Control --- */
    desired_recording_t    desired_recording;
    const camera_driver_t *driver;
    void                  *driver_ctx;
    bool                   requires_ble; /* mirrors the driver registration flag        */
} camera_slot_t;
```

The `WIFI_CAM_*` enum names are retained for both transports (they predate the BLE-control path). For BLE-control cameras, `wifi_status == WIFI_CAM_READY` simply means "the driver's readiness sequence completed and the slot accepts recording commands"; `WIFI_CAM_NONE` means "not ready". The intermediate `ASSOCIATING` / `ASSOCIATED` / `CONNECTED` / `PROBING` states are only meaningful for RC.

### 7.2 `cam_ble_status_t`

Camera manager sees only coarse BLE states. Detailed GoPro-specific states (bonded, subscribed, provisioning) are internal to `open_gopro_ble` and mapped to this enum when reported upward.

```c
typedef enum {
    CAM_BLE_NONE = 0,       /* RC-emulation camera, or BLE-control camera not yet contacted */
    CAM_BLE_CONNECTING,     /* Any in-progress BLE work: scan, connect, bond                 */
    CAM_BLE_CONNECTED,      /* L2 up, manufacturer-specific setup in progress                */
    CAM_BLE_READY,          /* Readiness sequence complete; control transport for Hero 9+    */
} cam_ble_status_t;
```

### 7.3 `wifi_cam_status_t`

```c
typedef enum {
    WIFI_CAM_NONE = 0,      /* Not on network                                          */
    WIFI_CAM_ASSOCIATING,   /* L2 association in progress                              */
    WIFI_CAM_ASSOCIATED,    /* L2 up, waiting for DHCP lease                          */
    WIFI_CAM_CONNECTED,     /* IP assigned by ESP32 DHCP server                       */
    WIFI_CAM_PROBING,       /* Sending test HTTPS/UDP command to verify responsiveness */
    WIFI_CAM_READY,         /* Camera confirmed ready for recording commands           */
} wifi_cam_status_t;
```

**Operational readiness:** `wifi_status == WIFI_CAM_READY` is the sole gate for `start_recording()` / `stop_recording()`. Replaces the old `camera_ready` bool and `wifi_connected` bool.

### 7.4 `camera_recording_status_t`

Reported by every driver via `get_recording_status()`. Three states only — there is intentionally no `STARTING` or `STOPPING` state. The "command in flight" case is handled by the mismatch-correction grace period (§13.4); the type system does not need to reflect it.

```c
typedef enum {
    CAMERA_RECORDING_UNKNOWN = 0,   /* Driver has not yet observed the camera, or the
                                       last status request failed. Mismatch correction
                                       is suppressed in this state.                      */
    CAMERA_RECORDING_IDLE,          /* Camera is connected and not recording.            */
    CAMERA_RECORDING_ACTIVE,        /* Camera is actively recording.                     */
} camera_recording_status_t;
```

`UNKNOWN` is the initial value and is also the value returned after a status poll error or transport failure. The mismatch-correction loop (§13.4) treats `UNKNOWN` as a no-op — it never issues a corrective command against an unknown status, because doing so would risk re-issuing the same command repeatedly during a transient failure.

### 7.5 `desired_recording_t`

Per-slot recording intent. Stored in `camera_slot_t` and consulted by the mismatch-correction loop.

```c
typedef enum {
    DESIRED_RECORDING_UNKNOWN = 0,  /* No intent established yet. Set at boot and held
                                       until the first CAN logging frame arrives (when
                                       auto-control is enabled) or the UI explicitly sets
                                       a state (when auto-control is disabled).
                                       Mismatch correction is suppressed in this state — 
                                       the system does not issue start or stop commands
                                       until it knows what the operator intends.          */
    DESIRED_RECORDING_STOP    = 1,  /* Intent: camera should not be recording.           */
    DESIRED_RECORDING_START   = 2,  /* Intent: camera should be recording.               */
} desired_recording_t;
```

`DESIRED_RECORDING_UNKNOWN` is distinct from `DESIRED_RECORDING_STOP`. A `STOP` intent actively commands cameras to stop recording. `UNKNOWN` means "we have not yet heard from the CAN bus or the UI — take no action." This prevents the controller from issuing stop commands to cameras that were already recording when the ESP32 rebooted, before the first CAN frame re-establishes intent.

---

## 8. Driver Vtable

Two drivers are registered with `camera_manager`, each owning a disjoint subset of `camera_model_t` values:

| Driver registration `matches()` | Component | Transport |
|---|---|---|
| `gopro_model_uses_rc_emulation` | `gopro_wifi_rc` | UDP for keepalive / status / shutter (broadcast for fire-all, unicast for per-slot) / identify (`cv`); HTTP/1.0 only for date/time on the (Hero4-class today) cameras for which `gopro_model_supports_http_datetime()` returns true |
| `gopro_model_uses_ble_control` | `open_gopro_ble` | BLE TLV + protobuf (everything) |

`camera_manager_register_new()` allocates a slot keyed on MAC; the model is set in a separate call. For BLE-control cameras the model comes from `GetHardwareInfo` after BLE encryption. For RC-emulation cameras the model is established at pair time by the UDP `cv` opcode response (see §17.2.5); slots whose `cv` reply doesn't match a known string fall through to `CAMERA_MODEL_GOPRO_HERO_LEGACY_RC` and continue to operate via UDP.

```c
struct camera_driver {
    /* Per-slot — always non-NULL.  Used by the mismatch poll, by
     * camera_manager_set_desired_recording_slot(), and by
     * camera_manager_set_desired_recording_all() for drivers whose
     * broadcasts_to_all flag is false. */
    esp_err_t (*start_recording)(void *ctx);
    esp_err_t (*stop_recording)(void *ctx);
    camera_recording_status_t (*get_recording_status)(void *ctx); /* returns last known cached value */
    void (*teardown)(void *ctx);                                  /* nullable — see §20.5 */
    void (*update_slot_index)(void *ctx, int new_slot);           /* nullable — update cached slot */
    void (*on_wifi_disconnected)(void *ctx);                      /* nullable; RC-emulation only   */

    /* Broadcast — only consulted by camera_manager_set_desired_recording_all
     * when broadcasts_to_all is true.  start_recording_all / stop_recording_all
     * fire ONCE per dispatch wave; subsequent slots using the same driver have
     * their intent + grace deadline updated but skip enrollment.  Per-slot
     * calls always go through the per-slot entries above. */
    bool       broadcasts_to_all;
    esp_err_t (*start_recording_all)(void);
    esp_err_t (*stop_recording_all)(void);
};
```

`get_recording_status()` is a non-blocking cache read, safe to call from any context. How and when a driver refreshes that cache is an internal implementation detail — `camera_manager` does not need to know or care.

`teardown()` is called by `camera_manager_remove_slot()` and gives the driver a chance to clean up its own resources (stop timers, free memory). It may be `NULL` for drivers that have nothing to clean up beyond the slot record.

`on_wifi_disconnected()` is a hook for SoftAP-using drivers. BLE-control cameras leave it `NULL` because they never associate to the AP. The current RC-emulation driver also leaves it `NULL` and tracks station events through its own `gopro_wifi_rc_on_station_*` public API; the vtable hook is retained for future drivers that may want it.

`broadcasts_to_all` is a capability declaration: when true, the driver supplies `start_recording_all` / `stop_recording_all` that fire one transport-level command covering every camera the driver owns. `gopro_wifi_rc` sets it true (UDP `SH` to `255.255.255.255:8484`); `open_gopro_ble` leaves it false (BLE is point-to-point). See §13.3a for dispatch semantics.

---

## 9. Public Info Struct

`camera_slot_info_t` is returned by `camera_manager_get_slot_info()` to the web UI. The CAN manager reads camera state via `camera_manager_get_slot_can_state()` (see §14.2) and never touches this struct.

```c
typedef struct {
    /* Identity */
    int            index;
    char           name[32];
    camera_model_t model;
    uint8_t        mac[6];
    bool           is_configured;

    /* Granular connection state — for web UI */
    cam_ble_status_t  ble_status;
    wifi_cam_status_t wifi_status;

    /* Operational state */
    bool                  is_recording;
    desired_recording_t   desired_recording;
    uint32_t              ip_addr;          /* 0 when not connected */

    /* Pair-flow tracking (persisted in NVS) */
    bool                  first_pair_complete;

    /* RC-emulation only: true between SoftAP join and disassociation events.
     * Used by the web UI to render a slot whose wifi_status was demoted by
     * the keepalive silence watchdog as "connecting" (still on the AP) vs.
     * "disconnected" (left the AP). */
    bool                  wifi_associated;

    /* Future additions: battery_pct, storage_remaining_mb, temperature, etc. */
} camera_slot_info_t;
```

`is_recording` is derived from `wifi_status == WIFI_CAM_READY && driver->get_recording_status() == CAMERA_RECORDING_ACTIVE`. `desired_recording` is exposed so the web UI can show whether the operator intends the camera to be recording, which may differ from its actual state during a transition.

`first_pair_complete` is set to `true` after the camera completes its readiness sequence at least once (BLE drivers call `camera_manager_mark_first_pair_complete()` from `complete_connection_sequence()`; RC-emulation drivers call it immediately after `camera_manager_register_new()` since they have no multi-step handshake). Persisted in NVS. Used by the web UI to distinguish "Pairing" (initial add-camera flow, false) from "Connecting" (subsequent reconnects, true) for BLE cameras. Adding this field bumped `CAMERA_NV_SCHEMA_VERSION` from 1 to 2 — flash must be erased on first boot after upgrade.

### 9.1 Pair-attempt state machine

Single in-RAM state machine in `pair_attempt.c` tracks the user-initiated add-camera flow (BLE *or* RC-emulation) and surfaces errors that prevent a camera from being remembered. Reconnects do not use this machine.

```c
typedef enum {
    PAIR_TRANSPORT_BLE     = 0,
    PAIR_TRANSPORT_WIFI_RC = 1,
} pair_attempt_transport_t;

typedef enum {
    PAIR_ATTEMPT_IDLE,          /* No attempt has been started */
    PAIR_ATTEMPT_CONNECTING,    /* BLE L2 connect in flight, or RC probes sent */
    PAIR_ATTEMPT_BONDING,       /* BLE only: L2 up, waiting for encrypted bond */
    PAIR_ATTEMPT_PROVISIONING,  /* Slot registered, readiness sequence running */
    PAIR_ATTEMPT_SUCCESS,       /* Slot persisted; camera ready */
    PAIR_ATTEMPT_FAILED,        /* error_code + error_message valid */
} pair_attempt_state_t;
```

`pair_attempt_begin(addr, addr_type, transport)` reserves the machine. The transport is recorded on `pair_attempt_info_t` and surfaced to the web UI via `/api/pair/status`. Both transports share `state`, error codes, sticky terminal states, the 20 s overall watchdog, and the `pair_attempt_advance` / `pair_attempt_fail` / `pair_attempt_set_model` API.

Error codes (`pair_attempt_error_t`): `NONE`, `SLOTS_FULL`, `BLE_CONNECT_FAILED`, `BOND_FAILED`, `HWINFO_TIMEOUT`, `MODEL_UNSUPPORTED`, `HANDSHAKE_TIMEOUT`, `DISCONNECTED`, `CANCELLED`, `INTERNAL`. The BLE-specific codes never fire on the RC path; RC failures are `SLOTS_FULL` (no free slot at register time), `HANDSHAKE_TIMEOUT` (watchdog — no UDP response in 20 s), or `CANCELLED` (user/`/api/pair/cancel`).

**Sticky terminal states:** SUCCESS and FAILED remain set until the next `pair_attempt_begin()` clears them. Avoids polling races where the UI could miss the result.

**Forward-only transitions:** `pair_attempt_advance(state)` ignores any state at or behind the current state. `pair_attempt_fail(err, msg)` ignores calls when state is already terminal — the first specific cause wins (e.g. `HWINFO_TIMEOUT` raised before the inevitable `BLE_DISCONNECT` cleanup).

**Transport-aware cancel cleanup.** `pair_attempt_cancel()` always sets `FAILED + CANCELLED` (so racing `advance(SUCCESS)` becomes a no-op), then performs cleanup that depends on `info.transport`:

- **`PAIR_TRANSPORT_BLE`:** call `ble_gap_conn_cancel()` then, if a `conn_handle` was recorded, `ble_gap_terminate()`. If a slot was registered during `PROVISIONING` and never reached `first_pair_complete`, remove it (mirrors the frozen-model cleanup path).
- **`PAIR_TRANSPORT_WIFI_RC`:** look up the slot by MAC and remove it unconditionally. RC slots are committed (with `first_pair_complete = true`) at register time because there is no multi-step handshake to roll back to, so the BLE-style "remove only if PROVISIONING and not first_pair_complete" gate would never fire — without an unconditional removal, a never-responding device would leave a phantom camera in NVS.

**Wiring (BLE — `open_gopro_ble`):**
- `gopro_on_connected` → `advance(BONDING)` if addr matches the in-flight attempt.
- `gopro_on_encrypted` → `advance(PROVISIONING)` after `register_new` succeeds; `fail(SLOTS_FULL)` if not.
- `gopro_on_hw_info_ok` frozen-model branch → `fail(MODEL_UNSUPPORTED)` before terminating.
- `on_readiness_timer` exhaustion → `fail(HWINFO_TIMEOUT)` before terminating.
- `complete_connection_sequence` → `mark_first_pair_complete(slot)` then `advance(SUCCESS)`.
- `gopro_on_disconnected` → `fail(DISCONNECTED)` if addr matches and state is non-terminal (no-op if a more specific cause is already set).

**Wiring (RC — `gopro_wifi_rc`):**
- `/api/rc/add` calls `pair_attempt_begin(mac, 0, PAIR_TRANSPORT_WIFI_RC)` *before* `gopro_wifi_rc_add_camera()`. Returns `409 Conflict` if a previous attempt is still in flight.
- `gopro_wifi_rc_add_camera` registers the slot, primes keepalive + `st` + `cv`. If `register_new` fails, `fail(SLOTS_FULL)`.
- `rc_handle_promote` (first UDP datagram from the camera) → `set_model(mapped)` if `cv` was parsed inline, then `advance(SUCCESS)` if addr matches the in-flight attempt.
- `rc_handle_apply_cv` (cv response after promote) → `set_model(mapped) + advance(SUCCESS)` if addr matches. Both calls are no-ops once the state is terminal, so it's safe regardless of which command the work task processes first.
- 20 s watchdog elapses with no SUCCESS → `pair_attempt_cancel` (RC branch removes the slot) → message overwritten to `HANDSHAKE_TIMEOUT`. UDP keepalive doesn't have a separate per-slot timeout — cv retries on every keepalive tick (3 s) until the watchdog fires or the camera responds.

### 9.2 BLE-disconnect status reset

When a BLE-control camera disconnects, `camera_manager_on_ble_disconnected_by_handle()` resets both `ble_status` to `CAM_BLE_NONE` *and* `wifi_status` to `WIFI_CAM_NONE` (the latter only for slots with `requires_ble == true`). The `wifi_status` reset is necessary because BLE drivers overload the field as the universal "fully ready" flag — without the reset, `is_recording` derivation and the UI status mapping would see stale values after the connection drops. The mismatch poll timer is also stopped if it was armed.

---

## 10. Network Topology

The ESP32 runs a SoftAP whose role depends on the camera type:

- **BLE-control cameras (Hero 9+)** never associate to the SoftAP. The control transport is the BLE link only; the SoftAP exists only to support RC cameras and the web UI.
- **RC-emulation cameras (Hero 4 Black / Silver)** put themselves into WiFi RC mode (a setting on the camera itself, separate from the GoPro app). In that mode the camera scans for an AP whose SSID matches `HERO-RC-XXXXXX` and whose MAC OUI matches `d8:96:85`, joins it, and waits to be driven over UDP + HTTP/1.0.

Because the ESP32 is the DHCP server, RC camera IP addresses are known the moment a DHCP lease fires.

### Connection Workflow — BLE-Control Cameras (Hero 9+)

```
BLE scan → connect → bond → MTU exchange → subscribe to notifications
  → GetHardwareInfo poll until status=0  (parse model + firmware)
  → SetCameraControlStatus(EXTERNAL)     (3 s timeout; sequence proceeds either way)
  → camera_manager_on_camera_ready()      → WIFI_CAM_READY
  → SetDateTime (best-effort; deferred via datetime_pending_utc if not session-synced)
  → start 5 s GetStatusValue poll        (status ID 8 = system_record_mode_active)
  → BLE keepalive (0x42 every 3 s) maintains link supervision indefinitely
```

Disconnect handling is symmetric: the BLE link is the camera's heartbeat. A lost link cancels the readiness/status timers and clears the slot's connection state. `ble_core`'s background reconnect scan (gated on `has_disconnected_cameras()`) brings the camera back automatically.

### Connection Workflow — RC-Emulation Cameras (Hero 4)

```
Camera associates with SoftAP (recognises WiFi Remote SSID)
  → DHCP event fires → ip_addr known → update last_ip in NVS
  → gopro_wifi_rc: HTTP/1.0 probe confirms camera responds
  → camera_manager_on_camera_ready() → WIFI_CAM_READY
```

On reconnect, `last_ip` from NVS is used as the initial WoL target since RC-emulation cameras may not request a new DHCP lease. RC-emulation cameras cannot report their own model — `gopro_wifi_rc_add_camera()` defaults to `CAMERA_MODEL_GOPRO_HERO4_BLACK`; no model picker is present in the web UI.

### Command Transport Summary

| Command | BLE-control (Hero 9+) | RC-emulation (Hero 3 / 4 / 5 / 6 / 7) |
|---|---|---|
| Keepalive | BLE `0x42` to GP-0074 every 3 s | UDP `_GPHD_:0:0:2:0.000000\n` → port 8484 every 3 s |
| Shutter start/stop | BLE TLV `SetShutter` (0x01) → GP-0072 | UDP binary `SH` opcode (param 0x02 / 0x00). Broadcast `255.255.255.255:8484` × 3 for fire-all, unicast `last_ip:8484` × 1 for per-slot |
| Recording status | BLE TLV `GetStatusValue` (0x13) on GP-0076, 5 s poll | UDP binary `st` opcode → port 8484, 5 s poll; reply b13/b15 → recording state |
| Identify (model + fw) | BLE TLV `GetHardwareInfo` once at pair time | UDP binary `cv` opcode at pair + retry on every keepalive tick until the camera answers |
| Date/time | BLE TLV `SetDateTime` (0x0D) → GP-0072 | HTTP/1.0 `GET /gp/gpControl/command/setup/date_time?p=...` (Hero4-class only — gated by `gopro_model_supports_http_datetime`) |
| Control claim | BLE protobuf `SetCameraControlStatus(EXTERNAL)` once at readiness | (none — RC has no equivalent) |

### Recording Command Dispatch

The camera manager fires `start_recording()` / `stop_recording()` on all ready slots in a tight loop via the driver vtable — effectively concurrent. Each driver handles its own transport internally; BLE-control commands fan out asynchronously through `ble_core_gatt_write()`, RC commands queue onto the per-camera HTTP/UDP work task.

---

## 11. WiFi Manager

### 11.1 Responsibilities

`wifi_manager` is a pure infrastructure component. Its sole responsibilities are:

- Bring up the ESP32 SoftAP with the correct SSID, IP, and radio settings.
- Maintain a station table (MAC → IP mapping) for all currently-associated SoftAP clients.
- Forward WIFI and IP events to registered callbacks so higher-level components can react without knowing anything about the underlying Wi-Fi stack.

`wifi_manager` has **no camera awareness** and **no HTTP server**. It does not import `camera_manager`, `open_gopro_ble`, `gopro_wifi_rc`, `ble_core`, or `can_manager`.

### 11.2 SoftAP Configuration

| Parameter | Value | Notes |
|---|---|---|
| SSID | `HERO-RC-XXXXXX` | Last 3 bytes of chip MAC, e.g. `HERO-RC-A1B2C3` |
| IP address | `10.71.79.1` | Fixed; set before `esp_wifi_start()` |
| Gateway | `10.71.79.1` | Same as IP |
| Subnet mask | `255.255.255.0` | `/24` |
| DHCP pool | `10.71.79.2 – 10.71.79.50` | 49 addresses; cameras use option-50 preferred-IP on reconnect |
| Channel | 11 | 2462 MHz; clear of all three primary BLE advertising channels (37/38/39 = 2402/2426/2480 MHz). See §4 for coexistence rationale. |
| Auth mode | Open (`WIFI_AUTH_OPEN`) | No password; RC-emulation cameras require open AP |
| Max connections | 6 | 4 camera slots + 1 setup device + 1 spare |
| Power save | `WIFI_PS_NONE` | Disabled at init and re-applied on `WIFI_EVENT_AP_STOP` restart |
| Inactive time | 60 s | Default is 300 s; shortened so a silently-dropped camera is evicted and deauth'd cleanly in 15 s |

**Inactive time note:** The 60-second value was chosen because keepalive packets are sent every 2–3 seconds. Any camera that is actually alive will elicit a response that resets the inactivity timer. A camera that has gone silent is cleanly evicted in ≤60 s, triggering a fresh DHCP handshake on reconnect rather than getting stuck in a stale association.

**HT20 bandwidth + PMF disabled:** Bandwidth is forced to HT20 (`esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20)`) on `WIFI_EVENT_AP_START`. PMF (Protected Management Frames) is explicitly disabled because some iOS builds attempt an OWE handshake on certain IDF builds with WPA3 compiled in, which prevents association.

### 11.3 MAC Spoofing

The AP MAC OUI is overridden to `d8:96:85` so the ESP32 presents as a GoPro WiFi Remote device. Hero4 (and other RC-emulation) cameras auto-connect to an AP whose SSID is `HERO-RC-XXXXXX` **and** whose source MAC matches this OUI. The last three bytes are preserved from the factory MAC to keep the address unique per device.

```c
uint8_t gopro_mac[6] = { 0xd8, 0x96, 0x85, mac[3], mac[4], mac[5] };
esp_wifi_set_mac(WIFI_IF_AP, gopro_mac);
```

**Known ESP-IDF v5.x issue:** `0xD8` (binary `1101 1000`) does not have bit 1 set (the "locally administered" bit). Newer IDF builds validate this bit and return `ESP_ERR_WIFI_MAC`. If the override is rejected, the AP starts with the factory OUI and Hero4 RC-emulation cameras will not auto-connect. The warning is logged and `gopro_wifi_rc` must detect this case at startup. Workaround options: patch IDF validation, use a different MAC byte that satisfies the locally-administered check, or apply MAC spoofing via a different mechanism.

### 11.4 Station Table

```c
typedef struct {
    bool     active;
    uint8_t  mac[6];
    uint32_t ip_addr;   /* 0 until DHCP lease fires */
} sta_entry_t;

static sta_entry_t s_stations[AP_MAX_CONN];
```

The table is written from WiFi/IP event callbacks and read from HTTP handlers and camera components via the public API. Access is lock-free — minor races are acceptable because the table is only used for display and device discovery, not for safety-critical operations.

**Hero4 IP behaviour:** Hero4 cameras perform a standard DHCP request on their very first connection to a new SoftAP. The DHCP server assigns an IP from the pool and fires `IP_EVENT_ASSIGNED_IP_TO_CLIENT`; `gopro_wifi_rc` records this as `last_ip` and persists it via `camera_manager_save_slot()`. On subsequent reconnections, the Hero4 re-associates without requesting a new DHCP lease — it reuses the IP it learned on the first connection. Because no DHCP event fires on reconnect, `gopro_wifi_rc` relies on `last_ip` from NVS to address the camera. This is why `last_ip` is persisted on every confirmed DHCP assignment.

### 11.5 Event Handling

**`WIFI_EVENT_AP_START`**
- Apply HT20 bandwidth (`esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20)`)
- Set `AP_STARTED_BIT` in the event group to unblock `wifi_manager_wait_for_ap_ready()`

**`WIFI_EVENT_AP_STOP`**
- Log warning ("AP stopped unexpectedly")
- Clear `AP_STARTED_BIT`
- Call `esp_wifi_start()` to restart; re-apply `WIFI_PS_NONE`

**`WIFI_EVENT_AP_STACONNECTED`**
- Add station to table (`ip_addr` starts at 0)
- Fire `on_station_associated(mac)` callback

**`WIFI_EVENT_AP_STADISCONNECTED`**
- Remove station from table
- Fire `on_station_disconnected(mac)` callback

**`IP_EVENT_ASSIGNED_IP_TO_CLIENT`**
- Update station table entry with assigned IP
- Fire `on_station_ip_assigned(mac, ip)` callback

### 11.6 Callbacks

`wifi_manager` exposes station events via registered callbacks. It does not call into camera components directly.

```c
typedef void (*wifi_mgr_station_associated_cb_t)(const uint8_t mac[6]);
typedef void (*wifi_mgr_station_disconnected_cb_t)(const uint8_t mac[6]);
typedef void (*wifi_mgr_station_ip_assigned_cb_t)(const uint8_t mac[6], uint32_t ip);

void wifi_manager_set_callbacks(
    wifi_mgr_station_associated_cb_t  on_associated,
    wifi_mgr_station_disconnected_cb_t on_disconnected,
    wifi_mgr_station_ip_assigned_cb_t  on_ip_assigned
);
```

`gopro_wifi_rc` registers these callbacks at init. `http_server` does not need them (it queries the station table directly when serving `GET /api/rc/discovered`).

### 11.7 Public API

```c
/** Bring up the SoftAP, configure DHCP, apply radio settings, start the AP. */
void wifi_manager_init(void);

/** Block until WIFI_EVENT_AP_START fires (or AP_READY_TIMEOUT_MS = 5000 ms elapses).
 *  Called by the startup sequence before BLE init to ensure the beacon is on air
 *  before starting radio-intensive BLE work. */
void wifi_manager_wait_for_ap_ready(void);

/** Return the current IP of a station by MAC, or 0 if not found / no DHCP yet. */
uint32_t wifi_manager_get_station_ip(const uint8_t mac[6]);

/** Copy up to max_count currently-associated station entries into out[].
 *  Returns the number of entries written. */
int wifi_manager_get_connected_stations(wifi_mgr_sta_info_t *out, int max_count);

/** Register station event callbacks (called once at system init). */
void wifi_manager_set_callbacks(
    wifi_mgr_station_associated_cb_t,
    wifi_mgr_station_disconnected_cb_t,
    wifi_mgr_station_ip_assigned_cb_t
);
```

### 11.8 What `wifi_manager` Does NOT Own

| Item | Owned by |
|---|---|
| HTTP server startup and all `/api/` handlers | `http_server` |
| `index.html` / web assets | `web_ui/` (project root) |
| `GET /api/rc/discovered` handler | `http_server` (reads station table via `wifi_manager_get_connected_stations()`) |
| RC-emulation camera event routing | `gopro_wifi_rc` (via registered callbacks) |
| BLE-control cameras | Don't generate SoftAP events at all — they never associate to the AP. |

---

## 12. BLE Core

### 12.1 Responsibilities

`ble_core` owns the NimBLE host task, the GAP scan/connect lifecycle, and bond management. It is fully camera-agnostic — it knows nothing about GoPro or slot numbers. Higher layers register a callback struct and are notified of every relevant event.

`ble_core` does not import `camera_manager`, `open_gopro_ble`, `wifi_manager`, or any camera driver. The only direction of coupling is upward: callers register callbacks, and `ble_core` invokes them.

### 12.2 Threading Model

A single FreeRTOS task (`ble_host_task`) runs `nimble_port_run()` and processes the NimBLE event queue. All NimBLE API calls (`ble_gap_connect`, `ble_gap_disc`, `ble_store_*`, etc.) **must** run on this task. Calls originating from other tasks (HTTP handlers, camera_manager timers) are safe because `ble_core` routes them through the NimBLE event queue via `ble_npl_eventq_put()` before executing. All callbacks fire on the NimBLE host task — implementations must not block indefinitely.

### 12.3 Initialisation

```
1. ble_core_register_callbacks()   ← must be called first; callbacks copied by value
2. ble_core_init()
     - nimble_port_init()
     - ble_hs_cfg: sync_cb = on_sync, reset_cb = on_reset
     - Security manager: sm_io_cap = NO_IO, sm_bonding = 1
     - Key distribution: ENC + ID (both directions)
     - ble_svc_gap_init(), device name = "ESP32 Controller"
     - nimble_port_freertos_init(ble_host_task)
3. Stack fires on_sync when ready → boot reconnect chain starts automatically
```

**Pairing mode:** Just Works (no I/O capability). No PIN or confirmation is required. Bonding is enabled so long-term keys (LTK) are stored in NimBLE's NVS peer-security store and reused on every subsequent connection.

### 12.4 Scan Modes

Two scan modes are used, both passive (no scan requests sent to peers):

**Background scan** — runs indefinitely until all known cameras are connected.
- Parameters: `BLE_HS_FOREVER`, `BLE_HCI_SCAN_FILT_USE_WL` (hardware whitelist filters non-target advertisements at the controller)
- `filter_duplicates` disabled — a camera that comes back online after a gap must be detected even though its address was seen before the scan started
- `on_disc` callback is **not** invoked; advertisements only reach the scan event handler for reconnect processing
- Suppressed entirely via `start_scan_if_needed()` when `has_disconnected_cameras()` returns false

**Discovery scan** — user-initiated, runs for 120 seconds.
- Parameters: 120 000 ms timeout, `filter_policy = 0` (no whitelist — must see all cameras including unpaired ones), `filter_duplicates = 0`
- Every advertisement packet that parses successfully is forwarded to the `on_disc` callback; `open_gopro_ble` applies its own GoPro UUID filter
- Cancelled immediately by `ble_core_stop_discovery()`, which then restarts the background scan if needed
- `BLE_GAP_EVENT_DISC_COMPLETE` fires at end of 120 s and transitions back to background scan automatically

**Scan mutual exclusion:** Only one scan or connect can be active at a time. `ble_core_connect_by_addr()` cancels any running scan before calling `ble_gap_connect()`. `ble_core_start_discovery()` cancels the background scan before starting the discovery scan.

### 12.5 Boot Reconnect

On `on_sync`, `ble_core` calls `start_scan_if_needed()`. That is the entire boot reconnect sequence.

The passive background scan with `is_known_addr` already handles reconnection: when a known camera advertises, the scan handler sees it, calls `ble_gap_connect()`, and the normal connection event flow proceeds. No explicit iteration of bonded peers, no sequential chain, no `BLE_HS_FOREVER` stall.

### 12.6 Connection Events

All GAP events are handled in `connection_event_cb` (defined in `ble_connect.c`, shared across the module via `ble_core_internal.h`).

**`BLE_GAP_EVENT_CONNECT` (success)**
- Fires `on_connected(conn_handle, addr)` — higher layer stores handle, sets `CAM_BLE_CONNECTED`
- Immediately calls `ble_gap_security_initiate()` to resume encryption with stored LTK (or begin first-time pairing)
- Clears `s_connecting` flag

**`BLE_GAP_EVENT_CONNECT` (failure)**
- Clears `s_connecting` flag
- `start_scan_if_needed()` resumes the background scan

**`BLE_GAP_EVENT_ENC_CHANGE` (success)**
- Fires `on_encrypted(conn_handle, addr)` — this is the correct point for `open_gopro_ble` to begin GATT service discovery and subscribe to notifications. GATT commands sent before encryption is established will be rejected by the GoPro.

**`BLE_GAP_EVENT_ENC_CHANGE` (failure)**
- Distinguishes transient failures from genuine key mismatches:

| Error codes | Classification | Action |
|---|---|---|
| `BLE_HS_ETIMEOUT`, `BLE_HS_ETIMEOUT_HCI`, `BLE_HS_ENOTCONN`, `BLE_HS_ECONTROLLER` | Transient | Bond preserved; scan resumes via disconnect handler |
| All other status codes | Key mismatch | Bond deleted from NVS; next connection will perform fresh SMP pairing |

Deleting a bond on a transient failure would cause desynchronised bond state: the camera still has a bond but the ESP32 does not. The camera would then reject the fresh pairing request and the two sides could diverge permanently until the user re-pairs manually. The distinction above prevents this.

**`BLE_GAP_EVENT_REPEAT_PAIRING`**
- Symmetric case: the camera has a bond but the ESP32 does not (e.g. after an NVS erase)
- Deletes the camera's stale bond entry and returns `BLE_GAP_REPEAT_PAIRING_RETRY` so NimBLE retries fresh pairing on the same connection

**`BLE_GAP_EVENT_DISCONNECT`**
- Fires `on_disconnected(conn_handle, addr, reason)` before clearing state — higher layer must look up the slot by `conn_handle` while it is still valid
- Clears `s_connecting` flag
- Calls `start_scan_if_needed()` to resume the background scan

**`BLE_GAP_EVENT_NOTIFY_RX`**
- Copies notification payload into a 512-byte stack buffer (matches maximum negotiated ATT MTU; previous 64-byte limit silently dropped long responses such as `GetHardwareInfoRsp` at ~88 bytes)
- Fires `on_notify_rx(conn_handle, attr_handle, data, len)` — `open_gopro_ble` dispatches by `attr_handle` to the appropriate response handler

### 12.7 Bond Management

**`ble_core_purge_unknown_bonds(keep[], keep_count)`**
- Called **once at startup** from `open_gopro_ble_init()`, before `ble_core_init()` starts the NimBLE host task. Because no cameras can connect before the host task is running, the purge executes without any connection race. The background scan (`start_scan_if_needed()`) does not start until `on_sync` fires — which is after init returns — so the purge is guaranteed complete before the first advertisement is processed.
- Walks the peer-security store, collects addresses not in `keep[]`, deletes them.
- Must not be called after `ble_core_init()` starts the NimBLE host task; it is not posted to the event queue and is not safe from other contexts.

**`ble_core_remove_bond(addr)`**
- Terminates the active connection to `addr` (if any) via `BLE_ERR_REM_USER_CONN_TERM`
- Deletes `addr`'s entry from the peer-security store
- Posts to the NimBLE event queue — safe to call from HTTP handler tasks
- Caller must remove the camera from `camera_manager` *before* calling this, so that `is_known_addr` returns false and the disconnect handler does not trigger an automatic reconnect

### 12.8 GATT Write

```c
esp_err_t ble_core_gatt_write(uint16_t conn_handle, uint16_t attr_handle,
                               const uint8_t *data, uint16_t len);
```

ATT Write Request (write-with-response). Non-blocking on success. Used by `open_gopro_ble` to send OpenGoPro commands to the camera. The OpenGoPro spec lists the command/settings/query/net-mgmt characteristics as plain "Write" — Hero13 and later silently drop ATT Write Commands on those handles, so the request form is required. The ATT-level write response only confirms receipt; application-level GoPro responses still arrive asynchronously via GATT notifications.

### 12.9 Callback Table

Registered once via `ble_core_register_callbacks()` before `ble_core_init()`. Copied by value — the caller does not need to keep the struct alive.

| Field | Type | Implementor | Purpose |
|---|---|---|---|
| `on_disc` | `ble_core_on_disc_cb_t` | `open_gopro_ble` | Advertisement seen during discovery scan |
| `on_connected` | `ble_core_on_connected_cb_t` | `open_gopro_ble` / `camera_manager` | L2 link up, before encryption |
| `on_encrypted` | `ble_core_on_encrypted_cb_t` | `open_gopro_ble` | Encryption established — begin GATT discovery |
| `on_disconnected` | `ble_core_on_disconnected_cb_t` | `open_gopro_ble` / `camera_manager` | Link dropped |
| `on_notify_rx` | `ble_core_on_notify_rx_cb_t` | `open_gopro_ble` | ATT notification/indication received |
| `is_known_addr` | `ble_core_is_known_addr_cb_t` | `camera_manager` | Returns true if addr is a paired camera; gates auto-reconnect in scan handler |
| `has_disconnected_cameras` | `ble_core_has_disconnected_cameras_cb_t` | `camera_manager` | Returns true if any paired camera is not connected; gates background scan start |

All callbacks are optional (NULL-safe). All fire on the NimBLE host task.

### 12.10 Source File Layout

| File | Contents |
|---|---|
| `ble_init.c` | `ble_core_init()`, `on_sync` boot reconnect, `ble_core_register_callbacks()` |
| `ble_scan.c` | Background and discovery scan management, `ble_core_connect_by_addr()`, `scan_event_cb` |
| `ble_connect.c` | `connection_event_cb`, bond purge/remove, boot reconnect chain helpers |
| `ble_gatt_write.c` | `ble_core_gatt_write()` |
| `ble_core_internal.h` | Shared state and internal function declarations across the four source files |

---

## 13. Recording State Management

### 13.1 Per-slot desired state

Each camera slot holds a `desired_recording` value of type `desired_recording_t` (§7.5). This is the authoritative intent for that camera — independent of what the camera is actually doing at any given moment.

`desired_recording` is never persisted to NVS. It always resets to `DESIRED_RECORDING_UNKNOWN` on boot. The mismatch-correction loop takes no action while the value is `UNKNOWN` — no start or stop commands are issued until the CAN bus or the web UI establishes an explicit intent. See §13.2 for how intent is established after boot.

### 13.2 Automatic control (CAN-driven)

When automatic control is enabled (`camera_manager_get_auto_control() == true`), **every** received `0x600` frame calls:

```c
camera_manager_set_desired_recording_all(desired_recording_t intent);
/* intent is DESIRED_RECORDING_START or DESIRED_RECORDING_STOP based on the frame payload */
```

This call is intentionally idempotent — set on every frame, not only on transitions. The function compares the new intent against each slot's previously stored value: only slots that actually transition are dispatched (see §13.3a). A re-stated `DESIRED_RECORDING_START` while every camera already has `desired_recording == DESIRED_RECORDING_START` produces no output, no log line, and no BLE/HTTP traffic. The reason for setting on every frame instead of only on transitions is robustness against missed transitions:

- If the ESP32 reboots mid-race, the first `0x600` frame after boot re-establishes the correct intent without depending on transition detection.
- If a single `0x600` frame is dropped on the bus, the next frame reasserts the intent — no harm done.
- If a camera was offline when the transition occurred and connects later, its status poll will see the desired/actual divergence and issue the correct command on its own. The CAN side does not need to remember to re-send.

Default state at boot is `DESIRED_RECORDING_UNKNOWN` (§7.5 / §13.1). `camera_manager_set_desired_recording_all()` is **not called** while the CAN logging state is `LOGGING_STATE_UNKNOWN` (§14.2) — the `UNKNOWN` CAN state means the bus is silent or the RaceCapture has not yet sent a frame, and the system has no basis for issuing a command. The `desired_recording` for all slots stays at `UNKNOWN` until a real `0x600` frame arrives.

When automatic control is disabled, CAN frames do not modify `desired_recording`. The web UI controls each camera individually via:

```c
camera_manager_set_desired_recording_slot(int slot, desired_recording_t intent);
```

The UI must supply `DESIRED_RECORDING_START` or `DESIRED_RECORDING_STOP`; it may not set `UNKNOWN`. The new intent is dispatched immediately if the slot is `WIFI_CAM_READY` (§13.3a); otherwise the mismatch loop picks it up once the slot becomes ready (§13.4).

### 13.2.1 Reboot-mid-race recovery

The combination of "set on every CAN frame", the immediate-dispatch path (§13.3a), and the per-camera status poll provides automatic recovery from an ESP32 reboot during an active recording session. After reboot:

1. `desired_recording` resets to `DESIRED_RECORDING_UNKNOWN` on every slot (§13.1). No shutter commands are sent until intent is established.
2. Cameras and CAN come back online over the next several seconds.
3. The first `0x600` frame received sets `desired_recording = DESIRED_RECORDING_START` on all slots if RaceCapture is still in the `LOGGING` state.
4. For each slot that is already `WIFI_CAM_READY` at that moment, the immediate-dispatch path (§13.3a) issues `start_recording()` synchronously inside the `set_desired_recording_all()` call. For slots not yet ready, the mismatch poll (§13.4) catches them once `WIFI_CAM_READY` is reached and `CAMERA_RECORDING_IDLE` is observed.

If the RaceCapture logging state is `NOT_LOGGING` when the ESP32 reboots, intent is set to `DESIRED_RECORDING_STOP`. Ready slots receive an immediate `stop_recording()` call (a no-op if the camera is already idle, but cheap); unready slots fall to the mismatch poll, which does nothing because the cameras are already idle.

If no `0x600` frame arrives (CAN fault, RaceCapture powered off), `desired_recording` stays `UNKNOWN` indefinitely and no shutter commands are ever issued. The operator must either restore the CAN link or switch to manual control via the web UI.

Total recovery time for ready slots is bounded by the CAN logging-state heartbeat (continuous at 1+ Hz from RaceCapture) plus one BLE/HTTP write — typically under 1 s. For slots that come ready after the first `0x600` frame, recovery is bounded by one additional mismatch-poll interval (default 2 s).

### 13.3 Dispatch overview

`camera_manager` keeps `desired_recording` aligned with the camera's actual state through **two complementary paths**:

| Path | Trigger | Latency | Purpose |
|------|---------|---------|---------|
| Immediate dispatch (§13.3a) | `set_desired_recording_*()` call where the new intent differs from the previous value | 0 ms (synchronous in the API call) | Fast shutter response on CAN/UI input |
| Mismatch correction (§13.3b / §13.4) | Per-slot `esp_timer` firing every 2 s | up to one poll interval | Catches slots that weren't ready at API call time, recovers from missed/dropped commands, corrects state divergence |

Both paths arm the same per-slot `grace_until_us` deadline after issuing a command, so they cannot fire duplicate corrections at each other inside the grace window. They route through different vtable entries depending on whether the driver advertises broadcast capability — see §13.3a.

### 13.3a Immediate dispatch

`camera_manager_set_desired_recording_all()` and `camera_manager_set_desired_recording_slot()` execute the following sequence under the slot mutex:

```
for each affected slot (in order):
    if new intent == previous desired_recording → skip (idempotent)
    update desired_recording = new intent
    if new intent == DESIRED_RECORDING_UNKNOWN  → skip (no command to issue)
    if no driver assigned                       → skip (mismatch poll will catch when assigned)
    if wifi_status != WIFI_CAM_READY            → skip (mismatch poll will catch when ready)
    if driver->broadcasts_to_all && already enrolled in this wave:
                                                → arm grace_until_us; skip enrollment (broadcast already covers this slot)
    enroll (driver, ctx_or_NULL, slot, broadcast_flag); arm grace_until_us
```

After releasing the mutex, the function dispatches each enrolled entry **in slot order**:
- broadcast entry → `driver->start_recording_all()` / `driver->stop_recording_all()` (no per-slot ctx)
- per-slot entry → `driver->start_recording(ctx)` / `driver->stop_recording(ctx)`

Driver calls happen **outside the mutex** to avoid holding the lock across a potentially slow BLE write or UDP burst — same discipline as `poll_timer_cb` (§13.4).

`set_desired_recording_slot()` always uses the per-slot vtable entries regardless of `broadcasts_to_all`, so single-camera commands stay targeted at one IP / one BLE link and don't disturb peers.

#### Broadcast drivers

A driver that sets `camera_driver_t::broadcasts_to_all = true` and supplies `start_recording_all` / `stop_recording_all` is declaring "one call covers every camera I own." `set_desired_recording_all()` honours this by enrolling the driver only on the first matching slot it encounters; later slots using the same driver have their `desired_recording` and grace deadline updated but skip dispatch enrollment. Slot ordering is preserved, so a [BLE, RC, BLE, RC] fleet fires as: BLE slot 0 GATT write → RC broadcast (covers slots 1 and 3) → BLE slot 2 GATT write.

The broadcast and per-slot entries are independent functions on the vtable — drivers may implement either, both, or neither (subject to the per-slot pair always being non-NULL). The `gopro_wifi_rc` driver implements both: broadcast for "fire all" (UDP `SH` to `255.255.255.255:8484` × 3 for reliability) and unicast per-slot for the mismatch poll and `set_desired_recording_slot` (UDP `SH` to the slot's `last_ip` × 1). The unicast per-slot path exists deliberately to avoid sending a Start to a Hero4 that's already recording, which has been observed to flip it back off. The `open_gopro_ble` driver leaves `broadcasts_to_all = false`; BLE is point-to-point by nature.

Why a real-transition check instead of always dispatching:

- `can_manager` calls `set_desired_recording_all()` on every received `0x600` frame (§13.2). Without the check, this would send a SetShutter to every camera ~1+ Hz indefinitely.
- The check is "intent transition", not "intent vs. cached status". The cached status may legitimately disagree with `desired_recording` for a short window (e.g. the user just paired a camera that's actually recording while desired is STOP — the mismatch poll will correct it on its next tick).

Why no `cached_status` consultation here:

- The driver's own `start_recording()` already updates `cached_status` optimistically after dispatch. The next mismatch poll therefore sees no work to do.
- Even if `cached_status` is `UNKNOWN` (e.g. brand-new BLE connection that hasn't yet received its first status notification), we still want the user-initiated command to go out — the command is exactly the thing that will resolve the unknown.

### 13.3b Status polling

Each camera slot, once `WIFI_CAM_READY`, runs a periodic mismatch check driven by a per-slot `esp_timer` in `camera_manager`. The interval is per-model and defined in `gopro_model.h` (default 2 seconds; may be tuned as testing reveals differences).

On each timer fire, `camera_manager` calls `driver->get_recording_status(ctx)`, which returns the driver's **last known cached value** immediately — no network request is made. A few seconds of lag between the camera's actual state and the cached value is acceptable in this application.

Each driver is responsible for keeping its own cache fresh through whatever internal mechanism suits its transport:

| Camera category | Cache update mechanism |
|---|---|
| BLE-control (Hero 9+) | `open_gopro_ble` issues `GetStatusValue` (TLV 0x13, status ID 8 = `system_record_mode_active`) on the Query channel every 5 s; response handler updates `cached_status` |
| RC-emulation (Hero 3 / 4 / 5 / 6 / 7) | `gopro_wifi_rc` issues UDP binary `st` opcode (port 8484) every 5 s; reply byte 13 (power) and byte 15 (recording state) → `recording_status` (see §17.2.4) |

The per-slot timer is stopped when the slot leaves `WIFI_CAM_READY` and restarted when it re-enters it.

### 13.4 Mismatch correction

§13.3a's immediate-dispatch path covers the happy case where the user/CAN changes intent and the camera is ready. The mismatch-correction loop is the **safety net** for everything else: slots that weren't ready when intent changed, dropped commands, cameras whose actual state diverges from what we commanded (e.g. user pressed the physical shutter button on the camera body), and intent established before a slot reaches `WIFI_CAM_READY`.

Immediately after each status update, `camera_manager` compares the driver's cached recording status against `desired_recording`:

```
if desired_recording == UNKNOWN                               → no-op (no intent established)
if status == UNKNOWN                                          → no-op (camera state unknown)
if now < grace_until_us                                       → no-op (recent command still settling)
if desired_recording == START  && status == IDLE              → call driver->start_recording(ctx)
                                                                 arm grace_until_us = now + RECORDING_STATUS_GRACE_MS
if desired_recording == STOP   && status == ACTIVE            → call driver->stop_recording(ctx)
                                                                 arm grace_until_us = now + RECORDING_STATUS_GRACE_MS
```

**Grace period.** After issuing `start_recording()` / `stop_recording()` (or their broadcast variants) from **either** path — immediate-dispatch in §13.3a or this poll — `camera_manager` arms a per-slot deadline `grace_until_us = esp_timer_get_time() + RECORDING_STATUS_GRACE_MS` (default **10 s**). Subsequent mismatch poll ticks are no-ops while `now < grace_until_us`. The deadline expires on its own; the poll does not reset it per tick.

10 s comfortably covers the worst-case time for the camera to reflect the new state in its status report:

- BLE-control cameras stream status notifications, but a freshly-issued `SetShutter` may take several seconds before the next `system_record_mode_active` notification fires.
- WiFi RC cameras are polled by the driver every 5 s, so the cache may be up to one full poll interval stale.

A shorter window risked re-issuing a Start to a camera that was already recording but hadn't yet told us — which on Hero4 in RC mode has been observed to flip the camera back off. After the grace deadline expires, the next poll re-evaluates and re-arms the deadline if it still has work to do, so the self-healing "missed shutter" safety net is preserved; it just runs on a slower beat.

**UNKNOWN suppresses correction (both sides).** If `desired_recording == DESIRED_RECORDING_UNKNOWN`, the system has no established intent and takes no action — this is the boot state before the first CAN frame or UI command. If `status == CAMERA_RECORDING_UNKNOWN`, the driver has not yet observed the camera and correction is suppressed to avoid re-issuing commands repeatedly during a transient transport failure when the camera might already be in the desired state.

**Note:** if a camera cannot record for hardware reasons (no SD card, storage full, overheating), the mismatch will persist indefinitely and a correction command will be sent on every poll cycle. This is acceptable for the current use case — these are operator errors in a race context. A retry counter or error-state detection can be added later if log spam becomes a problem.

### 13.5 Vtable

See §8 for the canonical signature. The vtable has six entries: `start_recording`, `stop_recording`, `get_recording_status`, `teardown` (nullable), `update_slot_index` (nullable), and `on_wifi_disconnected` (nullable; RC-only).

---

## 14. CAN Manager

### 14.1 Hardware

| Parameter | Value |
|---|---|
| Baud rate | 1 Mbps |
| TX GPIO | 7 |
| RX GPIO | 6 |
| RX queue depth | 32 frames |
| TX queue depth | 8 frames |
| Termination | Hardware (120 Ω solder jumpers on board — enabled by default for end-node use) |

### 14.2 CAN Protocol

All frames use standard 11-bit IDs.

**`0x600` — RaceCapture → ESP32: logging command**

Payload byte 0 is the `isLogging` flag. The manager fires the `can_logging_state_cb_t` callback on **every received frame**, passing `LOGGING_STATE_LOGGING` or `LOGGING_STATE_NOT_LOGGING` based on the byte. The callback consumer (`camera_manager`) is responsible for idempotent handling — see §13.2.

State defaults to `LOGGING_STATE_UNKNOWN` on boot. The first received `0x600` frame transitions out of `UNKNOWN`. If no `0x600` frame is received within 5 seconds at any point during operation, the state reverts to `LOGGING_STATE_UNKNOWN` and the callback is **not** fired with `UNKNOWN` — `UNKNOWN` is only used internally and reflected in `can_manager_get_logging_state()` for the web UI. The 5 s timeout covers RaceCapture power-off, cable fault, and bus-off conditions.

**`0x601` — ESP32 → RaceCapture: camera status broadcast**

Transmitted at 5 Hz (every 200 ms) by a periodic `esp_timer` callback regardless of bus activity. Bytes 0–3 carry `CAMERA_STATE_*` values for Cam 1, Cam 2, Cam 3, Cam 4 respectively (the externally-visible camera numbering is 1-based; the CAN frame's first byte corresponds to the first paired camera):

```
CAMERA_STATE_UNDEFINED    = 0   Slot not configured / no information yet
CAMERA_STATE_DISCONNECTED = 1   Camera not found or connection lost
CAMERA_STATE_IDLE         = 2   Connected, not recording
CAMERA_STATE_RECORDING    = 3   Connected and actively recording
```

Values are unsigned integers mapped directly to RaceCapture direct-CAN channels. The mapping must not be reordered without updating the RaceCapture configuration.

`can_manager` reads the current state for each slot using:

```c
camera_can_state_t camera_manager_get_slot_can_state(int slot);
```

This function is defined in `camera_manager` and translates the slot's `wifi_status` and `recording_status` into the appropriate `CAMERA_STATE_*` value, keeping the translation logic co-located with the data it reads. `can_manager` calls it directly when building the `0x601` payload — it does not go through `camera_slot_info_t`.

**`0x602` — RaceCapture → ESP32: GPS UTC timestamp**

A 64-bit little-endian Unix epoch in milliseconds, broadcast by a RaceCapture Lua script at 25 Hz once GPS lock is acquired. The first valid frame (year > 2020) of a boot session fires the `can_utc_acquired_cb_t` callback exactly once — used to set the date and time on all currently-connected cameras. Subsequent frames update the stored epoch and the monotonic clock reference used for extrapolation.

The UTC anchor tracks two flags:

- `valid` — any anchor is available, including one restored from NVS on boot.
- `session_synced` — the anchor came from a live source **this** boot session: either a `0x602` GPS frame or a successful `can_manager_set_manual_utc_ms()` call from the web UI. An NVS-restored value does **not** set this flag.

`can_manager_get_utc_ms()` uses the current anchor plus elapsed `esp_timer_get_time()` to return the estimated UTC without waiting for the next CAN frame. Returns `false` only when no anchor exists at all (first-ever boot with no GPS lock and no manual set yet).

`can_manager_utc_is_session_synced()` returns `true` only after a live source has set the anchor this session. It is the gate used by camera drivers to decide whether to push `SetDateTime` to a connected camera — an NVS-restored value is "close" but not authoritative, so we'd rather leave the camera's own clock alone than overwrite it with a stale value.

When a live UTC source updates the anchor, `can_manager` also calls `settimeofday()` so libc clock APIs (`time()`, `gettimeofday()`) used elsewhere return useful values. The same call is made during NVS restore so the system clock comes up approximately correct without a GPS fix.

### 14.3 NVS Persistence

`can_manager` owns the namespace `can_mgr` with two keys:

| Key | Type | Purpose |
|---|---|---|
| `tz_off` | `i8` | UTC offset in whole hours, clamped to IANA valid range `[−12, +14]` |
| `last_utc` | `u64` | Most recent best-estimate UTC (ms since epoch) for cross-boot continuity |

`tz_off` is applied when setting camera date/time so recorded clips have correct local timestamps. Loaded on `can_manager_init()` before any camera clock operations.

`last_utc` is written:

1. Immediately on the first session sync (live GPS frame or manual set).
2. Every 5 minutes thereafter, while the anchor is valid, by a periodic `esp_timer`.

On `can_manager_init()` the saved value is loaded into the in-memory anchor and pushed to the system clock. The anchor is marked `valid` but **not** `session_synced` — so the device boots with an approximately correct clock for log timestamps and any other system-level use of `gettimeofday`, while still treating live sync as authoritative for camera time-pushes. Drift during power-off is unrecoverable; the persisted value is "the last thing we knew before losing power", which is as close as we can get without an RTC.

### 14.4 Threading Model

A single FreeRTOS task (`s_rx_task`, priority 5) dequeues frames from `s_rx_queue` and processes them. The TWAI ISR enqueues raw frames. Camera state bytes are `volatile uint8_t` — single-byte writes are atomic on Xtensa LX7 without a mutex. UTC state is protected by a mutex since it is a multi-field structure read from multiple tasks.

### 14.5 Callbacks

| Callback | Fires when |
|---|---|
| `can_rx_frame_cb_t` | Every received frame — for development and bus sniffing |
| `can_logging_state_cb_t` | Every received `0x600` frame (consumer is responsible for idempotency — see §13.2) |
| `can_utc_acquired_cb_t` | Exactly once per boot session — on the first valid `0x602` frame **or** the first successful `can_manager_set_manual_utc_ms()` call. NVS-restored anchors at boot do not fire it. |

All callbacks are invoked from the RX task context. Implementations must not block indefinitely.

---

## 15. open_gopro_ble

### 15.1 Responsibilities

`open_gopro_ble` is the complete BLE control driver for Hero 9+ cameras. It registers as a `camera_driver_t` for `gopro_model_uses_ble_control()` models and owns every aspect of the camera's lifecycle on its primary transport.

- **Discovery**: scan for cameras advertising the GoPro service UUID (`0xFEA6`); populate the discovered list for the web UI.
- **Pairing & GATT setup**: register newly bonded cameras with `camera_manager`, run the explicit MTU exchange (Hero 13 doesn't initiate one itself), discover services and characteristics across the full handle range, and subscribe CCCDs sequentially.
- **Readiness sequence (V1-style, §15.5)**: `GetHardwareInfo` poll → `SetCameraControlStatus(EXTERNAL)` handshake → `camera_manager_on_camera_ready(slot)` → `SetDateTime` → start the 5 s status poll. The connection sequence proceeds even if the `SetCameraControlStatus` response times out — a silent camera should not stall setup.
- **Recording control**: `SetShutter` (TLV 0x01) on GP-0072 from the `start_recording` / `stop_recording` vtable entries.
- **Recording status poll**: `GetStatusValue` (TLV 0x13, status ID 8 = `system_record_mode_active`) on GP-0076 every 5 s; cached value exposed via `get_recording_status`.
- **Date / time**: `SetDateTime` (TLV 0x0D) on GP-0072. Best-effort; gated on `can_manager_utc_is_session_synced()` so an NVS-restored UTC anchor at boot cannot push stale time to the camera. If UTC isn't session-synced when the readiness sequence completes, the slot's `datetime_pending_utc` flag is set and `open_gopro_ble_sync_time_all()` consumes it when UTC arrives.
- **BLE keepalive**: send `0x42` to GP-0074 every 3 seconds. Required to maintain the BLE link supervision timer and prevent camera auto-sleep.

### 15.2 Discovery

The `on_disc` callback (registered with `ble_core`) filters advertisements by the GoPro BLE service UUID `0xFEA6`. Devices that match are added to a 10-entry discovery list (`gopro_device_t[]`) with name, address, and RSSI. The list is cleared when a new scan starts.

The `http_server` component reads this list via `open_gopro_ble_get_discovered()` when serving `GET /api/cameras`.

### 15.3 GATT Characteristics

All handles are 128-bit GoPro UUIDs of the form `b5f9XXXX-aa8d-11e3-9046-0002a5d5c51b`. Only six characteristics are now used (the network-management and WiFi-AP-control families have been removed along with COHN):

| Handle name | UUID suffix | Direction | Purpose |
|---|---|---|---|
| `cmd_write` | `0072` | Write | TLV commands (`GetHardwareInfo`, `SetShutter`, `SetDateTime`) + `SetCameraControlStatus` protobuf |
| `cmd_resp_notify` | `0073` | Notify | Command responses (TLV + protobuf) |
| `settings_write` | `0074` | Write | Keepalive (`0x42`) |
| `settings_resp_notify` | `0075` | Notify | Settings responses (acknowledged but not acted on) |
| `query_write` | `0076` | Write | TLV queries (`GetStatusValue`) |
| `query_resp_notify` | `0077` | Notify | Query responses (status updates) |
| `nw_mgmt_write` | `0091` | Write | Network-management protobuf (`RequestPairingFinish`) |
| `nw_mgmt_resp_notify` | `0092` | Notify | Network-management responses |
| `wifi_ap_state_indicate` | `0005` | Indicate | WiFi AP state push (0/1); logged at debug level only |

CCCD subscriptions are required on every connection — GoPro cameras do not persist CCCD state. The discovery loop subscribes to **every** 128-bit notify/indicate characteristic it finds and matches the UUID to a handle field for routing; unmatched UUIDs are still subscribed but their notifications are logged and dropped, so adding a new camera characteristic in the future starts as a recognised log line rather than a silent miss.

### 15.4 Per-Camera Driver Context (`gopro_ble_ctx_t`)

A single static array `s_ctx[CAMERA_MAX_SLOTS]` is initialised in `open_gopro_ble_init()`; `ctx_create(slot)` simply hands back `&s_ctx[slot]`. The context is passed to all vtable calls.

```c
typedef struct {
    uint16_t             conn_handle;
    int                  slot;
    gopro_gatt_handles_t gatt;             /* zeroed on disconnect */
    uint16_t             negotiated_mtu;

    /* GetHardwareInfo readiness poll */
    bool               readiness_polling;
    uint8_t            readiness_retry_count;
    esp_timer_handle_t readiness_timer;    /* one-shot, 3 s, retries GetHardwareInfo */

    /* SetCameraControlStatus handshake */
    bool               cam_ctrl_pending;
    esp_timer_handle_t cam_ctrl_timer;     /* one-shot, 3 s; advances on timeout */

    /* SetDateTime defer */
    bool               datetime_pending_utc;

    /* Periodic timers */
    esp_timer_handle_t keepalive_timer;    /* 3 s periodic */
    esp_timer_handle_t status_poll_timer;  /* 5 s periodic GetStatusValue */

    /* Recording status — atomic single-enum read for vtable get_recording_status */
    camera_recording_status_t cached_status;
} gopro_ble_ctx_t;
```

### 15.5 Connection State Machine

Every event on this path runs on the NimBLE host task or the esp_timer task.

```
on_connected(conn_handle, addr)
  -> camera_manager_find_by_mac(addr)
      known camera -> camera_manager_on_ble_connected(slot, conn_handle)
                      store conn_handle in ctx
      unknown      -> registered in on_encrypted below

on_encrypted(conn_handle, addr)
  -> if unknown: camera_manager_register_new()
  -> snapshot ATT MTU via ble_att_mtu(); if zero, default to BLE_ATT_MTU_DFLT
  -> ble_gattc_exchange_mtu()  (Hero 13 doesn't initiate one itself)
  -> on MTU exchange complete -> gopro_gatt_start_discovery(ctx)

GATT discovery
  1. ble_gattc_disc_all_svcs -> for each service:
     ble_gattc_disc_all_chrs -> record GP-00XX handles into ctx->gatt
  2. Subscribe CCCDs on every notify/indicate characteristic
     (4 notify channels GP-0073/0075/0077/0092 + GP-0005 WiFi AP State indicate)
  3. All CCCDs done -> ble_core_resume_background_scan()
                       (lets a second paired camera connect while this slot
                        finishes readiness; no-op if no other slots disconnected)
                    -> gopro_readiness_start(ctx)

Readiness poll
  -> Send GetHardwareInfo (TLV 0x3C) to cmd_write (GP-0072) immediately
  -> Arm 3 s one-shot readiness_timer
  -> on response (cmd_resp_notify, GP-0073):
      status 0x00 -> hardware ready -> gopro_on_hw_info_ok()
      non-zero    -> retry; up to GOPRO_READINESS_RETRY_MAX (10)
      timer fires -> retry; on overflow -> ble_gap_terminate()

gopro_on_hw_info_ok(ctx, model_num)
  -> parse the positional LV body (model #, model name, firmware, serial,
     AP SSID, AP MAC) and log it
  -> camera_manager_set_model(slot, (camera_model_t)model_num)
  -> camera_manager_save_slot(slot)
  -> camera_manager_on_ble_ready(slot)
  -> gopro_keepalive_start(ctx)
  -> send_set_cam_ctrl(ctx)
        Build packet { 0x04, 0xF1, 0x69, 0x08, 0x02 } -> GP-0072
        Set ctx->cam_ctrl_pending = true
        Arm 3 s cam_ctrl_timer
        On ResponseGeneric (Feature 0xF1, Action 0xE9) with any result,
          OR on cam_ctrl_timer firing first
            -> complete_connection_sequence(ctx)

complete_connection_sequence(ctx)
  -> camera_manager_on_camera_ready(slot)        → WIFI_CAM_READY
  -> if can_manager_utc_is_session_synced():
       gopro_control_set_datetime(ctx)
     else:
       ctx->datetime_pending_utc = true
  -> gopro_status_poll_start(ctx)
        Periodic GetStatusValue every 5 s; first request fires immediately

open_gopro_ble_sync_time_all()
  -> for every connected slot:
       gopro_control_set_datetime(ctx)
       ctx->datetime_pending_utc = false
```

### 15.6 Recording Commands

The driver vtable maps to BLE writes:

```
drv_start_recording(ctx)
  -> gopro_control_send_shutter(ctx, true)
       Build packet { 0x03, 0x01, 0x01, 0x01 } -> GP-0072
  -> ctx->cached_status = CAMERA_RECORDING_ACTIVE   (optimistic)

drv_stop_recording(ctx)
  -> gopro_control_send_shutter(ctx, false)
       Build packet { 0x03, 0x01, 0x01, 0x00 } -> GP-0072
  -> ctx->cached_status = CAMERA_RECORDING_IDLE     (optimistic)

drv_get_recording_status(ctx)
  -> return ctx->cached_status   (no lock; single 4-byte enum on Xtensa LX7)
```

The optimistic `cached_status` update means the next mismatch poll cycle (§13.3b) sees the state we just commanded; the next status poll cycle (within 5 s) confirms or corrects it. If the camera silently refuses the shutter, the optimistic value is overwritten with the real state on the next poll and the mismatch loop will issue a fresh command.

### 15.7 Recording Status Poll

A per-slot 5 s `esp_timer` (`status_poll_timer`) issues `GetStatusValue` on GP-0076. The request payload is fixed at 3 bytes: `{ 0x02, 0x13, 0x08 }` (GPBS header len=2, cmd_id=0x13, status_id=8).

The response on GP-0077 has the form:

```
[cmd_id, status, (id, len, value)*]
```

`status.c` strips the 2-byte TLV header and walks the body looking for `id == 0x08`; the byte that follows the length is `0` (idle) or `1` (recording). `cached_status` is updated only when the value changes, to avoid log spam.

The first poll fires immediately on `gopro_status_poll_start()` so `cached_status` updates within ~one MTU latency rather than waiting the full 5 s.

### 15.8 Disconnect Cleanup

On `on_disconnected(conn_handle, addr, reason)` (fires before `camera_manager` clears the handle):

1. `gopro_readiness_cancel(ctx)` — stops and deletes `readiness_timer` and `cam_ctrl_timer`
2. `gopro_keepalive_stop(ctx)`
3. `gopro_status_poll_stop(ctx)`
4. `gopro_query_free(ctx)` — wipes per-slot reassembly buffers
5. Clear `gopro_ble_ctx_t`: zero `gatt` handles, set `conn_handle = GOPRO_CONN_NONE`, `cached_status = UNKNOWN`. `datetime_pending_utc` is **not** cleared — the user may reconnect before UTC arrives.
6. `camera_manager_on_ble_disconnected_by_handle(conn_handle)` — clears `ble_handle`, sets `ble_status = CAM_BLE_NONE`

Timer cleanup must happen before the slot's driver context is touched to prevent stale callbacks dereferencing freed state.

### 15.9 NVS Ownership

This component owns no NVS keys. The slot record (`cam_N/camera`) is owned by `camera_manager`; bond storage is owned by NimBLE.

### 15.10 Public API

```c
void open_gopro_ble_init(void);

/* Discovery */
void open_gopro_ble_start_discovery(void);
void open_gopro_ble_stop_discovery(void);
int  open_gopro_ble_get_discovered(gopro_device_t *out, int max_count);

/* Connection */
void open_gopro_ble_connect_by_addr(const ble_addr_t *addr);

/* UTC sync — called from main.c on first live UTC sync (GPS or manual web set).
 * Pushes SetDateTime to every connected slot.  No-op for any slot when
 * can_manager_utc_is_session_synced() is false (so an NVS-restored value at
 * boot will not trigger camera time updates). */
void open_gopro_ble_sync_time_all(void);
```

The recording API (`start_recording` / `stop_recording` / `get_recording_status`) is exposed only through the `camera_driver_t` vtable registered with `camera_manager`.

### 15.11 Source File Layout

| File | Contents |
|---|---|
| `include/open_gopro_ble_spec.h` | All raw protocol constants: GoPro GATT UUIDs, command IDs, GPBS header format, protobuf field tags, response field offsets, poll intervals, retry caps. The canonical reference layer — all `.c` files include it rather than embedding raw byte literals. |
| `include/open_gopro_ble.h` | Public API |
| `open_gopro_ble_internal.h` | Private shared types (`gopro_gatt_handles_t`, `gopro_ble_ctx_t`, `gopro_channel_t`) and internal function declarations |
| `driver.c` | Per-slot context table, discovery list, `camera_driver_t` vtable, driver registration, `open_gopro_ble_init()` |
| `pairing.c` | `on_connected` / `on_encrypted` / `on_disconnected` callbacks; explicit MTU exchange |
| `gatt.c` | Service / characteristic discovery, sequential CCCD subscription state machine |
| `readiness.c` | `GetHardwareInfo` retry loop, hardware-info LV parser, `SetCameraControlStatus(EXTERNAL)` handshake with timeout, drives the post-readiness sequence |
| `control.c` | `SetDateTime`, `SetCameraControlStatus`, `SetShutter` packet builders; 3 s keepalive timer |
| `status.c` | 5 s `GetStatusValue` poll timer; status response parser |
| `query.c` | GPBS packet reassembler (general / ext-13 / ext-16 headers, continuation packets); 3-channel response dispatch |
| `notify.c` | `on_notify_rx` callback — maps `attr_handle` to `gopro_channel_t`, feeds `gopro_query_feed()` |

---

## 16. open_gopro_http (removed)

**This section is intentionally retained as a tombstone for cross-references.** The `open_gopro_http` component was deleted in the COHN removal pass (2026-05-03). All recording control for Hero 9+ cameras now lives in `open_gopro_ble` (§15); see §15.6 for shutter, §15.7 for status poll, §10 for the transport summary.

Historical context: this component was an `esp_http_client`-based HTTPS driver that issued `GET /gopro/camera/state`, `/shutter/start`, `/shutter/stop`, etc., authenticated via Basic Auth against COHN credentials provisioned over BLE. It was removed because BLE alone proved sufficient and removed an entire axis of failure (TLS, COHN cert lifecycle, dual-MAC slot tracking, 401 re-provisioning).

## 17. gopro_wifi_rc

WiFi Remote Control emulation driver for the GoPro "Smart Remote" UDP protocol, accepted by Hero3 / Hero3+ / Hero4 / Hero5 / Hero7 / Hero8 as a backwards-compatible control channel. All recurring traffic — keepalive, status poll, shutter — is short binary UDP datagrams between this device's SoftAP (src port 8383) and the camera (dst port 8484). HTTP is used **only** for the optional date/time set on cameras that run an HTTP server on their STA-interface IP (Hero4+); it is not on the critical control path. Camera slot persistence stays in `camera_manager`; MAC OUI spoofing in `wifi_manager`; WoL drives reconnect for cameras that sleep while still associated to the SoftAP.

### 17.1 Responsibilities

| Responsibility | Notes |
|---|---|
| Implement `camera_driver_t` vtable | `start_recording`, `stop_recording`, `get_recording_status` |
| Station lifecycle | React to L2 associate / DHCP / disassociate events from `wifi_manager` |
| WoL on associate without DHCP | Send magic packet × 5; retry every 2 s if camera silent for > 10 s |
| UDP keepalive | `_GPHD_:0:0:2:0.000000\n` unicast, every 3 s — fire-and-forget |
| UDP status poll | Binary `st` opcode every 5 s; response parsed for power + recording state |
| UDP shutter | Binary `SH` opcode (param 0x02 / 0x00). Broadcast to `255.255.255.255:8484` × 3 for "fire all" (CAN / Start All); unicast to `ctx->last_ip:8484` × 1 for the mismatch poll and per-slot web-UI commands |
| UDP camera-version (`cv`) identify | Sent at pair time and re-sent on every keepalive tick until the camera answers; response is a length-prefixed firmware string + model name (`HD7.01.01.90.00` / `HERO7 Black` on a real Hero7).  Drives `gopro_model_from_name()` to set the slot's model (camera_model_t enum) with no HTTP involvement.  The slot's name field is left blank — the cv response carries the model identity, not a user-set device name, and there is no known WiFi RC protocol path to retrieve the latter. |
| HTTP date/time (best-effort) | URL-encoded hex bytes; gated on `gopro_model_supports_http_datetime()` — currently Hero4 Black/Silver only.  Hero5+/Hero7 silently drop port-80 SYNs in Smart-Remote mode, so this stays narrow until verified per-model. |
| Discovery exposure | Unmanaged SoftAP stations whose MAC OUI is on the GoPro allow-list (`GOPRO_RC_OUIS[]` in `api_rc.c` — IEEE MA-L registrations to Woodman Labs / GoPro) are exposed via `/api/rc/discovered` for the manual Add flow. Other vendors (phones, laptops) are filtered out by `http_server`. |

### 17.2 Protocol Overview

| Transport | Local | Remote | Direction | Purpose |
|---|---|---|---|---|
| UDP unicast | 8383 | 8484 | ESP32 → camera | Keepalive `_GPHD_`, status `st`, per-slot shutter `SH`, identify `cv` |
| UDP broadcast | 8383 | 8484 | ESP32 → 255.255.255.255 | "Fire all" shutter `SH` × 3 (`set_desired_recording_all`); requires `SO_BROADCAST` on the socket |
| UDP unicast | 8383 | (from cam:8484) | camera → ESP32 | Replies — single bound socket |
| UDP broadcast | — | 9 | ESP32 → 255.255.255.255 | WoL magic packet × 5 |
| HTTP/1.0 | — | 80/TCP | ESP32 → camera | Date/time set — Hero4 Black/Silver only, best-effort |

A single `SOCK_DGRAM` bound to local port 8383 carries every UDP send and receive. Hero3-era cameras reply to the source port of the keepalive — binding to 8383 gives the legitimate "WiFi Remote" source port the camera expects. HTTP/1.0 is required for command endpoints; Hero4 returns 500 to standard HTTP/1.1 requests with extra headers. The date/time set targets the camera's DHCP-assigned STA IP, not the documented `10.5.5.9` (which is the camera's own AP IP and is unreachable when the camera is associated to us). **Identification has moved entirely to UDP** (`cv` opcode); the HTTP probe was dropped in 2026-05 after diagnostic probes confirmed Hero5/6/7/8 do not run an HTTP server on their STA interface in Smart-Remote mode — only Hero4 does, and Hero4 also answers `cv`, so HTTP isn't needed for identify on any model we support.

#### 17.2.1 Keepalive

Payload: `_GPHD_:0:0:2:0.000000\n` (22 ASCII bytes). Camera reply: `_GPHD_:0:0:2:\x01` (14 bytes, first byte = `0x5F`). The RX-task dispatch path that detects ACKs by `buf[0] == 0x5F` is correct and retained.

#### 17.2.2 Binary command format (`st`, `SH`, future: `CM`, `PW`, `YY`)

Every binary packet shares this header:

| Bytes | Field | Notes |
|---|---|---|
| 0–7 | Reserved | Zeros |
| 8 | Selector | `0x00` GET, `0x01` SET |
| 9–10 | Sequence bytes | Static per opcode (verified working against Hero3/4 with no rolling counter) |
| 11–12 | ASCII opcode | e.g. `'s','t'`, `'S','H'` |
| 13… | Parameters | Per-opcode |

#### 17.2.3 Opcodes used by this driver

Templates are copied verbatim from the Lua reference (verified working against Hero4) and the ESP8266 reference sketch. Both use `byte 8 == 0x00` for the shutter command — the public docs describe byte 8 as a GET/SET selector, but the Hero3-era cameras accept `0x00` for both query and command opcodes, so we follow the verified-working values rather than the documented selector convention.

| Opcode | Byte 8 | Bytes 9–10 | Length | Purpose |
|---|---|---|---|---|
| `_GPHD_…` | (ASCII) | — | 22 B | Keepalive |
| `s t` | 0x00 | 0x00, 0x00 | 13 B | Status request |
| `S H` | 0x00 | 0x01, 0x00 | 14 B | Shutter — param byte 13: 0x02=start, 0x00=stop |
| `c v` | 0x00 | 0x00, 0x00 | 13 B | Camera-version identify — reply is variable-length (see §17.2.5) |

Stored as `static const uint8_t` byte templates in `gopro_wifi_rc_spec.h` (`RC_PKT_ST`, `RC_PKT_SH_START`, `RC_PKT_SH_STOP`, `RC_PKT_CV`); send wrappers `sendto` directly from the template — no scratch buffer or counter state required.

#### 17.2.4 Status (`st`) response parse

20-byte reply. After bytes 0–10 (echo) and 11–12 (`'s','t'`):

| b13 | b14 | b15 | meaning |
|---|---|---|---|
| 1 | × | × | camera off / sleeping → `RECORDING_UNKNOWN` |
| 0 | mode | 1 | recording → `RECORDING_ACTIVE` |
| 0 | mode | 0 | idle → `RECORDING_IDLE` |

`b14` is mode (0=video, 1=photo, 2=burst, 3=timelapse) — not consumed today; reserved for future UI.

#### 17.2.5 `cv` response decode (UDP identify)

44-byte reply (verified against a real Hero7 with firmware HD7.01.01.90.00).
After bytes 0–10 (header echo) and 11–12 (`'c','v'`):

| Bytes | Field | Notes |
|---|---|---|
| 13–15 | Reserved / response-format | Observed `0x00 0x03 0x01` on Hero7; not parsed |
| 16 | Firmware-string length | uint8 (e.g. `0x0F` = 15) |
| 17 .. 16+fw_len | Firmware version | ASCII, no NUL terminator |
| 16+1+fw_len | Model-name length | uint8 (e.g. `0x0B` = 11) |
| .. | Model name | ASCII, no NUL terminator |

Decode lives in `rc_parse_cv_response()` (`status.c`):

```
rc_parse_cv_response(slot, buf, len)
  -> validate fw_len + name_len fit within len
  -> copy firmware  → ctx->parsed_firmware    (capped 39 bytes + NUL)
  -> copy model_name → ctx->parsed_model_name (capped 31 bytes + NUL)
  -> log INFO "slot N: cv parsed — model='HERO7 Black' fw='HD7.01.01.90.00'"
  -> post CMD_APPLY_CV(slot)
```

The work-task handler `rc_handle_apply_cv()` then maps the name string via
`gopro_model_from_name()`, calls `camera_manager_set_model()` /
`save_slot()`, and flips `ctx->identify_attempted = true` so the keepalive
tick stops re-sending `cv`.  The slot's name field is **not** populated
from the cv response — the model_name string is the camera's model
identity, not a user-set device name, and there is no known WiFi RC
protocol path to retrieve the latter.

**Retry policy:** `cv` is sent once during the pair prime, again on
promotion if no response yet, and on every keepalive tick (3 s) thereafter
while `identify_attempted` is false.  Some cameras drop the very first `cv`
(arrives before the RC pairing has fully settled) — the retry window keeps
nudging until the camera answers.  If it never does, the slot stays at
`CAMERA_MODEL_GOPRO_HERO_LEGACY_RC` (the default set by `add_camera`) and
all UDP control still works; only the resolved model enum and HTTP-datetime
capability check are missing.

Identification is **not** on the readiness path — `wifi_ready` flips on the
first datagram of any kind from the camera; `cv` simply settles the model
asynchronously when the camera gets around to answering.

#### 17.2.6 Date/time HTTP format

```
GET /gp/gpControl/command/setup/date_time?p=%YY%MM%DD%hh%mm%ss HTTP/1.0\r\n\r\n
```

Each `%XX` is URL-encoded hex of one binary byte, in this order:
1. `year mod 100`
2. `month` (1–12)
3. `day` (1–31)
4. `hour` (0–23)
5. `minute` (0–59)
6. `second` (0–59)

Example for 2026-05-04 14:30:00 → `?p=%1A%05%04%0E%1E%00`.

Times are local; `can_manager`'s tz offset must be applied before encoding (existing TODO — see 17.13).

### 17.3 Per-Camera Driver Context (`gopro_wifi_rc_ctx_t`)

```c
typedef struct {
    int                       slot;
    uint8_t                   mac[6];        /* WoL target */
    uint32_t                  last_ip;       /* network byte order */

    camera_recording_status_t recording_status;
    bool                      wifi_ready;            /* true after first UDP datagram */
    bool                      identify_attempted;    /* set true once cv has been applied */

    /* Populated by the RX task on cv response, consumed by the work task in
     * handle_apply_cv / handle_promote.  Empty until cv arrives. */
    char                      parsed_model_name[32]; /* e.g. "HERO7 Black" */
    char                      parsed_firmware[40];   /* e.g. "HD7.01.01.90.00" */

    TickType_t                last_response_tick;    /* refreshed by RX on any datagram */
    esp_timer_handle_t        keepalive_timer;       /* 3 s periodic */
    esp_timer_handle_t        wol_retry_timer;       /* 2 s periodic; armed on > 10 s silence */
} gopro_wifi_rc_ctx_t;
```

`last_keepalive_ack` was renamed to `last_response_tick` — liveness is refreshed by any UDP datagram from the camera (keepalive ACK, `st` response, `SH` echo, or `cv` response). `identify_attempted` is set when `apply_cv` successfully resolves the model — at that point the keepalive tick stops sending further `cv` probes. The `parsed_*` strings are written only by the RX task and read only by the work task (single-writer / single-reader, no mutex).

### 17.4 Threading Model

**Shutter task** (`gopro_rc_shutter`, priority 7): Drains `s_shutter_queue`. Each enqueued `rc_shutter_cmd_t` carries `{start, ip, repeat}` — the task sends `repeat` copies of the appropriate `SH` packet to `ip:8484` and is otherwise transport-agnostic. Two routing styles use the same task:

- **Broadcast** (`set_desired_recording_all` → `drv_start_recording_all` / `_stop_recording_all`): `ip = 0xFFFFFFFFu`, `repeat = RC_SHUTTER_BROADCAST_REPEAT` (3). 802.11 broadcasts are unacknowledged, so three back-to-back sends compensate for single-frame loss; the burst completes in well under 1 ms at 54 Mbps.
- **Unicast** (`set_desired_recording_slot` and the mismatch poll → `drv_start_recording(ctx)` / `drv_stop_recording(ctx)`): `ip = ctx->last_ip`, `repeat = 1`. The link-layer 802.11 retry handles drops; a duplicate Start to a Hero4 that's already recording has been observed to flip it back off, so single-camera commands stay targeted at the one IP.

Higher priority guarantees shutter latency.

**Work task** (`gopro_rc_work`, priority 5): Station lifecycle, WoL, keepalive tick (which also drives cv-retry), UDP status poll, slot promotion, cv-data apply, HTTP date/time sync.

**UDP RX task** (`gopro_rc_udp_rx`, priority 4): `recvfrom` on the shared 8383 socket. Dispatches by content (see 17.4.1). On any datagram from a known slot, updates `last_response_tick`; if the slot is not yet `wifi_ready`, posts `CMD_PROMOTE` to the work queue. On `cv` responses additionally posts `CMD_APPLY_CV`.

```
wifi_manager callbacks
  -> on_station_associated(mac)      -> work queue: CMD_STATION_ASSOCIATED
  -> on_station_dhcp(mac, ip)        -> work queue: CMD_STATION_DHCP
  -> on_station_disassociated(mac)   -> work queue: CMD_STATION_DISCONNECTED

esp_timer (keepalive, 3 s, per slot) -> work queue: CMD_KEEPALIVE_TICK(slot)
esp_timer (WoL retry, 2 s, per slot) -> work queue: CMD_WOL_RETRY(slot)
esp_timer (status poll, 5 s, global) -> work queue: CMD_STATUS_POLL_ALL

UDP RX task (any datagram from known slot updates last_response_tick and
  posts CMD_PROMOTE if !wifi_ready)
  - 0x5F keepalive ACK            -> liveness only
  - "st" response                 -> parse b13/b14/b15 → recording_status
  - "SH" echo (shutter ACK)       -> liveness only
  - "cv" response                 -> rc_parse_cv_response() →
                                       fill parsed_model_name + parsed_firmware,
                                       post CMD_APPLY_CV(slot)
  - anything else                 -> log and discard

camera_driver_t vtable
  -> start_recording / stop_recording -> shutter queue (UDP SH)
  -> get_recording_status              -> read ctx->recording_status directly
```

`CMD_PROBE` was removed when the design switched to UDP-driven promotion. `CMD_HTTP_IDENTIFY` was removed when the cv path replaced the HTTP `/gp/gpControl` probe (2026-05). Current cmd types: `CMD_STATION_*`, `CMD_KEEPALIVE_TICK`, `CMD_WOL_RETRY`, `CMD_STATUS_POLL_ALL`, `CMD_PROMOTE`, `CMD_APPLY_CV`, `CMD_SYNC_TIME_ALL`.

#### 17.4.1 RX dispatch rules

```
buf[0] == 0x5F                                              → keepalive ACK
buf[11] == 's' && buf[12] == 't' && n>=16                   → status response
buf[11] == 'S' && buf[12] == 'H' && n>=13                   → shutter echo
buf[11] == 'c' && buf[12] == 'v' && n>RC_CV_RESP_FW_LEN_OFFSET → camera-version response
default                                                      → log/discard
```

Any of these paths can promote a slot to ready (whichever datagram lands first).

### 17.5 Connection Flow

**Rule 1: ignore unknown MACs.** If `camera_manager_find_by_addr(mac)` returns no slot, the MAC is ignored entirely.

**Rule 2: ignore non-RC slots.** After looking up the slot by MAC, check `gopro_model_uses_rc_emulation(slot->model)`. If false, return immediately.

```c
static int find_managed_slot(const uint8_t mac[6])
{
    int slot = camera_manager_find_by_addr(mac);
    if (slot < 0) return -1;
    camera_model_t model = camera_manager_get_model(slot);
    if (!gopro_model_uses_rc_emulation(model)) return -1;
    return slot;
}
```

**Station associates with DHCP lease** — camera awake:

```
handle_station_dhcp(mac, ip)
  -> ctx->last_ip = ip
     camera_manager_save_slot(slot)
     prime: rc_send_keepalive(ip)
            rc_send_st(ip)
            rc_send_cv(ip)
     arm keepalive timer (3 s)
  -> first response from camera (ACK / st / cv) → CMD_PROMOTE
  -> cv response (whenever it arrives)         → CMD_APPLY_CV
```

The same prime sequence runs from `gopro_wifi_rc_add_camera()` when the user first registers the camera via `/api/rc/add`.

**Station associates without DHCP lease** — may be sleeping:

```
handle_station_associated(mac)
  -> ip = camera_manager_get_last_ip(slot)
     if ip == 0:  return        /* CMD_STATION_DHCP will follow if camera wakes */
     send WoL magic packet × 5
     prime: rc_send_keepalive(ip)
     arm keepalive timer
```

**Promotion** (driven by the first received UDP datagram):

```
handle_promote(slot)
  -> ctx->wifi_ready = true
  -> camera_manager_on_camera_ready(slot)
  -> active_pair = pair_attempt_addr_matches(ctx->mac)
  -> if !identify_attempted:
        if parsed_model_name is set (cv arrived first):
            identify_attempted = true
            apply (gopro_model_from_name + set_model + save_slot)
            /* name field intentionally left blank — see §17.2.5 */
            if active_pair: pair_attempt_set_model(mapped)
        else:
            rc_send_cv(ip)     /* nudge — keepalive_tick will keep retrying */
  -> if active_pair: pair_attempt_advance(SUCCESS)
  -> rc_send_datetime(slot)    /* gated on gopro_model_supports_http_datetime() */
```

**Apply cv** (when cv arrives, whether before or after promote):

```
handle_apply_cv(slot)
  -> if parsed_model_name is empty: return
  -> log INFO "slot N: applying cv data — model='HERO7 Black' fw='HD7.01.01.90.00'"
  -> mapped = gopro_model_from_name(parsed_model_name)
  -> camera_manager_set_model(slot, mapped)
  -> camera_manager_save_slot(slot)
  /* slot's name field is intentionally left blank — see §17.2.5 */
  -> identify_attempted = true   /* keepalive_tick stops re-sending cv */
  -> if pair_attempt_addr_matches(ctx->mac):
        pair_attempt_set_model(mapped)
        pair_attempt_advance(SUCCESS)   /* no-op if promote already advanced */
```

If the camera never answers `cv` at all, the slot stays at `CAMERA_MODEL_GOPRO_HERO_LEGACY_RC` (the default seeded by `add_camera`). All UDP control still works in that state — only the resolved model enum and HTTP-datetime capability check are missing.

**Pair-attempt success semantics.** For the web-UI Add flow, *any* UDP response from the camera proves it's a working RC GoPro, so the first promote = pair success. If `cv` is parsed first, the model is surfaced before SUCCESS. If `cv` arrives later (or never), the home-screen card picks up the upgraded model on its next refresh — by then the pair-progress modal is already closed. See §9.1 for the full state-machine wiring and the transport-aware cancel/timeout behaviour.

#### 17.5.1 Status-poll bootstrap

The status-poll handler iterates **all slots with `last_ip != 0`**, not only `wifi_ready` ones. New slots can't promote without their first `st` response, so they need to be polled before they're ready.

### 17.6 WoL and Reconnect Logic

```
handle_keepalive_tick(slot)
  -> rc_send_keepalive(ctx->last_ip)        /* _GPHD_ ASCII */
  -> if !ctx->identify_attempted:
        rc_send_cv(ctx->last_ip)            /* retry until cv lands */
  -> if last_response_tick != 0
     and (now - last_response_tick) > 10 s:
        if wol_retry_timer not armed: arm it (2 s)
     else if wol_retry_timer armed:
        disarm it

handle_wol_retry(slot)
  -> rc_send_wol(ctx->last_ip, ctx->mac)
  -> rc_send_keepalive(ctx->last_ip)

(no separate "on_keepalive_ack" handler — the RX task updates
 last_response_tick on any datagram from the slot's IP, and posts
 CMD_PROMOTE if the slot is not yet ready)
```

**WoL packet**: 102-byte magic packet: `FF FF FF FF FF FF` + target MAC × 16. Broadcast to `255.255.255.255:9`. Sent in a burst of 5.

**WoL retry termination**: The WoL retry timer continues until either a keepalive ACK or `st` response arrives (disarms the timer) or the camera disassociates from the SoftAP. There is intentionally no max-retry counter — the SoftAP's 60 s inactive-time (§11.2) evicts the silent station, which disarms the timer in the disconnect handler. WoL retry is naturally bounded to ≤ 60 s.

Liveness is now measured against any received UDP datagram (ACK *or* `st` response), not specifically a `0x5F`-prefixed ACK.

### 17.7 Shutter Commands

Handled by the high-priority shutter task. The driver supplies two pairs of vtable entries; `camera_manager` picks one based on the call site (see §13.3a).

**Broadcast path** (`set_desired_recording_all` / CAN `0x600` / web UI Start All):

```
drv_start_recording_all() / drv_stop_recording_all()
  -> xQueueSend(s_shutter_queue, { start, 0xFFFFFFFFu, RC_SHUTTER_BROADCAST_REPEAT })

rc_shutter_task
  -> for i in 0..repeat: rc_send_sh(0xFFFFFFFFu, start)   /* SO_BROADCAST set on socket */
```

One queued command → 3 UDP `sendto` calls to `255.255.255.255:8484`. `SO_BROADCAST` is enabled on `s_udp_sock` once at init (also required for WoL). The 3× repeat compensates for unacknowledged 802.11 broadcast frames; the burst completes in well under 1 ms at 54 Mbps.

**Unicast path** (`set_desired_recording_slot` / mismatch poll):

```
drv_start_recording(ctx) / drv_stop_recording(ctx)
  -> xQueueSend(s_shutter_queue, { start, ctx->last_ip, 1 })

rc_shutter_task
  -> rc_send_sh(ctx->last_ip, start)   /* single targeted UDP datagram */
```

One queued command → 1 UDP `sendto` to the slot's `last_ip:8484`. The 802.11 link layer retries on its own. The single-camera unicast path exists deliberately: a duplicate Start to a Hero4 already recording has been observed to flip it back off, so single-slot commands stay targeted.

Was ~50 ms HTTP per camera before this protocol revision; now < 1 ms total whether broadcasting to four cameras or unicasting to one.

### 17.8 Periodic Work

**Keepalive** (3 s `esp_timer`, per slot): UDP `_GPHD_:0:0:2:0.000000\n` to `ctx->last_ip`. Fire-and-forget.

**Status poll** (5 s `esp_timer`, global): UDP `st` packet to every slot with `last_ip != 0` (not just `wifi_ready` ones — see §17.5.1). Response handled async by the RX task.

**UDP RX loop** (dedicated task on the shared 8383 socket): dispatches by content per §17.4.1. Recognised opcodes: `0x5F`-first-byte (keepalive ACK), `'s','t'` (status), `'S','H'` (shutter echo), `'c','v'` (camera-version reply). Any other payload is logged at VERBOSE and discarded.

**`cv` retry on every keepalive tick:** until `identify_attempted` flips true (which happens when `apply_cv` lands), each keepalive tick also re-sends `cv`. Some cameras drop the first `cv` (arrives before the RC pairing has fully settled); the retry keeps nudging once per 3 s window until the camera answers.

There is no periodic HTTP traffic. Date/time runs once on promote (Hero4 only) and again on `gopro_wifi_rc_sync_time_all()` after a UTC source.

### 17.9 Disconnect

On `CMD_STATION_DISCONNECTED(mac)`:

1. Disarm `keepalive_timer` and `wol_retry_timer`
2. `ctx->wifi_ready = false`
3. `ctx->recording_status = CAMERA_RECORDING_UNKNOWN`
4. `camera_manager_on_wifi_disconnected(slot)`

### 17.10 NVS Ownership

`gopro_wifi_rc` owns no NVS namespace. All persistence goes through `camera_manager` — MAC, name, model, and `last_ip` all live in `cam_N/camera`.

### 17.11 Public API

```c
void gopro_wifi_rc_init(void);

void gopro_wifi_rc_on_station_associated(const uint8_t mac[6]);
void gopro_wifi_rc_on_station_dhcp(const uint8_t mac[6], uint32_t ip);
void gopro_wifi_rc_on_station_disassociated(const uint8_t mac[6]);

void gopro_wifi_rc_add_camera(const uint8_t mac[6], uint32_t ip);
void gopro_wifi_rc_remove_camera(int slot);
bool gopro_wifi_rc_is_managed_slot(int slot);
bool gopro_wifi_rc_is_managed_mac(const uint8_t mac[6]);

void gopro_wifi_rc_sync_time_all(void);
```

### 17.12 Source File Layout

| File | Contents |
|---|---|
| `include/gopro_wifi_rc_spec.h` | Port numbers, `_GPHD_` payload string, opcode byte templates (`RC_PKT_ST`, `RC_PKT_SH_START`, `RC_PKT_SH_STOP`, `RC_PKT_CV`), `st`-response and `cv`-response field offsets, timing constants, HTTP path string (date/time only) |
| `driver.c` | Init, vtable registration, work-queue dispatch (`CMD_PROMOTE`, `CMD_APPLY_CV`, `CMD_KEEPALIVE_TICK`, `CMD_WOL_RETRY`, `CMD_STATUS_POLL_ALL`, `CMD_SYNC_TIME_ALL`, station events), global status-poll timer; `gopro_wifi_rc_add_camera` primes the camera with keepalive + `st` + `cv` and calls `pair_attempt_fail(SLOTS_FULL, ...)` if no free slot is available for the in-flight Add attempt |
| `connection.c` | Station lifecycle handlers; `rc_handle_promote` (cv-aware; calls `pair_attempt_advance(SUCCESS)` when the in-flight Add attempt's MAC matches); `rc_handle_apply_cv` (sets the resolved model on the pair_attempt info and advances to SUCCESS); `rc_handle_keepalive_tick` (which also re-sends `cv` until identify_attempted); `rc_handle_wol_retry`; per-slot timer arm/disarm |
| `command.c` | Shutter task (UDP `SH`); `rc_send_datetime` (HTTP `setup/date_time`, gated on `gopro_model_supports_http_datetime`); `rc_http_get` retained as a minimal helper for date/time only |
| `status.c` | Status-poll handler (sends UDP `st`); `rc_parse_st_response()` (binary b13/b14/b15); `rc_parse_cv_response()` (length-prefixed firmware + model name → ctx fields, posts CMD_APPLY_CV) |
| `udp.c` | Single bound socket on 8383; `rc_send_keepalive`, `rc_send_st`, `rc_send_sh`, `rc_send_cv`, `rc_send_wol`; RX task with 0x5F / `st` / `SH` / `cv` dispatch |
| `diagnostic.c` | `gopro_wifi_rc_diagnose()` — temporary dev-only network probe (ICMP, TCP port scan, UDP opcode probes).  Compiled in but not currently dispatched; kept for future hardware investigations (see commit history: 2026-05 cv-discovery diagnostic). |
| `settings.c` | (unchanged placeholder) |
| `gopro_model.c` | `gopro_model_from_name()` lookup table (lives in the gopro umbrella component, used by `rc_handle_apply_cv`) |

Removed (history): 4 KB JSON body buffer, JSON status-field constants, `RC_HTTP_PATH_STATUS`, `RC_HTTP_PATH_SHUTTER_*`, `RC_HTTP_PATH_IDENTIFY`, `RC_HTTP_IDENTIFY_RESP_MAX`, probe retry constants, `parse_recording_status()` JSON parser, `rc_handle_http_identify()`, `rc_parse_identify_json()`, `RC_CMD_PROBE`, `RC_CMD_HTTP_IDENTIFY`.

### 17.13 Known TODOs

| Where | Issue |
|---|---|
| `gopro_model.c` | `gopro_model_from_name()` lookup table is keyed off the model_name string in the `cv` UDP response.  Verified against real hardware: HERO4 Black, HERO4 Silver, HERO7 Black.  HERO4 Session / HERO5 Black/Session / HERO6 Black / HERO 2018 are seeded from public docs but the actual reported strings haven't been observed yet; if the camera reports a different string the lookup falls through to LEGACY_RC and `rc_parse_cv_response` logs the unexpected value at INFO so the table can be corrected. |
| `gopro_model.h` `supports_http_datetime` | Currently allow-listed for Hero4 Black/Silver only.  Hero5/6 with HTTP-on-STA in Smart-Remote mode would also benefit from datetime sync — verify per-model and add as confirmed.  Hero7 confirmed silent on TCP port 80 in this mode (probed 2026-05); stays excluded. |
| `command.c` `rc_send_datetime` | URL-encoded hex-byte format works on Hero4 (Lua reference).  Currently sends UTC; `can_manager`'s tz offset must be applied before encoding so the camera's local-time fields are correct. |
| `command.c` `rc_send_datetime` | Hero3-class and Hero5/6/7 have no working date/time path — the camera does not run an HTTP server on its STA interface, and the binary `setTM` UDP variant was flagged as broken in the ESP8266 reference.  Currently a documented limitation; UI may want to surface "camera clock not synced" to the user. |
| Future opcodes | `YY` (battery / mode get / clock get), `CM` (mode change), `PW` (power off) are documented in §17.2 but not implemented. |


---

## 18. Reconnect Logic

### 18.1 BLE-Control Cameras — BLE Passive Scan

Reconnection is driven by a passive BLE scan managed by `ble_core` and directed by `camera_manager`.

**Scan lifecycle:**

- On boot, `camera_manager` calls `ble_core_start_reconnect_scan(addrs, count)` with the MAC addresses of all configured-but-disconnected BLE-control cameras (i.e. configured slots whose driver was registered with `requires_ble == true`).
- When a target is found, `ble_core` fires a callback. `camera_manager` initiates a normal connection.
- If a connection succeeds, `camera_manager` reassesses: if all configured cameras are now connected, it calls `ble_core_stop_scan()`. Otherwise the scan continues.
- If a connection attempt does not complete within **20 seconds**, the camera is returned to `CAM_BLE_NONE` and the scan resumes automatically.
- On mid-session disconnect (camera drops an established connection), `camera_manager` waits **2 seconds** before calling `ble_core_start_reconnect_scan()`.

**Serial reconnection:** BLE allows only one connection attempt at a time (section 12.4). With multiple BLE-control cameras configured, reconnection is serialised: camera A connects and completes its readiness sequence, then the background scan resumes for camera B. In the worst case — all cameras offline simultaneously — the last camera begins reconnecting only after all preceding cameras have finished. In this application (race vehicle, cameras powered together) this is acceptable: once the car is moving, cameras are expected to stay connected. Serial reconnection on boot-up is a one-time cost.

**Whitelist:** The scan should use the NimBLE whitelist (`BLE_HCI_SCAN_FILT_USE_WL`) to drop non-target advertisements at the controller level. If the whitelist proves unreliable, the fallback is software filtering inside the callback. Both approaches produce identical behaviour from `camera_manager`'s perspective.

**State during a connection attempt:** While a connection is in progress, `ble_status` is set to `CAM_BLE_CONNECTING`. If the attempt times out or the GAP event returns an error, `ble_status` returns to `CAM_BLE_NONE` and the scan resumes.

### 18.2 RC-Emulation Cameras — Fully Passive

No active scanning. The SoftAP handles discovery by virtue of the camera connecting to it. `gopro_wifi_rc` ignores all unknown MACs — only cameras already registered in `camera_manager` receive any autonomous action.

**Station associates with a DHCP lease:** The DHCP request proves the camera is awake. `gopro_wifi_rc` records `last_ip`, persists it via `camera_manager_save_slot()`, primes the camera with a keepalive + `st` + `cv` UDP burst, and arms the 3 s keepalive timer. On the first received UDP datagram (any opcode) the slot transitions to `WIFI_CAM_READY`; a `cv` response settles the model asynchronously when the camera answers (see §17.5 / §17.2.5). The slot's name field is left blank — there is no known WiFi RC protocol path to retrieve the user-set camera name.

**Station associates without a DHCP lease (cached IP):** The camera may be asleep or reusing a static IP. `gopro_wifi_rc` checks `last_ip` from the camera's NVS record:
- `last_ip` known: send WoL magic packet x5, prime with a keepalive, arm the 3 s keepalive timer. The first received UDP datagram drives `CMD_PROMOTE` -> `WIFI_CAM_READY`.
- `last_ip` unknown (never connected before): log warning, take no further action. If the camera wakes and requests a DHCP lease, `CMD_STATION_DHCP` fires and the normal flow resumes.

**Keepalive silence > 10 s (WoL retry):** If no UDP datagram (keepalive ACK, `st`, `SH`, or `cv` reply) arrives within 10 s of the timer being armed, a 2 s WoL-retry timer fires repeatedly — sending WoL x5 followed by a keepalive each cycle — until traffic resumes. On entry to the silence state the driver also calls `camera_manager_on_camera_unresponsive()`, which demotes `wifi_status` from `WIFI_CAM_READY` to `WIFI_CAM_PROBING` and stops the mismatch poll. The slot remains `wifi_associated == true` (it has not left the AP), so the http_server reports its status as `"connecting"` rather than `"disconnected"`. The next received UDP datagram drives `CMD_PROMOTE` again, which calls `camera_manager_on_camera_ready()` and restores `WIFI_CAM_READY`.

**Station disassociates:** Slot returns to `WIFI_CAM_NONE` immediately. Keepalive and WoL-retry timers are disarmed. No further traffic is sent until the camera re-associates.

---

## 19. Web UI and Data Partition

### 19.1 Storage

The project's existing partition table already reserves a 3MB LittleFS partition (`storage`). Web UI source files live in `web_ui/` at the project root; the LittleFS image (`build/storage.bin`) is generated automatically during `idf.py build` and flashed as part of `idf.py flash`.

Benefits:
- Web UI can be re-flashed independently from firmware (`idf.py storage-flash`).
- Browser can cache JS and CSS separately from the HTML shell.
- Pre-compressed assets reduce transfer size significantly on the SoftAP link.

### 19.2 File layout

Source files (checked in to `web_ui/`):
```
web_ui/
  index.html        HTML shell
  style.css         Stylesheet
  app.js            Application logic
  compress.py       Build helper — called by CMake to copy + gzip assets
```

On-device layout (`/www/` on LittleFS):
```
/www/
  index.html        Copied as-is
  style.css         Plain copy
  style.css.gz      Pre-compressed (gzip -9); served when Accept-Encoding: gzip present
  app.js            Plain copy
  app.js.gz         Pre-compressed; served when Accept-Encoding: gzip present
```

`compress.py` is invoked by a CMake custom target (`web_ui_stage`) defined in the root `CMakeLists.txt`. `littlefs_create_partition_image()` (from `joltwallet__littlefs`) then builds `build/storage.bin` from the staged output and wires it into `idf.py flash` via `FLASH_IN_PROJECT`.

### 19.3 Serving compressed assets

The HTTP server checks `Accept-Encoding: gzip` in the request headers (sent by every modern browser and iOS Safari). If present, it serves the `.gz` file with:

```
Content-Encoding: gzip
Content-Type: application/javascript   (or text/css)
Content-Length: <compressed size>
Cache-Control: no-cache
```

`Cache-Control: no-cache` is sent on every static-asset response (compressed and uncompressed). An earlier `max-age=3600` value caused browsers to keep cached old assets across LittleFS reflashes; `no-cache` forces revalidation on every page load so a freshly flashed web UI shows up without a manual hard-refresh.

This also resolves the observed iOS loading issues. iOS Safari requires an accurate `Content-Length` header and handles chunked transfer encoding poorly from embedded HTTP servers. Serving a known-size pre-compressed file with explicit headers satisfies both requirements.

### 19.4 Web UI source location

Web UI source files live in `web_ui/` at the project root (not inside any component). LittleFS image generation is wired into the root `CMakeLists.txt` so it runs as part of every build. `wifi_manager` and `http_server` have no build-time dependency on the web UI source.

---

## 20. http_server

The HTTP server is the topmost component in the dependency graph. It owns the `esp_httpd` instance, serves web assets from LittleFS, and implements all `/api/` endpoint handlers. It has no logic of its own beyond routing — all decisions are delegated to the component being called.

### 20.1 Responsibilities

| Responsibility | Notes |
|---|---|
| Serve web UI assets | `index.html`, `app.js.gz`, `style.css.gz` from LittleFS `/www/` |
| Handle all `/api/` endpoints | Route to the appropriate component; build JSON responses |
| No business logic | Handlers read arguments, call one component API, write response |

`http_server` depends on every other component. No other component depends on it.

### 20.2 Task Model

`esp_httpd` runs on its own internal task. API handlers are called directly on that task — no additional queue or dispatch task is needed. This is safe because read operations are non-blocking RAM reads, state-setting calls are flag writes, and BLE/WiFi RC operations already return immediately by posting to their own component queues internally.

`esp_httpd` stack size and max open sockets are no longer Kconfig symbols in IDF v6 and must be set programmatically in `http_server_init()`:

```c
httpd_config_t config = HTTPD_DEFAULT_CONFIG();
config.stack_size      = 8192;   /* default 4 KB is tight when building JSON for all camera slots */
config.max_open_sockets = 16;    /* matches V1; handles concurrent browser + background polls */
httpd_start(&server, &config);
```

### 20.3 Serving Web Assets

On `GET /` and `GET /index.html`: serve `/www/index.html` from LittleFS.

On `GET /app.js` and `GET /style.css`: check `Accept-Encoding` header. If `gzip` is present, serve the `.gz` file with explicit `Content-Encoding: gzip` and `Content-Length`. This satisfies iOS Safari's requirement for accurate content length on compressed responses (see section 19.3).

All asset responses include `Cache-Control: no-cache` (forces revalidation on every page load — see §19.3 for why this replaced the earlier `max-age=3600`).

### 20.4 API Handlers

All handlers follow the same structure: parse request -> call one component API -> write JSON response. The full endpoint contract lives in **[`web-ui.md`](web-ui.md)**, which is the canonical reference for the HTTP API contract. This section only maps endpoints to the component each handler calls into.

| Endpoint | Component called |
|---|---|
| `GET /api/logging-state` | `can_manager_get_logging_state()` |
| `GET /api/utc` | `can_manager_get_utc_ms()` |
| `GET /api/auto-control` | `camera_manager_get_auto_control()` |
| `POST /api/auto-control` | `camera_manager_set_auto_control()` |
| `GET /api/paired-cameras` | `camera_manager_get_all_slot_info()` |
| `POST /api/shutter` | `camera_manager_set_desired_recording_all()` or `_slot()` |
| `POST /api/remove-camera` | `camera_manager_remove_slot()` (section 20.5) |
| `POST /api/reorder-cameras` | `camera_manager_reorder_slots()` (section 20.6) |
| `GET /api/cameras` | `open_gopro_ble_get_discovered()` |
| `POST /api/scan` | `open_gopro_ble_start_discovery()` |
| `POST /api/scan-cancel` | `open_gopro_ble_stop_discovery()` |
| `POST /api/pair` | `pair_attempt_begin(addr, addr_type, PAIR_TRANSPORT_BLE)` -> `open_gopro_ble_connect_by_addr()` |
| `GET /api/pair/status` | `pair_attempt_get()` (shared by BLE and RC Add flows; response includes the `transport` field) |
| `POST /api/pair/cancel` | `pair_attempt_cancel()` (transport-aware cleanup — see §9.1) |
| `GET /api/rc/discovered` | `wifi_manager_get_connected_stations()` filtered by GoPro OUI allow-list (`GOPRO_RC_OUIS[]` in `api_rc.c`) and `gopro_wifi_rc_is_managed_mac()` |
| `POST /api/rc/add` | `pair_attempt_begin(addr, 0, PAIR_TRANSPORT_WIFI_RC)` -> `gopro_wifi_rc_add_camera()` |
| `GET /api/settings/timezone` | `can_manager_get_tz_offset_hours()` |
| `POST /api/settings/timezone` | `can_manager_set_tz_offset()` |
| `POST /api/settings/datetime` | `can_manager_set_manual_utc_ms()` -> fires UTC-acquired path. Rejected with `ESP_ERR_INVALID_STATE` only after a live source has already won this session; an NVS-restored anchor at boot does not block manual entry. |
| `POST /api/reboot` | `esp_restart()` |

### 20.5 Unified Camera Remove with Slot Compaction

`camera_manager_remove_slot(slot)` is the single entry point for all camera removal. After removing the target slot, all higher-indexed slots are compacted down by one position so that the slot array is always contiguous.

**Remove and compact sequence:**

1. Call `driver->teardown(ctx)` if non-NULL — allows the driver to stop timers and free per-slot resources.
2. Call `ble_core_remove_bond(mac)` for BLE-control cameras (BLE bond must be cleared so the camera does not auto-reconnect).
3. Erase `cam_N/camera` from NVS.
4. For each slot index i from the removed slot up to (MAX_SLOTS - 2):
   - Copy RAM slot i+1 -> RAM slot i (struct copy).
   - Call `driver->update_slot_index(ctx, i)` so the driver context's cached slot number reflects the new position.
   - Write `cam_i/camera` to NVS from the new RAM slot i content.
5. Zero the highest RAM slot (now a duplicate after the shift).
6. Erase `cam_(last)/camera` from NVS.

See §8 for the canonical `camera_driver_t` vtable signature, including `update_slot_index` and `on_wifi_disconnected`.

`open_gopro_ble` implements `teardown` (stop keepalive, status poll, and readiness timers; reset cached_status) and `update_slot_index`. `gopro_wifi_rc` implements `teardown` (delete keepalive and WoL retry timers) and `update_slot_index`.

**CAN `0x601` output changes immediately** after compaction. If the RaceCapture configuration maps CAN byte positions to camera identities, the operator must update RaceCapture to match after removing a camera.

### 20.6 Manual Slot Reordering

The web UI allows the operator to reorder cameras. Reordering requires all affected cameras to be disconnected first (the API returns an error if any slot in the reorder set is currently `WIFI_CAM_READY` or `CAM_BLE_CONNECTED`).

```c
esp_err_t camera_manager_reorder_slots(const int *new_order, int count);
```

```
POST /api/reorder-cameras
Body: { "order": [3, 1, 4, 2] }
  — order[0]=3 means "put the camera currently at position 3 into position 1"
  — Camera positions in the API are 1-based; camera_manager_reorder_slots()
    receives 0-based indices after API-boundary conversion in api_cameras.c
Response: 200 OK on success, 409 Conflict if any camera is currently connected
```

**Implementation:** `camera_manager_reorder_slots` performs the permutation in RAM, writes all affected `cam_N/camera` records to NVS in the new order, and calls `update_slot_index` on each driver context for affected slots.

**Note on RaceCapture:** reordering changes which CAN byte corresponds to which physical camera. The operator must update the RaceCapture channel mapping after reordering.

### 20.7 Source File Layout

| File | Contents |
|---|---|
| `driver.c` | `http_server_init()`, `esp_httpd` start, LittleFS mount, static asset handlers |
| `api_cameras.c` | `/api/paired-cameras`, `/api/shutter`, `/api/remove-camera`, `/api/reorder-cameras`, `/api/scan`, `/api/scan-cancel`, `/api/pair`, `/api/cameras` |
| `api_rc.c` | `/api/rc/discovered`, `/api/rc/add` |
| `api_settings.c` | `/api/settings/timezone`, `/api/settings/datetime` |
| `api_system.c` | `/api/logging-state`, `/api/utc`, `/api/auto-control`, `/api/reboot` |


---

## 21. Startup Sequence

### 21.1 Design Constraints

Two constraints shape the init order:

**Networking last.** `wifi_manager_init()` is the call that raises the SoftAP and makes it visible. An RC-emulation camera could associate the moment the AP is visible. If `gopro_wifi_rc`'s station callbacks are not yet registered when that happens, the event is lost. Similarly, `ble_core_init()` starts the NimBLE host task; the `on_sync` callback fires asynchronously and immediately begins scanning. All BLE and WiFi RC callbacks must be wired before their respective stacks are started.

**Drivers before slots.** `camera_manager_init()` loads slot records from NVS but cannot assign drivers until the drivers have registered themselves — see section 21.4 for the registration mechanism. By the time all driver inits are complete, all loaded slots whose model is recognised by some registered driver have a live driver pointer and context. Slots whose model matches no driver are left in an unconfigured state and logged at WARN.

### 21.2 Init Order

```
app_main()
  |
  +- nvs_flash_init()
  |    NVS is the prerequisite for every component that reads or writes
  |    persistent state. Must be first.
  |
  +- camera_manager_init()
  |    Loads all cam_N/camera records from NVS into RAM slots.
  |    All slots start with ble_status=CAM_BLE_NONE, wifi_status=WIFI_CAM_NONE.
  |    No drivers assigned yet.
  |
  +- gopro_wifi_rc_init()
  |    Calls camera_manager_register_driver(&s_rc_driver,
  |                                          gopro_model_uses_rc_emulation,
  |                                          gopro_wifi_rc_create_ctx,
  |                                          /*requires_ble=*/false).
  |    Starts shutter task + work task. Opens UDP socket.
  |    Starts global keepalive and status-poll timers
  |    (timers iterate wifi_ready slots — none yet, so they fire and return).
  |
  +- open_gopro_ble_init()
  |    Registers its callback struct with ble_core (stores pointers; ble_core
  |    not yet started so no callbacks fire here).
  |    Calls camera_manager_register_driver(&s_ble_driver,
  |                                          gopro_model_uses_ble_control,
  |                                          ctx_create,
  |                                          /*requires_ble=*/true).
  |    camera_manager iterates loaded slots and assigns this driver to all
  |    matching BLE-control slots, calling ctx_create() to claim per-slot state.
  |    (Bond purge against the camera_manager known-MAC list is a future TODO —
  |    requires persisting BLE address-type alongside the MAC.)
  |
  +- ble_core_init()
  |    Starts the NimBLE host task.
  |    on_sync fires asynchronously once NimBLE is ready:
  |      -> start_scan_if_needed() — begins passive scan if any BLE-control
  |        slots are configured but disconnected (see §18.1).
  |    No explicit call from main() required after this point for BLE.
  |
  +- can_manager_init(&s_can_callbacks)
  |    Loads tz_offset from NVS.
  |    Starts TWAI driver and RX task.
  |    Registers s_can_callbacks (defined in main.c — see section 21.3).
  |
  +- wifi_manager_init(&s_wifi_callbacks)
  |    Registers s_wifi_callbacks (defined in main.c — see section 21.3).
  |    Spoofs SoftAP MAC to d8:96:85:XX:XX:XX.
  |    Raises SoftAP — SSID now visible to cameras.
  |    Starts DHCP server.
  |    *** From this point RC-emulation cameras may associate. ***
  |
  +- http_server_init()
       Mounts LittleFS partition (/www/).
       Starts esp_httpd instance.
       Registers all URI handlers.
       Web UI now accessible.
```

### 21.3 Callback Wiring

All cross-component wiring lives in `main.c`. Components register callbacks through their init functions; `main.c` defines the handler bodies that call into the appropriate components.

#### CAN callbacks (`s_can_callbacks`)

```c
static void on_logging_state_changed(can_logging_state_t state)
{
    /* Fires on every received 0x600 frame (section 14.2) — idempotent on the
     * camera_manager side since set_desired_recording_all() is itself idempotent. */
    if (state == LOGGING_STATE_LOGGING || state == LOGGING_STATE_NOT_LOGGING) {
        bool recording = (state == LOGGING_STATE_LOGGING);
        camera_manager_set_desired_recording_all(recording);
    }
    /* LOGGING_STATE_UNKNOWN is reflected in the web UI but does not modify
     * desired_recording — see section 13.2. */
}

static void on_utc_acquired(void)
{
    open_gopro_ble_sync_time_all();
    gopro_wifi_rc_sync_time_all();
}

static const can_callbacks_t s_can_callbacks = {
    .on_logging_state_changed = on_logging_state_changed,
    .on_utc_acquired          = on_utc_acquired,
};
```

`on_utc_acquired` fires exactly once per boot session — on the first valid GPS timestamp from RaceCapture **or** the first successful manual web-UI set, whichever comes first. NVS-restored UTC at boot does not fire it. Both camera types are notified simultaneously. `open_gopro_ble_sync_time_all()` also clears any slot's `datetime_pending_utc` flag and pushes `SetDateTime` over BLE (see §15.5).

#### WiFi callbacks (`s_wifi_callbacks`)

```c
static void on_station_associated(const uint8_t mac[6])
{
    camera_manager_on_station_associated(mac);
    gopro_wifi_rc_on_station_associated(mac);
    /* BLE-control cameras never associate to the SoftAP. */
}

static void on_station_dhcp(const uint8_t mac[6], uint32_t ip)
{
    camera_manager_on_station_ip(mac, ip);   /* updates last_ip for any matching slot */
    gopro_wifi_rc_on_station_dhcp(mac, ip);
}

static void on_station_disassociated(const uint8_t mac[6])
{
    camera_manager_on_station_disassociated(mac);
    gopro_wifi_rc_on_station_disassociated(mac);
}

static const wifi_callbacks_t s_wifi_callbacks = {
    .on_station_associated    = on_station_associated,
    .on_station_dhcp          = on_station_dhcp,
    .on_station_disassociated = on_station_disassociated,
};
```

`on_station_disassociated` is only relevant to `gopro_wifi_rc` (and the camera_manager's `wifi_associated` bookkeeping). BLE-control cameras don't generate SoftAP events; their lifecycle is driven entirely by BLE link state.

#### BLE callbacks

Registered by `open_gopro_ble_init()` directly via `ble_core_register_callbacks()` — no wiring required in `main.c`. `open_gopro_ble` owns the full BLE callback set.

### 21.4 Driver Registration Mechanism

`camera_manager` does not depend on any driver component. Drivers know about `camera_manager`, not the other way around — that is the whole point of the vtable in section 13.5. At boot, `camera_manager` has no way to construct a driver itself; the drivers must announce themselves.

**The registration call:**

```c
typedef bool   (*camera_model_match_fn)(camera_model_t model);
typedef void  *(*camera_ctx_create_fn)(int slot);

esp_err_t camera_manager_register_driver(
    const camera_driver_t *driver,
    camera_model_match_fn   matches,
    camera_ctx_create_fn    create_ctx,
    bool                    requires_ble);
```

Each driver component calls this from its `_init()` function. For example:

```c
/* in open_gopro_ble_init() */
camera_manager_register_driver(&s_ble_driver,
                                gopro_model_uses_ble_control,
                                ctx_create,
                                /*requires_ble=*/true);
```

The `requires_ble` flag controls `camera_manager_has_disconnected_cameras()` (§12.9): a slot owned by a `requires_ble=true` driver counts as "disconnected" for the BLE background scan gate when its BLE link is down. RC-emulation slots register with `requires_ble=false` so they don't keep the BLE scanner running permanently.

**What `camera_manager` does on registration:**

1. Stores the registration in a small fixed-size table.
2. Iterates all currently-loaded slots. For each slot whose `model` returns `true` from the registered `matches()`:
   - Calls `create_ctx(slot)` and stores the returned pointer in `slot->driver_ctx`.
   - Stores the `driver` pointer in `slot->driver`.
3. Logs a WARN for any slot that ends up with no matching driver after all expected drivers have registered.

**Why predicate functions instead of a `camera_driver_type_t` enum?** A predicate keeps GoPro-specific knowledge out of `camera_manager`. With an enum, `camera_manager` would need to know "model X corresponds to driver type Y" — which is exactly the GoPro-specific decision the architecture is trying to keep out. With a predicate, each driver answers "is this slot mine?" using its own logic, and `camera_manager` never has to map model values.

**Boot ordering.** All driver `_init()` calls in section 21.2 happen before `wifi_manager_init()` and `ble_core_init()` raise their stacks. Driver assignment is guaranteed complete before any driver method can be called.

### 21.5 Post-Boot Steady State

After `http_server_init()` returns, the system is fully operational:

- NimBLE is scanning passively for configured BLE-control cameras (if any are configured but disconnected)
- SoftAP is visible; RC-emulation cameras (Hero 4) can join. BLE-control cameras (Hero 9+) do not associate to the SoftAP.
- CAN bus is listening for `0x600` logging commands and `0x602` UTC frames
- `0x601` camera-state frames are being transmitted at 5 Hz regardless of camera connection state
- Web UI is accessible at `http://10.71.79.1/`

No component requires any further call from `main()`. All subsequent activity is event-driven.

---

## 22. Logging Strategy

ESP-IDF's `esp_log` macros are the baseline. The conventions below are not enforced by tooling — they are what every component in V2 should follow.

### 22.1 Tags

```c
static const char *TAG = "camera_manager";
```

One TAG per `.c` file (or one per logical area within a component). Tag string equals component name, lowercase, no spaces. Sub-components (e.g. `gopro_ble_pairing`) can use a more specific tag if log volume warrants it.

### 22.2 Per-slot prefix

Every per-slot log line includes `slot %d:` early in the format string, for grep-ability:

```c
ESP_LOGI(TAG, "slot %d: camera ready", slot);
ESP_LOGW(TAG, "slot %d: shutter timeout", slot);
```

### 22.3 Levels

| Macro | Use for |
|---|---|
| `ESP_LOGE` | Unrecoverable for this operation: NVS write failed, vtable null deref avoided, OOM, unrecoverable protocol error |
| `ESP_LOGW` | Transient error, retry in progress, unexpected-but-handled: HTTP non-200, BLE timeout, missing model, single 401 |
| `ESP_LOGI` | Milestone state transitions: slot configured, BLE encrypted, hardware ready, camera ready, station joined, scan started/stopped |
| `ESP_LOGD` | Verbose state-machine traces: each scan event, each poll fire, each timer arm, individual GATT writes |
| `ESP_LOGV` | Packet-level dumps. Off by default. Per-component opt-in when investigating something specific |

### 22.4 Spam control

Anywhere a condition can fire repeatedly (mismatch correction loop, WoL retry, repeated 401s short of the threshold), log only on transition. Pattern:

```c
if (state != ctx->last_logged_state) {
    ESP_LOGW(TAG, "slot %d: transitioned to %s", slot, state_name(state));
    ctx->last_logged_state = state;
}
```

Do not suppress logs with timers — suppress by transition. A log line that fires once per state change is debuggable; a line that fires once every 5 s per slot drowns the console.

### 22.5 Runtime tuning

Per-component log levels can be set at runtime in `app_main()` before component init:

```c
esp_log_level_set("*",                ESP_LOG_INFO);
esp_log_level_set("open_gopro_ble",    ESP_LOG_DEBUG);
esp_log_level_set("ble_core",          ESP_LOG_DEBUG);
```

This survives without rebuild — useful in the field.

### 22.6 Build-time configuration

`CONFIG_LOG_DEFAULT_LEVEL` in sdkconfig sets the compile-time maximum. `CONFIG_LOG_MAXIMUM_LEVEL` sets the absolute ceiling for `esp_log_level_set()` to override. Set the maximum to `VERBOSE` so runtime tuning is unconstrained, and the default to `INFO` so a release build is quiet.

---

## 23. Test Strategy

V2 testing is **host-side unit tests only**. Hardware testing is performed manually using a real ESP32, real cameras, and a real CAN bus — covered by personal experience and not automated.

This split is deliberate: on-device automated testing for an embedded control system with BLE+WiFi+CAN is high-effort to set up and produces flaky results. The host-side tests are cheap to run and catch the class of bugs that table-driven logic is most prone to.

### 23.1 Host-side unit tests with Unity

ESP-IDF ships with **Unity** (`unity/`) — a small C testing framework. Pure-logic source files that don't include ESP-IDF headers can be compiled and run on the development host using a tiny CMake shim, completely independent of the ESP32 build.

**Targets that benefit most:**

- **`gopro/gopro_model.h` capability helpers.** All pure functions, no dependencies. Table-driven Unity tests covering every defined `camera_model_t` value plus boundary cases (CAMERA_MODEL_UNKNOWN, values just outside the GoPro ranges, hypothetical 1000+ multi-manufacturer values).
- **The mismatch-correction state machine (section 13.4)** — if extracted from the timer callback into a pure step function:

  ```c
  typedef enum {
      MISMATCH_ACTION_NONE,
      MISMATCH_ACTION_START,
      MISMATCH_ACTION_STOP,
  } mismatch_action_t;

  mismatch_action_t mismatch_step(bool desired_recording,
                                   camera_recording_status_t actual,
                                   bool grace_period_active);
  ```

  Then exhaustively unit-test the truth table: 2 x 3 x 2 = 12 input combinations, each with a known-correct output.
- **Binary RC response parsers** — `rc_parse_st_response()` (20-byte status reply, b13/b14/b15 → `recording_status`) and `rc_parse_cv_response()` (length-prefixed firmware + model name).  Feed in canned byte arrays captured from real cameras (e.g. the documented Hero7 cv payload in §17.2.5), assert parsed `recording_status` / model_name / firmware values.
- **CAN frame parsers** — `0x600`, `0x601`, `0x602` — separated from the TWAI driver so the parser is a pure `parse(uint8_t *bytes, size_t len) -> can_frame_t`. Cover bad payloads, short payloads, year-out-of-range UTC.

### 23.2 Build setup

A subdirectory `tests/host/` with its own `CMakeLists.txt` that compiles only the pure-logic files plus Unity, builds an executable on the host, and runs it via CTest. Files that include `esp_log.h`, `freertos/FreeRTOS.h`, `esp_http_client.h`, `nimble/...`, etc. are out of scope.

The convention to make this work: keep pure-logic functions in files that intentionally avoid ESP-IDF includes — `mismatch.c`, `gopro_model.c`, `can_parse.c` — and wire them up from the platform-aware files that do include ESP-IDF headers.

### 23.3 Recommended starting point

Pick **one** target — `gopro_model.h` capability helpers is the easiest — and get the host-side build running with one passing test. Once that workflow is proven (CMake + CTest + Unity all wired up), expand to the mismatch state machine and JSON parsers. The goal is having the workflow in place; coverage grows organically from there.

### 23.4 Out of scope (covered by manual testing)

The following are explicitly NOT in the unit test plan — they are validated manually on real hardware:

- Multi-camera RF behaviour and BLE/WiFi coexistence under load.
- Camera-side state transitions (record/stop, sleep/wake).
- WoL recovery from real battery-dead conditions.
- iOS / Safari rendering of the web UI.
- CAN bus integration with the RaceCapture device.
- BLE control end-to-end with a real GoPro (Hero 9+): pairing, readiness sequence, shutter, status poll, datetime.
- RC-emulation flow with a real Hero 4 in WiFi RC mode.

---

## 24. Open Items

The following will be designed in subsequent sessions:
- **Live telemetry:** Battery percentage, storage remaining, and other camera-reported values. Will be polled via the same BLE Query channel mechanism as the recording-status poll (`GetStatusValue` with additional status IDs), cached in the slot, and added to `camera_slot_info_t`.
- **Pre-Hero 9 BLE control:** Hero 5 and Hero 7 are reportedly functional over BLE despite not being officially supported by Open GoPro. Verify on hardware and add to `gopro_model_uses_ble_control()` if confirmed.
- **Additional `gopro_model.h` capability helpers:** Further behavioral differences (BLE feature support per generation, status poll intervals, etc.) documented as cameras are tested.

### 24.1 Multi-manufacturer hooks (deferred)

Adding a second camera manufacturer is not designed in this revision. The intent is to keep V2 from making decisions that would later be expensive to undo, without prematurely building hooks we don't yet understand. Concretely:

- **`camera_manager` is the boundary.** It already takes drivers via `register_driver()` with predicate functions over `camera_model_t`. A new manufacturer just registers their own driver(s) with their own model-range predicate. No `camera_manager` change.
- **`camera_model_t` reserves 1000+ for non-GoPro values.** New manufacturer ranges go there. GoPro capability helpers in `gopro/gopro_model.h` already gate on the GoPro ranges (section 5.2), so they will not misclassify.
- **`ble_core` is manufacturer-agnostic.** It exposes `on_disc` to whoever registers — currently `open_gopro_ble`. When a second BLE-using manufacturer is added, the routing question (which BLE driver handles a given advertisement) becomes real. **Do not pre-build this.** Today there is one consumer of `on_disc`, and that consumer applies its own UUID filter. When a second manufacturer arrives, the design choice becomes: either add a registration list to `ble_core` (each driver registers a UUID filter, `ble_core` dispatches), or have a thin `ble_dispatch` shim above `ble_core`. Either is straightforward to retrofit. The trap to avoid is *guessing* the routing API now — get it wrong and we'll be stuck with awkward callbacks.
- **`wifi_manager` is manufacturer-agnostic.** The station table is keyed by MAC; each driver looks up its own slots. A second manufacturer just adds another driver listening on the same callback set.
- **`http_server` will need to know.** The pairing flow (`/api/scan`, `/api/pair`) is currently GoPro-specific. When a second manufacturer arrives, this likely splits into per-manufacturer endpoints — the web UI controls the dispatch by which endpoint it calls. No need to design this until the second manufacturer is real.

The corner-paint risk is small as long as we keep the `gopro/` subtree clearly bounded and don't leak GoPro assumptions into `camera_manager`, `ble_core`, `wifi_manager`, or `http_server`.
