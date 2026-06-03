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
 * own state.  All access is intended to go through cam_core APIs (Phase
 * 3.3); reading or writing the fields directly bypasses the locking
 * cam_core will manage.
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
