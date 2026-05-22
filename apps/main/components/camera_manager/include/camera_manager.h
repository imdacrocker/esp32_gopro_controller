#pragma once

#include "camera_types.h"
#include "esp_err.h"
#include "host/ble_hs.h"   /* ble_addr_t, BLE_HS_CONN_HANDLE_NONE */

#define CAMERA_MAX_SLOTS 4

/* ---- Driver vtable (§8 / §13.5) ---- */
typedef struct camera_driver camera_driver_t;
struct camera_driver {
    /* Per-slot recording control.  Always non-NULL.  Used by the mismatch
     * poll, set_desired_recording_slot, and (for non-broadcast drivers)
     * set_desired_recording_all. */
    esp_err_t                  (*start_recording)(void *ctx);
    esp_err_t                  (*stop_recording)(void *ctx);
    /* Non-blocking cache read — safe from any context (§8) */
    camera_recording_status_t  (*get_recording_status)(void *ctx);
    /* nullable — called by camera_manager_remove_slot() (§20.5) */
    void                       (*teardown)(void *ctx);
    /* nullable — notifies driver of new slot index after compaction (§20.5) */
    void                       (*update_slot_index)(void *ctx, int new_slot);
    /*
     * nullable — called from camera_manager_on_wifi_disconnected() when a
     * SoftAP-using camera leaves the AP.  Driver should stop any in-flight
     * network work.  Must not block.
     */
    void                       (*on_wifi_disconnected)(void *ctx);

    /* If true, set_desired_recording_all() calls *_recording_all() ONCE per
     * wave instead of per-slot start/stop.  Subsequent slots using the same
     * driver have their intent + grace period updated but skip dispatch.
     * Per-slot calls (set_desired_recording_slot, mismatch poll) always go
     * through start_recording/stop_recording regardless of this flag. */
    bool                        broadcasts_to_all;
    esp_err_t                  (*start_recording_all)(void);
    esp_err_t                  (*stop_recording_all)(void);
};

/* ---- Public slot info struct (§9) ---- */
typedef struct {
    int               index;
    char              name[32];
    camera_model_t    model;
    uint8_t           mac[6];
    bool              is_configured;
    cam_ble_status_t  ble_status;
    wifi_cam_status_t wifi_status;
    /* Driver reports ACTIVE.  Gated on wifi_status==READY internally so the
     * value only goes true when the camera is fully ready, regardless of
     * transport (BLE drivers also flip wifi_status to READY at the end of
     * their readiness sequence). */
    bool              is_recording;
    desired_recording_t desired_recording;
    uint32_t          ip_addr;             /* 0 when not connected */
    /* True after the camera has completed its readiness sequence at least
     * once (or, for RC-emulation cameras, immediately after registration).
     * Persisted in NVS.  Used by the web UI to distinguish "Pairing" (first-
     * time setup, false) from "Connecting" (subsequent reconnect, true). */
    bool              first_pair_complete;
    /* RC-emulation only: true between a SoftAP station-join event and its
     * matching station-disassociation event.  Used by the http_server to
     * distinguish "silent but still on the AP" (→ "connecting") from "left
     * the AP entirely" (→ "disconnected") for RC slots whose wifi_status
     * has been demoted from READY by the keepalive silence watchdog. */
    bool              wifi_associated;
} camera_slot_info_t;

/* ---- Pair-attempt info exposed via /api/pair/status ---- */
typedef struct {
    pair_attempt_state_t      state;
    pair_attempt_transport_t  transport;
    uint8_t                   addr[6];
    uint8_t                   addr_type;     /* BLE only — 0 for RC */
    camera_model_t            model;         /* CAMERA_MODEL_UNKNOWN until known */
    pair_attempt_error_t      error_code;
    char                      error_message[64];
} pair_attempt_info_t;

