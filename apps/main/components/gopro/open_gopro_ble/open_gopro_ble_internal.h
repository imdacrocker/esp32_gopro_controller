/*
 * open_gopro_ble_internal.h — Private shared types and declarations.
 *
 * Not part of the public include path.  Only included by the .c files inside
 * this component.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "open_gopro_ble.h"
#include "open_gopro_ble_spec.h"
#include "camera_manager.h"
#include "ble_core.h"

/* ---- GATT handle table (one per connected slot) -------------------------- */

typedef struct {
    uint16_t cmd_write;
    uint16_t cmd_resp_notify;
    uint16_t settings_write;
    uint16_t settings_resp_notify;
    uint16_t query_write;
    uint16_t query_resp_notify;
    uint16_t nw_mgmt_write;
    uint16_t nw_mgmt_resp_notify;
    uint16_t wifi_ap_state_indicate;  /* GP-0005, separate GP-0001 service */
    uint16_t wifi_ap_ssid;            /* GP-0002, encryption-required Read */
    uint16_t wifi_ap_password;        /* GP-0003, encryption-required Read */
} gopro_gatt_handles_t;

/* ---- Per-slot driver context (§15.4) ------------------------------------- */

typedef struct {
    uint16_t             conn_handle;
    int                  slot;
    gopro_gatt_handles_t gatt;
    uint16_t             negotiated_mtu;

    /* Readiness poll (GetHardwareInfo until status=0).
     *
     * readiness_polling, cam_ctrl_pending, and third_party_pending below are
     * atomic_bool because each is read+cleared by two tasks racing each other:
     * the response handler runs on the NimBLE host task (core 1) when a notify
     * arrives, while the corresponding timeout callback runs on the esp_timer
     * task (core 0).  Without atomic exchange both can pass the gate and both
     * call the completion path, producing duplicate state transitions.
     * Callers use atomic_exchange(&flag, false) at the claim point — exactly
     * one caller observes a true→false transition and proceeds. */
    atomic_bool        readiness_polling;
    uint8_t            readiness_retry_count;
    esp_timer_handle_t readiness_timer;

    /* SetCameraControlStatus(EXTERNAL) handshake — arms after readiness,
     * gates the rest of the connection sequence on the response or timeout. */
    atomic_bool        cam_ctrl_pending;
    esp_timer_handle_t cam_ctrl_timer;

    /* SetThirdPartyClient handshake — arms before cam_ctrl, gates the rest
     * of the sequence on a TLV response (cmd 0x50) or timeout. */
    atomic_bool        third_party_pending;
    esp_timer_handle_t third_party_timer;

    /* SetDateTime deferred-send flag — set when readiness completes but UTC
     * is not yet session-synced.  open_gopro_ble_sync_time_all() consumes
     * this flag and sends SetDateTime when UTC arrives. */
    bool datetime_pending_utc;

    /* BLE keepalive timer (3 s) */
    esp_timer_handle_t keepalive_timer;

    /* Recording-status poll (GetStatusValue every GOPRO_STATUS_POLL_INTERVAL_MS) */
    esp_timer_handle_t status_poll_timer;

    /* Cached recording status — updated by status response handler;
     * read by driver vtable get_recording_status (no lock — single
     * 4-byte enum write on Xtensa LX7 is atomic). */
    camera_recording_status_t cached_status;
} gopro_ble_ctx_t;

/* Sentinel for "no connection" */
#define GOPRO_CONN_NONE  BLE_HS_CONN_HANDLE_NONE

/* ---- Context management (driver.c) --------------------------------------- */

/* Returns ctx for slot, or NULL if slot is out of range / uninitialized. */
gopro_ble_ctx_t *gopro_ctx_by_slot(int slot);

/* Returns ctx whose conn_handle matches, or NULL. */
gopro_ble_ctx_t *gopro_ctx_by_conn(uint16_t conn_handle);

/*
 * Look up the advertised name captured during discovery for a given MAC.
 * Copies into `out` (NUL-terminated, truncated if needed) and returns true on
 * hit; returns false if no matching discovery record is cached.
 */
bool open_gopro_ble_lookup_disc_name(const uint8_t mac[6], char *out, size_t out_len);

/* ---- GATT discovery (gatt.c) --------------------------------------------- */

/* Start MTU exchange + characteristic discovery.  Called from pairing.c. */
void gopro_gatt_start_discovery(gopro_ble_ctx_t *ctx);

