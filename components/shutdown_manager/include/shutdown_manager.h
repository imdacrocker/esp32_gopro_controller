/*
 * shutdown_manager.h — Coordinates the operator/CAN-initiated system
 * shutdown.  See docs/design/shutdown.md.
 *
 * Lifecycle:
 *   IDLE → SHUTTING_DOWN → SHUTDOWN_COMPLETE
 *
 * Transitions are RAM-only (no NVS persistence).  A real reboot via
 * POST /api/reboot returns the state machine to IDLE.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SHUTDOWN_STATE_IDLE          = 0,
    SHUTDOWN_STATE_SHUTTING_DOWN = 1,
    SHUTDOWN_STATE_COMPLETE      = 2,
} shutdown_state_t;

/*
 * Initialise state, mutex, and counters.  Must be called after
 * camera_manager_init() and before can_manager_register_callbacks() — the
 * CAN handlers consult shutdown_manager_is_active() to gate 0x600 frames.
 */
void shutdown_manager_init(void);

/*
 * Begin the shutdown sequence.  Idempotent — calling while
 * SHUTTING_DOWN or SHUTDOWN_COMPLETE is a no-op that returns ESP_OK.
 * Spawns one per-slot task per configured camera.
 */
esp_err_t shutdown_manager_request(void);

/*
 * Bridge for the CAN-0x603 callback.  Drops the unused arg and calls
 * shutdown_manager_request().  Safe to register as can_shutdown_request_cb_t.
 */
void shutdown_manager_on_can_request(void *arg);

/* Current state.  Safe from any context. */
shutdown_state_t shutdown_manager_get_state(void);

/* True if state != IDLE — the universal "are we shutting down?" gate. */
bool shutdown_manager_is_active(void);

/*
 * Failed-slot bitmask.  Bit i is set if the per-slot task for internal slot
 * i exited via the timeout path or a hard error.  Only meaningful in
 * SHUTDOWN_COMPLETE; reads under SHUTTING_DOWN are partial.
 */
uint8_t shutdown_manager_get_failed_slots_mask(void);