/* ---- CAN camera state (§14.2) ---- */
typedef enum {
    CAMERA_CAN_STATE_UNDEFINED    = 0,
    CAMERA_CAN_STATE_DISCONNECTED = 1,
    CAMERA_CAN_STATE_IDLE         = 2,
    CAMERA_CAN_STATE_RECORDING    = 3,
} camera_can_state_t;

/* ---- Driver registration types (§21.4) ---- */
typedef bool   (*camera_model_match_fn)(camera_model_t model);
typedef void  *(*camera_ctx_create_fn)(int slot);

/* ==========================================================================
 * Public API
 * ========================================================================== */

/*
 * Load all cam_N/camera NVS records into RAM.  All statuses start at NONE.
 * Must be called before any other camera_manager function.
 */
void camera_manager_init(void);

/*
 * Register a driver.  camera_manager immediately assigns it to all already-
 * loaded slots whose model satisfies matches().  Called by driver _init()
 * functions before wifi/BLE stacks are started (§21.4).
 *
 * requires_ble: pass true for BLE-control drivers (BLE connection is the
 *   primary control transport); false for RC-emulation drivers.  This flag
 *   controls whether a slot counts as "disconnected" for the ble_core
 *   has_disconnected_cameras() gate (§12.9).
 */
esp_err_t camera_manager_register_driver(const camera_driver_t *driver,
                                          camera_model_match_fn   matches,
                                          camera_ctx_create_fn    create_ctx,
                                          bool                    requires_ble);

/* ----- Slot lookup ----- */

/* Returns slot index [0, CAMERA_MAX_SLOTS), or -1 if not found. */
int camera_manager_find_by_mac(const uint8_t mac[6]);

/*
 * Allocate a new slot for an unknown camera discovered over BLE.
 * Sets placeholder name, model = UNKNOWN.  Does NOT write NVS (model must be
 * set before saving).  Returns slot index or -1 if full.
 */
int camera_manager_register_new(const uint8_t mac[6]);

/* ----- BLE state transitions (called by open_gopro_ble) ----- */

void camera_manager_on_ble_connected(int slot, uint16_t conn_handle);
void camera_manager_on_ble_ready(int slot);      /* CAM_BLE_READY */
void camera_manager_on_ble_disconnected_by_handle(uint16_t conn_handle);

/* ----- Slot field updates ----- */

/* Set model and immediately try to assign a matching registered driver. */
void camera_manager_set_model(int slot, camera_model_t model);
void camera_manager_set_name(int slot, const char *name);

/*
 * Called by a driver after its connection / readiness sequence completes
 * and the camera is ready to accept recording commands.  Sets WIFI_CAM_READY
 * (the enum name is retained for both transports) and starts the per-slot
 * mismatch poll timer.
 *
 * For RC-emulation cameras this is called on the first received UDP
 * datagram from the camera (keepalive ACK, `st`, `SH`, or `cv` reply).
 * For BLE-control cameras this is called after GetHardwareInfo +
 * SetCameraControlStatus complete.
 */
void camera_manager_on_camera_ready(int slot);

/* Called when a SoftAP-using camera leaves the AP.  Stops the poll timer. */
void camera_manager_on_wifi_disconnected(int slot);

/*
 * Called by an RC-emulation driver when a previously-READY slot has gone
 * silent on UDP for longer than its silence threshold but is still associated
 * to the SoftAP (the WoL retry loop is now active).  Demotes wifi_status from
 * WIFI_CAM_READY to WIFI_CAM_PROBING and stops the mismatch poll timer; does
 * NOT touch ip_addr or wifi_associated, and does NOT invoke the driver's
 * on_wifi_disconnected hook — the slot is reachable in principle, the
 * controller just doesn't have a fresh response.  No-op if the slot is not
 * currently READY.  When the camera answers again the driver re-runs its
 * promote path and camera_manager_on_camera_ready() restores READY.
 */
void camera_manager_on_camera_unresponsive(int slot);

