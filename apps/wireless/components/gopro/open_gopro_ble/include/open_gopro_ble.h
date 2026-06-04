/*
 * open_gopro_ble.h — Public API for the GoPro BLE control component.
 *
 * This component is the camera_driver_t provider for all BLE-control GoPro
 * models (Hero 9 and later).  It owns:
 *   - BLE discovery, pairing, GATT setup, MTU exchange, bond management
 *   - GetHardwareInfo readiness poll
 *   - SetCameraControlStatus(EXTERNAL) handshake
 *   - SetDateTime push (gated on UTC validity)
 *   - 3 s BLE keepalive
 *   - SetShutter (TLV 0x01) — recording start/stop
 *   - GetStatusValue poll (5 s) — recording status
 *
 * §15 of docs/design/camera-manager.md
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "host/ble_hs.h"
#include "camera_manager.h"

/* Maximum number of cameras shown in the discovery list. */
#define GOPRO_DISC_MAX  10

/* ---- Discovery ----------------------------------------------------------- */

typedef struct {
    ble_addr_t addr;
    int8_t     rssi;
    char       name[32];
} gopro_device_t;

/*
 * Start a user-initiated BLE scan for GoPro cameras.
 * Advertisements matching the GoPro service UUID (0xFEA6) are added to the
 * internal discovery list.  Wraps ble_core_start_discovery().
 */
void open_gopro_ble_start_discovery(void);

/* Cancel the discovery scan. */
void open_gopro_ble_stop_discovery(void);

/*
 * Copy up to max_count discovered devices into out[].
 * Returns the number of entries written.
 * Safe to call from any task.
 */
int open_gopro_ble_get_discovered(gopro_device_t *out, int max_count);

/* ---- Connection ---------------------------------------------------------- */

/*
 * Initiate a connection to a camera by BLE address.
 * Wraps ble_core_connect_by_addr().
 */
void open_gopro_ble_connect_by_addr(const ble_addr_t *addr);

/* ---- UTC sync ------------------------------------------------------------ */

/*
 * Send SetDateTime to every currently-connected camera.  Called by
 * can_manager when a GPS fix is acquired, or by the web UI on a manual
 * time-set request.
 */
void open_gopro_ble_sync_time_all(void);

/* ---- Component lifecycle ------------------------------------------------- */

/*
 * Register BLE callbacks with ble_core, register the camera_driver_t with
 * camera_manager, and purge stale bonds.
 *
 * Must be called after camera_manager_init() and before ble_core_init().
 */
void open_gopro_ble_init(void);

/* ---- Shutdown helper (docs/design/shutdown.md) -------------------------- */

/*
 * Terminate the BLE link for the given slot, if any.  Idempotent — no-op
 * when the slot has no active connection.  Used by shutdown_manager to
 * explicitly drop the link after the sleep TLV has been sent so the camera
 * doesn't wait for its own supervision timeout.  The matching
 * BLE_GAP_EVENT_DISCONNECT will fire as normal and clear the slot's
 * conn_handle via the standard on_disconnected callback path.
 */
void open_gopro_ble_terminate_slot(int slot);