/* ---- Readiness poll (readiness.c) ---------------------------------------- */

/* Begin sending GetHardwareInfo; arms readiness_timer. */
void gopro_readiness_start(gopro_ble_ctx_t *ctx);

/* Stop and delete the readiness timer.  Safe if timer was never started. */
void gopro_readiness_cancel(gopro_ble_ctx_t *ctx);

/*
 * Called by query.c when the SetCameraControlStatus response arrives
 * (Feature 0xF1, Action 0xE9).  Also called by the timeout timer.
 * Advances to camera-ready state.
 */
void gopro_readiness_handle_cam_ctrl_acked(gopro_ble_ctx_t *ctx, uint8_t result);

/*
 * Called by query.c when the SetThirdPartyClient TLV response arrives
 * (cmd_id 0x50).  Stops the wait timer and advances the connection sequence:
 *  - legacy-BLE cameras skip SetCameraControlStatus and go straight to ready
 *  - other cameras proceed into the SetCameraControlStatus handshake
 */
void gopro_readiness_handle_third_party_acked(gopro_ble_ctx_t *ctx, uint8_t status);

/* ---- Control (control.c) ------------------------------------------------- */

/* Send SetDateTime to a connected slot.  Best-effort, no retry on failure. */
void gopro_control_set_datetime(gopro_ble_ctx_t *ctx);

/* Send SetCameraControlStatus(EXTERNAL).  Returns 0 on enqueue success. */
int  gopro_control_send_set_cam_ctrl(gopro_ble_ctx_t *ctx);

/* Send SetShutter (TLV 0x01). on=true starts recording, on=false stops. */
int  gopro_control_send_shutter(gopro_ble_ctx_t *ctx, bool on);

/*
 * Send Sleep (TLV cmd 0x05) on cmd_write.  Fire-and-forget — does not wait
 * for the cmd_resp_notify response.  Returns 0 on enqueue success, -1 if
 * the slot is not connected or has no cmd_write handle.
 */
int  gopro_control_send_sleep(gopro_ble_ctx_t *ctx);

/*
 * Send RequestPairingFinish (Feature 0x03 / Action 0x01) on the Network
 * Management channel.  Tells the camera the initial pairing flow is complete,
 * which clears the on-screen pairing prompt on supported models.  Best-effort:
 * silently skips if the slot has no nw_mgmt_write handle (older firmware).
 * Officially supported on Hero11 Mini / Hero12 / Hero13 / Max 2 / Lit Hero.
 */
int  gopro_control_send_pairing_finish(gopro_ble_ctx_t *ctx);

/*
 * Send SetThirdPartyClient (TLV cmd 0x50).  Sent to every BLE camera after
 * GetHardwareInfo — legacy Hero5/6/7 require it to complete app pairing,
 * newer cameras accept it harmlessly.
 */
int  gopro_control_send_third_party_client(gopro_ble_ctx_t *ctx);

/*
 * Send legacy SetMode (TLV cmd 0x02) with mode = video.  Used only for
 * legacy-BLE cameras (gopro_model_uses_legacy_ble); newer cameras use Load
 * Preset Group (cmd 0x3E) instead, which is not implemented in V2 yet.
 */
int  gopro_control_send_set_mode_video(gopro_ble_ctx_t *ctx);

/*
 * Send SetWifi (TLV cmd 0x17) to toggle the camera's WiFi AP.  Used by the
 * legacy pair-complete orchestration in pair_complete.c.
 */
int  gopro_control_send_set_wifi(gopro_ble_ctx_t *ctx, bool on);

/*
 * Send Wireless Band setting (ID 178) to force the camera AP onto 2.4 GHz
 * (band=GOPRO_WIFI_BAND_2_4GHZ) or 5 GHz (band=GOPRO_WIFI_BAND_5GHZ).
 * Fire-and-forget — response on settings_resp_notify is not awaited by
 * this call.  Returns 0 on enqueue success, -1 on no-link.
 */
int  gopro_control_send_set_wifi_band(gopro_ble_ctx_t *ctx, uint8_t band);

/* Start the 3-second periodic BLE keepalive timer. */
void gopro_keepalive_start(gopro_ble_ctx_t *ctx);

/* ---- Legacy WiFi pair-complete (pair_complete.c) ------------------------ */