/*
 * Called from the wifi_manager on_station_ip callback.
 * Updates last_ip for the matching slot (if any).  RC-emulation only —
 * BLE-control cameras never associate to the SoftAP.
 */
void camera_manager_on_station_ip(const uint8_t mac[6], uint32_t ip);

/*
 * Called from the wifi_manager on_station_associated / on_station_disconnected
 * callbacks.  Tracks whether the camera is currently joined to our SoftAP.
 */
void camera_manager_on_station_associated(const uint8_t mac[6]);
void camera_manager_on_station_disassociated(const uint8_t mac[6]);

/* ----- NVS persistence ----- */

/* Returns ESP_ERR_INVALID_ARG if model == CAMERA_MODEL_UNKNOWN (§6.1). */
esp_err_t camera_manager_save_slot(int slot);

/* ----- Queries ----- */

uint32_t           camera_manager_get_last_ip(int slot);
camera_model_t     camera_manager_get_model(int slot);
int                camera_manager_get_slot_count(void);

/* Copies slot state into *out.  Returns ESP_ERR_INVALID_ARG for bad index. */
esp_err_t          camera_manager_get_slot_info(int slot, camera_slot_info_t *out);

/* Translation for CAN 0x601 frame payload (§14.2). */
camera_can_state_t camera_manager_get_slot_can_state(int slot);

/* ----- Recording intent (§13) ----- */

/* Called by CAN manager on every received 0x600 frame (idempotent). */
void camera_manager_set_desired_recording_all(desired_recording_t intent);

/* Called by web UI for manual per-slot control. */
void camera_manager_set_desired_recording_slot(int slot, desired_recording_t intent);

bool camera_manager_get_auto_control(void);
void camera_manager_set_auto_control(bool enabled);

/* ----- Slot removal with compaction (§20.5) ----- */

esp_err_t camera_manager_remove_slot(int slot);

/*
 * Reorder camera slots in RAM and NVS (§20.6).
 *
 * Indices here are 0-based internal slot numbers — the http_server is
 * responsible for converting from the 1-based external "Cam N" numbering
 * before calling.  new_order[i] is the current slot index of the camera
 * that should occupy position i after the reorder.  Example:
 * new_order = {2, 0, 3, 1} moves the camera currently in slot 2 to slot 0,
 * slot 0 to slot 1, etc.
 *
 * Returns ESP_ERR_INVALID_STATE if any camera involved in the reorder is
 * currently WIFI_CAM_READY or CAM_BLE_CONNECTED (caller responds with 409).
 * Returns ESP_ERR_INVALID_ARG for out-of-range indices or count mismatch.
 */
esp_err_t camera_manager_reorder_slots(const int *new_order, int count);

/* ----- Callbacks for ble_core (§12.9) -----
 *
 * Pass these as function pointers when constructing ble_core_callbacks_t
 * in open_gopro_ble_init().  Signatures must match ble_core_is_known_addr_cb_t
 * and ble_core_has_disconnected_cameras_cb_t respectively.
 */
bool camera_manager_is_known_ble_addr(ble_addr_t addr);
bool camera_manager_has_disconnected_cameras(void);

/* ----- First-pair tracking ----------------------------------------------- *
 *
 * Sets first_pair_complete = true on the slot and persists to NVS.  Idempotent
 * (cheap re-save).  Called by the BLE driver at the end of the readiness
 * sequence, and immediately after registration by the RC-emulation driver
 * (which has no multi-step handshake).
 */
esp_err_t camera_manager_mark_first_pair_complete(int slot);

/*
 * Clear first_pair_complete on the slot and persist to NVS.  Used by the
 * web UI "Re-pair" affordance to force the legacy wireless/pair/complete
 * orchestration to re-run on the next BLE reconnect — e.g. after the user
 * runs Reset Connections on the camera and the camera-side app entry is
 * wiped.  Returns ESP_OK whether the flag was already false or not.
 */
