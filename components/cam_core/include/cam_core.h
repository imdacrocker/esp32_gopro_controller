/*
 * cam_core.h — Shared, BLE-free camera abstraction surface
 * (docs/multi-variant-restructure-plan.md §4).
 *
 * Defines the driver vtable, per-slot state record, model-matching
 * function pointers, and the CAN-state enum that every product variant
 * agrees on.  The recording-intent engine, mismatch poll, broadcast
 * dispatch, and CAN-state translation that operate on these types live
 * in cam_core.c (Phase 3.3); at Phase 3.2 only the types are exposed.
 *
 * Wireless-only types (`cam_ble_status_t`, `wifi_cam_status_t`,
 * `pair_attempt_*`, `camera_slot_info_t`) intentionally do NOT live here
 * — they stay next to the wireless camera_manager component so a future
 * single-target / wired variant can depend on cam_core without inheriting
 * wireless concepts.
 */
#pragma once

#include "camera_types.h"
#include "esp_err.h"
#include "esp_timer.h"   /* esp_timer_handle_t in cam_core_slot_t */

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

    /* Nullable — send a model-appropriate sleep command and return ESP_OK on
     * enqueue (not on camera ACK; the shutdown_manager budgets the overall
     * timeout itself).  Return ESP_ERR_NOT_SUPPORTED if the model has no
     * usable sleep path.  See docs/design/shutdown.md §5. */
    esp_err_t                  (*sleep)(void *ctx);
};

/* ---- Driver registration types (§21.4) ----
 *
 * `matches(model)` is called by the manager for each loaded slot to decide
 * which driver owns it.  `create_ctx(slot)` allocates the per-slot driver
 * context once a match is found.
 */
typedef bool   (*camera_model_match_fn)(camera_model_t model);
typedef void  *(*camera_ctx_create_fn)(int slot);

/* ---- CAN camera state (§14.2) ---- */
typedef enum {
    CAMERA_CAN_STATE_UNDEFINED    = 0,
    CAMERA_CAN_STATE_DISCONNECTED = 1,
    CAMERA_CAN_STATE_IDLE         = 2,
    CAMERA_CAN_STATE_RECORDING    = 3,
} camera_can_state_t;

/* ---- Per-slot cam_core state record ---------------------------------------
 *
 * Holds the variant-agnostic slice of slot state: the driver vtable +
 * context, recording intent, grace deadline, mismatch poll timer, ready
 * flag (universal "fully ready for recording commands"), and the
 * `requires_ble` flag the wireless variant uses to gate has_disconnected
 * accounting.
 *
 * The wireless app embeds one of these inside its richer per-slot struct
 * (alongside name, MAC, model, BLE/WiFi status, NVS-persisted fields).
 * A future single-target variant can use it standalone or embed it in its
 * own state.  All access goes through cam_core APIs — reading or writing
 * the fields directly bypasses the locking cam_core manages.
 */
typedef struct {
    const camera_driver_t *driver;
    void                  *driver_ctx;
    desired_recording_t    desired;
    /* Absolute esp_timer deadline (µs) until which the mismatch poll
     * suppresses dispatch — armed after every Start/Stop dispatch so the
     * camera has time to reflect the new state.  0 means "no grace
     * active". */
    int64_t                grace_until_us;
    esp_timer_handle_t     poll_timer;
    /* Universal "fully ready for recording commands" flag.  In wireless
     * today this mirrors `wifi_status == WIFI_CAM_READY` for both BLE and
     * RC slots (the enum name is historical).  Gates dispatch and the CAN
     * state translation. */
    bool                   ready;
    /* Mirrors the driver registration flag — wireless uses it for the
     * BLE-scan "has_disconnected_cameras" gate so RC-only cameras don't
     * keep the BLE scanner running. */
    bool                   requires_ble;
} cam_core_slot_t;

/* ==========================================================================
 * Runtime API
 *
 * Locking model: cam_core owns a single recursive mutex covering its slot
 * registry and the cam_core_slot_t fields of every registered slot.  All
 * field access goes through these functions; the host (wireless or a
 * future variant) must NOT read or write cam_core_slot_t fields directly.
 * Drivers' vtable methods are invoked with the cam_core lock held — they
 * must be non-blocking (post work to their own task queues).
 * ========================================================================== */

/*
 * Initialize the module.  Idempotent.  Must be called once at boot before
 * any other cam_core API.
 */
void cam_core_init(void);

/*
 * Register an externally-allocated cam_core_slot_t at the given index.
 * `slot` must be zero-initialized (typically embedded in a host-owned
 * per-slot struct that is itself zeroed at allocation).  Bumps the
 * internal slot count to max(current, idx+1) so the new slot participates
 * in `cam_core_set_desired_all` and `cam_core_slot_count` iteration.
 *
 * Returns ESP_ERR_INVALID_ARG for an out-of-range index or NULL slot.
 */
esp_err_t cam_core_register_slot(int idx, cam_core_slot_t *slot);

/*
 * Unregister the slot at the given index.  Stops the poll timer if armed
 * and clears the registry entry so subsequent enumerations skip it.  The
 * host-owned cam_core_slot_t buffer itself is untouched.
 */
void cam_core_unregister_slot(int idx);

