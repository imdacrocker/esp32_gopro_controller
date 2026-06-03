/*
 * can_manager.h — CAN bus manager public API (§14).
 *
 * Receives 0x600 logging commands and 0x602 UTC timestamps from RaceCapture.
 * Transmits 0x601 camera status at 5 Hz.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* ---- Logging state (§14.2) ----------------------------------------------- */

typedef enum {
    LOGGING_STATE_UNKNOWN = 0,   /* No 0x600 frame received, or 5 s timeout elapsed */
    LOGGING_STATE_NOT_LOGGING,
    LOGGING_STATE_LOGGING,
} can_logging_state_t;

/* ---- Callback types (§14.5) ---------------------------------------------- */

/* Every received frame — for development and bus sniffing. */
typedef void (*can_rx_frame_cb_t)(uint32_t id, const uint8_t *data,
                                   uint8_t len, void *arg);

/* Every received 0x600 frame; never called with LOGGING_STATE_UNKNOWN. */
typedef void (*can_logging_state_cb_t)(can_logging_state_t state, void *arg);

/* Exactly once, on the first valid 0x602 frame (year > 2020). */
typedef void (*can_utc_acquired_cb_t)(uint64_t utc_ms, void *arg);

/*
 * Fires on every received 0x603 frame with byte0 != 0 (request shutdown).
 * Idempotent — repeated requests during SHUTTING_DOWN / COMPLETE are
 * handled by the shutdown_manager itself; the callback fires unconditionally.
 * See docs/design/shutdown.md §6.
 */
typedef void (*can_shutdown_request_cb_t)(void *arg);

/* ---- Callback registration struct ---------------------------------------- */

typedef struct {
    can_rx_frame_cb_t          on_rx_frame;             /* nullable */
    void                      *on_rx_frame_arg;
    can_logging_state_cb_t     on_logging_state;         /* nullable */
    void                      *on_logging_state_arg;
    can_utc_acquired_cb_t      on_utc_acquired;          /* nullable */
    void                      *on_utc_acquired_arg;
    can_shutdown_request_cb_t  on_shutdown_request;      /* nullable */
    void                      *on_shutdown_request_arg;
} can_manager_callbacks_t;

/* ==========================================================================
 * Public API
 * ========================================================================== */

/*
 * Register callbacks.  Must be called before can_manager_init().
 * Struct is copied by value; caller does not need to keep it alive.
 * All fields are NULL-safe.
 */
void can_manager_register_callbacks(const can_manager_callbacks_t *cbs);

/*
 * Start the TWAI driver, RX task, TX timer, and 5 s watchdog.
 * Load timezone offset from NVS.
 * Must be called after cam_core_init() (and after camera_manager_init()
 * in the wireless app, which is what invokes cam_core_init()).
 */
void can_manager_init(void);

/* Current logging state — safe to call from any task. */
can_logging_state_t can_manager_get_logging_state(void);

/*
 * Returns the estimated current UTC as a Unix epoch in milliseconds,
 * extrapolated from the last known UTC anchor using esp_timer_get_time().
 * The anchor is set by either a 0x602 GPS frame, a manual web-UI set, or
 * (at boot) a value restored from NVS — so this can return true with an
 * NVS-restored value before any live sync has occurred this session.
 * Returns false (and leaves *out_ms unchanged) if no anchor is available.
 */
bool can_manager_get_utc_ms(uint64_t *out_ms);

/*
 * True only if UTC has been synced live this session by either a 0x602 GPS
 * frame or a successful can_manager_set_manual_utc_ms() call.  An NVS-restored
 * value at boot does NOT count.  Use this to gate operations that require
 * authoritative time (e.g. pushing SetDateTime to a camera).
 */
bool can_manager_utc_is_session_synced(void);

/*
 * CAN bitrate configuration.
 *
 * Allowed values (bps): 50000, 100000, 125000, 250000, 500000, 1000000.
 * Default: 1000000 (1 Mbps).  Persisted in NVS; takes effect on next boot.
 *
 * can_manager_set_bitrate() validates the input against the allowed list and
 * returns ESP_ERR_INVALID_ARG otherwise.
 */
uint32_t  can_manager_get_bitrate(void);
esp_err_t can_manager_set_bitrate(uint32_t bitrate_bps);

/* ---- Per-channel CAN identifier configuration ----------------------------- *
 *
 * Each of the four CAN channels (logging command RX, camera status TX, GPS
 * UTC RX, shutdown request RX) is independently configurable as either a
 * standard 11-bit ID or an extended 29-bit ID.  Persisted in NVS; takes
 * effect on next boot — see docs/design/can-id-configuration.md.
 *
 * Defaults:  logging=0x600 std, status=0x601 std, utc=0x602 std, shut=0x603 std.
 *
 * Ranges:    std  -> [0x008 .. 0x7FF]
 *            ext  -> [0x008 .. 0x1FFFFFFF]
 *
 * IDs below 0x008 are reserved for high-priority traffic on most vehicle
 * networks and are rejected by can_manager_set_channel().
 */

typedef enum {
    CAN_CH_LOGGING_CMD = 0,
    CAN_CH_CAM_STATUS,
    CAN_CH_GPS_UTC,
    CAN_CH_SHUTDOWN_REQ,
    CAN_CH_COUNT,
} can_channel_id_t;

typedef struct {
    bool     extended;   /* false = 11-bit standard, true = 29-bit extended */
    uint32_t id;
} can_channel_t;

/*
 * Read the boot-time channel configuration.  Safe to call from any task at
 * any time after can_manager_init() returns — values are frozen for the
 * boot session.  Callers (camera_manager debug screens, /api/settings/can)
 * use this when they need the values currently driving RX dispatch/TX header.
 *
 * Returns ESP_ERR_INVALID_ARG for an out-of-range ch.
 */
esp_err_t can_manager_get_channel(can_channel_id_t ch, can_channel_t *out);

/*
 * Persist a new value for one channel to NVS.  Does NOT update s_channels
 * for the current boot session — the running dispatch table and TX header
 * keep boot-time values until reboot.  Matches the bitrate model.
 *
 * Validates range + IDE per the ranges above.  Does NOT validate against
 * collisions with other channels — that is the API handler's job
 * (api_settings.c validates the merged set before persisting any value).
 *
 * Returns ESP_ERR_INVALID_ARG for bad ch, bad id range, or reserved low id.
 */
esp_err_t can_manager_set_channel(can_channel_id_t ch, can_channel_t value);

/*
 * Read the compile-time default for a channel.  Used by the reset-to-defaults
 * path and by can_manager_init()'s NVS fall-back when no saved value exists.
 */
can_channel_t can_manager_channel_default(can_channel_id_t ch);

/*
 * Persist a UTC-to-local offset in NVS (§14.3).
 * Clamped to IANA valid range [−12, +14].
 */
void   can_manager_set_tz_offset(int8_t hours);
int8_t can_manager_get_tz_offset(void);

/*
 * Manually set the system time from the web UI (POST /api/settings/datetime).
 * Only accepted when GPS time has not yet been acquired (gps_valid == false).
 * On success, fires the on_utc_acquired callback exactly as a real GPS frame
 * would, triggering time sync to all connected cameras.
 *
 * Returns ESP_ERR_INVALID_STATE if GPS time is already valid.
 * Returns ESP_ERR_INVALID_ARG  if utc_ms is implausibly small.
 */
esp_err_t can_manager_set_manual_utc_ms(uint64_t utc_ms);