/*
 * Run the legacy wireless/pair/complete handshake for cameras that need it
 * (Hero6/7/8 — see gopro_model_needs_wifi_pair_complete).  Spawns a one-shot
 * task that reads SSID + password over BLE, briefly switches the radio to
 * STA mode to issue the HTTP call on the camera's AP, then returns to AP.
 * On success: marks first_pair_complete and advances pair_attempt to
 * SUCCESS.  On any failure: terminates the BLE link, fails the pair attempt
 * with PAIR_ERROR_PAIR_COMPLETE_FAIL, and removes the slot.
 */
void gopro_pair_complete_run(gopro_ble_ctx_t *ctx);

/* Stop and delete the keepalive timer.  Safe if never started. */
void gopro_keepalive_stop(gopro_ble_ctx_t *ctx);

/* ---- Recording-status poll (status.c) ------------------------------------ */

/* Start the 5 s periodic GetStatusValue poll. */
void gopro_status_poll_start(gopro_ble_ctx_t *ctx);

/* Stop and delete the status poll timer. */
void gopro_status_poll_stop(gopro_ble_ctx_t *ctx);

/*
 * Parse a GetStatusValue response (cmd_id=0x13 already stripped) and
 * update ctx->cached_status.  Body is a sequence of (id, len, value) tuples.
 */
void gopro_status_handle_response(gopro_ble_ctx_t *ctx,
                                   const uint8_t *body, uint16_t body_len);

/*
 * One-shot blocking query of status ID 76 (WirelessBand).  Sends
 * GetStatusValue(76) on the Query channel and waits up to timeout_ms for the
 * response to be parsed by gopro_status_handle_response.
 *
 *   *out_known   = true if the camera included a WirelessBand entry,
 *                  false on timeout / camera silently dropped the ID.
 *   *out_is_5ghz = true if the byte was 0x01 (5 GHz), false otherwise.
 *
 * Must be called from a real FreeRTOS task (it blocks on a semaphore).
 * Only one query may be in flight at a time.  Returns ESP_OK whether the
 * camera answered or not — check *out_known to disambiguate.
 */
esp_err_t gopro_status_query_band_blocking(gopro_ble_ctx_t *ctx,
                                            uint32_t  timeout_ms,
                                            bool     *out_known,
                                            bool     *out_is_5ghz);

/* ---- Response reassembly (query.c) --------------------------------------- */

typedef enum {
    GOPRO_CHAN_CMD,       /* cmd_resp_notify  (GP-0073) */
    GOPRO_CHAN_SETTINGS,  /* settings_resp_notify (GP-0075) */
    GOPRO_CHAN_QUERY,     /* query_resp_notify (GP-0077) */
    GOPRO_CHAN_NW_MGMT,   /* nw_mgmt_resp_notify (GP-0092) */
} gopro_channel_t;

/*
 * Feed an incoming ATT notification into the per-slot reassembler for the
 * given channel.  Calls the appropriate handler when a complete response
 * has been accumulated.
 */
void gopro_query_feed(gopro_ble_ctx_t *ctx, gopro_channel_t chan,
                      const uint8_t *data, uint16_t len);

/* Free reassembly state for a slot (called on disconnect). */
void gopro_query_free(gopro_ble_ctx_t *ctx);

/* ---- Notification routing (notify.c) ------------------------------------- */

/* Called by ble_core's on_notify_rx callback. */
void gopro_notify_rx(uint16_t conn_handle, uint16_t attr_handle,
                     const uint8_t *data, uint16_t len);

/* ---- GPBS packet helpers ------------------------------------------------- */

/*
 * Encode a GPBS general header for payload_len bytes.
 * payload_len must be <= GPBS_HDR_GENERAL_MAX (31).
 * Returns header byte count (1).
 */
static inline int gpbs_write_hdr(uint8_t *buf, uint8_t payload_len)
{
    buf[0] = payload_len & GPBS_HDR_GENERAL_MAX;
    return 1;
}

/*
 * Encode a GPBS extended-13 header for payload_len bytes (32–8191).
 * Returns header byte count (2).
 */
static inline int gpbs_write_hdr13(uint8_t *buf, uint16_t payload_len)
{
    buf[0] = GPBS_HDR_EXT13 | ((payload_len >> 8) & 0x1Fu);
    buf[1] = payload_len & 0xFFu;
    return 2;
}
