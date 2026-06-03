/*
 * camera_manager_ble.h — BLE-typed extensions to the camera_manager API.
 *
 * Split out of camera_manager.h so the main camera_manager header does not
 * pull in NimBLE — a step in the multi-variant restructure (docs/multi-
 * variant-restructure-plan.md §4) toward a BLE-free shared `cam_core`
 * component.  Callers that need the BLE-typed entry points include this
 * header; everything else keeps including `camera_manager.h`.
 */
#pragma once

#include "camera_manager.h"
#include "host/ble_hs.h"   /* ble_addr_t */

/* ----- Callback for ble_core (§12.9) -----
 *
 * Pass as a function pointer when constructing ble_core_callbacks_t in
 * open_gopro_ble_init().  Signature must match ble_core_is_known_addr_cb_t.
 */
bool camera_manager_is_known_ble_addr(ble_addr_t addr);
