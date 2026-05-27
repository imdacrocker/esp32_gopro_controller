#pragma once

#include "esp_err.h"
#include "host/ble_hs.h"

/*
 * Callback types — all fire on the NimBLE host task.
 * Implementations must not block indefinitely.
 */
typedef void (*ble_core_on_disc_cb_t)(ble_addr_t addr, int8_t rssi,
                                      const uint8_t *data, int len);
typedef void (*ble_core_on_connected_cb_t)(uint16_t conn_handle, ble_addr_t addr);
typedef void (*ble_core_on_encrypted_cb_t)(uint16_t conn_handle, ble_addr_t addr);
typedef void (*ble_core_on_disconnected_cb_t)(uint16_t conn_handle, ble_addr_t addr,
                                               uint8_t reason);
typedef void (*ble_core_on_notify_rx_cb_t)(uint16_t conn_handle, uint16_t attr_handle,
                                            const uint8_t *data, uint16_t len);

/* Returns true if addr belongs to a known (paired) camera — gates auto-reconnect. */
typedef bool (*ble_core_is_known_addr_cb_t)(ble_addr_t addr);

/* Returns true if any paired camera is currently disconnected — gates background scan. */
typedef bool (*ble_core_has_disconnected_cameras_cb_t)(void);

/*
 * Returns true if the system is in shutdown mode (SHUTTING_DOWN or COMPLETE).
 * When true, ble_core suppresses background reconnect scans and connection
 * attempts so cameras aren't redialed mid-shutdown.  Nullable — when NULL
 * the gate is treated as "not shutting down".  See docs/design/shutdown.md §7.
 */
typedef bool (*ble_core_is_shutdown_active_cb_t)(void);

typedef struct {
    ble_core_on_disc_cb_t                   on_disc;
    ble_core_on_connected_cb_t              on_connected;
    ble_core_on_encrypted_cb_t              on_encrypted;
    ble_core_on_disconnected_cb_t           on_disconnected;
    ble_core_on_notify_rx_cb_t              on_notify_rx;
    ble_core_is_known_addr_cb_t             is_known_addr;
    ble_core_has_disconnected_cameras_cb_t  has_disconnected_cameras;
    ble_core_is_shutdown_active_cb_t        is_shutdown_active;
} ble_core_callbacks_t;

/*
 * Must be called before ble_core_init().
 * Callbacks are copied by value; caller does not need to keep the struct alive.
 * All fields are optional (NULL-safe).
 */
void ble_core_register_callbacks(const ble_core_callbacks_t *cbs);

/*
 * Initialises the NimBLE host and starts the BLE host task on core 1.
 * on_sync fires asynchronously once the stack is ready and triggers the
 * background reconnect scan automatically.
 */
void ble_core_init(void);

/*
 * Start a user-initiated discovery scan.
 * All advertisements are forwarded to on_disc for timeout_ms milliseconds.
 * Cancels any running background scan first.
 * Safe to call from any task — routed through the NimBLE event queue.
 */
void ble_core_start_discovery(int timeout_ms);

/*
 * Cancel a running discovery scan and resume the background scan if needed.
 * Safe to call from any task.
 */
void ble_core_stop_discovery(void);

/*
 * Initiate a connection to addr.
 * Cancels any running scan first.
 * Safe to call from any task — routed through the NimBLE event queue.
 */
esp_err_t ble_core_connect_by_addr(const ble_addr_t *addr);

/*
 * ATT Write Without Response (non-blocking).
 * Safe to call from any task — routed through the NimBLE event queue.
 */
esp_err_t ble_core_gatt_write(uint16_t conn_handle, uint16_t attr_handle,
                               const uint8_t *data, uint16_t len);

/*
 * Walk the NimBLE peer-security store and delete any bond whose address is
 * not in keep[0..keep_count-1].
 *
 * MUST be called before ble_core_init() — not safe after the NimBLE host
 * task has started.
 */
void ble_core_purge_unknown_bonds(const ble_addr_t keep[], int keep_count);

/*
 * Terminate the active connection to addr (if any) and delete its bond.
 * Safe to call from any task — posted to the NimBLE event queue.
 * Caller must remove the camera from camera_manager BEFORE calling this so
 * that is_known_addr returns false and the disconnect handler does not
 * trigger an automatic reconnect.
 */
void ble_core_remove_bond(const ble_addr_t *addr);

/*
 * Resume the background reconnect scan if any paired camera is still
 * disconnected.  No-op if a connect attempt is in progress, a discovery
 * scan is running, or has_disconnected_cameras() returns false.
 *
 * Intended call site: after a successful first-camera readiness milestone
 * (e.g. all CCCDs subscribed) so the controller can begin looking for
 * additional configured cameras without contending with the in-progress
 * pairing/discovery traffic.
 *
 * Must be called from the NimBLE host task (e.g. a NimBLE callback).
 */
void ble_core_resume_background_scan(void);