esp_err_t camera_manager_clear_first_pair_complete(int slot);

/* ==========================================================================
 * Pair-attempt state machine (BLE add-camera flow)
 *
 * Tracks a single in-flight initial-pair attempt.  Reconnects do NOT use this
 * machine — it exists purely for surfacing per-step status and errors during
 * the user-initiated add-camera flow.  Sticky terminal states: once the state
 * reaches SUCCESS or FAILED, it remains there until the next pair_attempt_begin
 * call clears it.
 * ========================================================================== */

/* Begin a new pair attempt.
 *
 * Returns ESP_OK on success.  Returns ESP_ERR_INVALID_STATE if a non-terminal
 * attempt is already in flight (caller should respond with HTTP 409).
 *
 * transport selects the cleanup path used by pair_attempt_cancel():
 *   - PAIR_TRANSPORT_BLE: ble_gap_conn_cancel + ble_gap_terminate; remove slot
 *     only if it was registered during PROVISIONING and never reached
 *     first_pair_complete.
 *   - PAIR_TRANSPORT_WIFI_RC: remove the registered slot by MAC (RC slots are
 *     committed at register time, so the cleanup path is unconditional).
 *   addr_type is BLE-specific; pass 0 for RC. */
esp_err_t pair_attempt_begin(const uint8_t addr[6], uint8_t addr_type,
                              pair_attempt_transport_t transport);

/*
 * Restart the in-flight pair-attempt watchdog with a new timeout.  Used by
 * long-running post-BLE phases (e.g. the legacy wireless/pair/complete WiFi
 * handshake in open_gopro_ble/pair_complete.c) to extend their deadline
 * beyond the default 20-second BLE-setup watchdog.
 *
 * No-op if no attempt is in flight.  The watchdog is automatically disarmed
 * by advance(SUCCESS) / fail() / cancel() — callers don't need to clear it
 * themselves.
 */
void pair_attempt_reset_watchdog(uint32_t timeout_ms);

/* True if a non-terminal pair attempt is in flight and its target address
 * matches addr.  Drivers use this to decide whether to drive the state
 * machine for a given camera (vs. a normal reconnect). */
bool pair_attempt_addr_matches(const uint8_t addr[6]);

/* Driver-side transitions.  All forward-only and idempotent — calling
 * advance() with the current or an earlier state is a no-op.  fail() only
 * sets FAILED if the current state is non-terminal, so a real cause set
 * before an inevitable BLE-disconnect cleanup is preserved. */
void pair_attempt_advance(pair_attempt_state_t new_state);
void pair_attempt_fail(pair_attempt_error_t err, const char *message);

/* Record the model number once it has been read from the camera. */
void pair_attempt_set_model(camera_model_t model);

/* Snapshot the current state for the web UI. */
void pair_attempt_get(pair_attempt_info_t *out);

/* True if a non-terminal attempt is currently in flight. */
bool pair_attempt_in_flight(void);

/* Record the BLE connection handle once L2 is up.  Called from the BLE
 * driver's on_connected callback when the address matches the in-flight
 * pair attempt.  Used by pair_attempt_cancel() to terminate the link. */
void pair_attempt_set_conn_handle(uint16_t conn_handle);

/* Abort an in-flight pair attempt:
 *   - Sets state = FAILED, error_code = CANCELLED (so any racing
 *     advance(SUCCESS) from the BLE thread becomes a no-op).
 *   - Calls ble_gap_conn_cancel() to abort an in-flight connect.
 *   - If a conn_handle was recorded, calls ble_gap_terminate() on it.
 *   - If a slot was registered during PROVISIONING and never reached
 *     first_pair_complete, removes it from camera_manager.
 *
 * Returns ESP_OK if a cancel was issued, ESP_ERR_INVALID_STATE if no
 * non-terminal attempt was in flight. */
esp_err_t pair_attempt_cancel(void);