/*
 * Exclusive upper bound for iteration (`< cam_core_slot_count()`).
 * Slots in [0, count) may be unregistered (gap) — enumerators must skip
 * indices where cam_core_get_can_state returns UNDEFINED, or use
 * cam_core_slot_active to test.
 */
int  cam_core_slot_count(void);

/* True iff a slot is currently registered at this index. */
bool cam_core_slot_active(int idx);

/*
 * Attach a driver to a registered slot.  `driver`, `start_recording`,
 * `stop_recording`, and `get_recording_status` must be non-NULL.  `ctx`
 * is opaque to cam_core; it's passed verbatim to every driver vtable
 * call.  `requires_ble` records whether this driver depends on a live BLE
 * link (used by the wireless variant's BLE-scan gate).
 *
 * Idempotent re-attach is allowed — it overwrites the previous driver.
 * Callers that want first-attach-wins semantics should gate with
 * cam_core_slot_has_driver first.
 */
esp_err_t cam_core_slot_attach_driver(int                       idx,
                                       const camera_driver_t    *driver,
                                       void                     *ctx,
                                       bool                      requires_ble);

/* True iff the slot is registered and currently has a driver attached. */
bool cam_core_slot_has_driver(int idx);

/* True iff the slot's currently attached driver was registered with
 * `requires_ble = true`.  Used by the wireless variant's BLE-scan gate
 * (`has_disconnected_cameras`).  False if the slot has no driver. */
bool cam_core_slot_requires_ble(int idx);

/*
 * Toggle the slot's "fully ready" flag.  Starts (on true) or stops + frees
 * (on false) the per-slot mismatch poll timer.  Idempotent; transitioning
 * to false also clears the grace deadline.  Must NOT be called from the
 * driver's get_recording_status, start_recording, or stop_recording
 * implementations — those run under the cam_core lock and a set_ready(false)
 * would attempt to delete the timer that's currently dispatching.
 */
void cam_core_slot_set_ready(int idx, bool ready);

/* Per-slot intent setter.  On a real intent transition with the slot
 * READY and a driver attached, immediately dispatches the corresponding
 * vtable call and arms the grace deadline.  Idempotent calls (same
 * intent as current) are no-ops. */
void cam_core_set_desired_slot(int idx, desired_recording_t intent);

/* Cross-slot intent setter (called e.g. from the CAN trigger).  Walks
 * all registered slots, updates intent, and immediately dispatches the
 * vtable call for READY slots with attached drivers.  For drivers whose
 * `broadcasts_to_all` is true, calls *_recording_all() once per wave
 * regardless of how many slots use that driver; subsequent same-driver
 * slots have their intent + grace updated but skip dispatch. */
void cam_core_set_desired_all(desired_recording_t intent);

/* Global auto-control flag.  Persisted only in RAM; defaults to true at
 * cam_core_init.  Used by callers (e.g. can_manager) to decide whether
 * to honour external recording-intent updates. */
bool cam_core_get_auto_control(void);
void cam_core_set_auto_control(bool enabled);

/* Non-blocking read of the driver's recording-status cache, or
 * CAMERA_RECORDING_UNKNOWN if no slot / no driver. */
camera_recording_status_t cam_core_get_recording_status(int idx);

/* Current per-slot intent (set by cam_core_set_desired_slot or the most
 * recent cam_core_set_desired_all wave).  DESIRED_RECORDING_UNKNOWN if
 * the slot is not registered or has never received intent. */
desired_recording_t cam_core_get_desired(int idx);

/* True iff the slot is READY, has a driver, and the driver reports
 * CAMERA_RECORDING_ACTIVE. */
bool cam_core_is_recording(int idx);

/* CAN-frame state translation (§14.2):
 *   not registered                                       → UNDEFINED
 *   registered but !ready                                → DISCONNECTED
 *   ready & driver reports ACTIVE                        → RECORDING
 *   ready & driver reports IDLE/UNKNOWN, or no driver    → IDLE  */
camera_can_state_t cam_core_get_can_state(int idx);

/* Invoke the driver's sleep entry.  Returns ESP_ERR_NOT_SUPPORTED if the
 * slot has no driver or no sleep handler.  Forwards the driver return
 * code otherwise.  Non-blocking; the driver enqueues the command. */
esp_err_t cam_core_invoke_sleep(int idx);

/* Stop the mismatch poll timer, clear the ready flag, and invoke the
 * driver's teardown entry (if any).  Used by the shutdown path and by
 * the host's slot-removal flow.  Does NOT clear the driver pointer or
 * the registry entry — call cam_core_unregister_slot for that. */
void cam_core_teardown_slot(int idx);

/* After a host-side compaction (slot index changes), notify the slot's
 * driver via its update_slot_index hook.  Safe to call when no driver is
 * attached or the hook is NULL. */
void cam_core_notify_slot_index_changed(int idx);

/* Fire the slot driver's on_wifi_disconnected vtable entry, if any.  No-op
 * if the slot has no driver or the hook is NULL.  Used by transports that
 * surface SoftAP-departure as a separate event (the wireless variant calls
 * this from camera_manager_on_wifi_disconnected). */
void cam_core_notify_wifi_disconnected(int idx);
