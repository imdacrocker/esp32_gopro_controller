#pragma once

#include "ble_core.h"
#include <stdbool.h>

/* Callback table — defined in ble_init.c, read by all four source files. */
extern ble_core_callbacks_t g_cbs;

/*
 * s_connecting: true while a ble_gap_connect() attempt is in flight.
 * s_discovering: true while a user-initiated discovery scan is running.
 * Both defined in ble_scan.c.
 */
extern volatile bool s_connecting;
extern volatile bool s_discovering;

/*
 * Start background reconnect scan if conditions are met:
 *   - not currently connecting or in discovery mode
 *   - has_disconnected_cameras() returns true (or callback is NULL)
 * Defined in ble_scan.c, called from ble_init.c and ble_connect.c.
 */
void start_scan_if_needed(void);

/*
 * Initialise the deferred-rescan callout used when ble_gap_connect() returns
 * BLE_HS_EDONE (controller still holds stale connection state). Call once
 * from ble_core_init() after nimble_port_init().
 * Defined in ble_scan.c.
 */
void ble_core_scan_init(void);

/*
 * True once ble_core_init() has initialised the NimBLE port (and with it the
 * default event queue).  Defined in ble_init.c.
 */
extern volatile bool g_port_initialized;

/*
 * Synchronous body of ble_core_purge_unknown_bonds().  Must run on the
 * NimBLE host task.  Defined in ble_connect.c, called from on_sync.
 */
void ble_core_purge_unknown_bonds_now(void);

/*
 * GAP event handler for all connection-lifecycle events.
 * Defined in ble_connect.c, registered as the callback for ble_gap_connect().
 */
int connection_event_cb(struct ble_gap_event *event, void *arg);
