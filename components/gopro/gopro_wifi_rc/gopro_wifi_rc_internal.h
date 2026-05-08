/*
 * gopro_wifi_rc_internal.h — Private types, globals, and forward declarations
 * shared across the gopro_wifi_rc source files.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "camera_manager.h"
#include "gopro_wifi_rc_spec.h"

/* ---- Per-slot driver context (§17.3) ------------------------------------- */

typedef struct {
    int                        slot;
    uint8_t                    mac[6];           /* for WoL target MAC */
    uint32_t                   last_ip;          /* network byte order */
    camera_recording_status_t  recording_status; /* cache updated by RX task */
    bool                       wifi_ready;
    bool                       identify_attempted; /* set true once cv has been parsed
                                                      and applied; gates cv-retry on tick */
    /* Populated by the UDP RX task on receipt of a `cv` response.  Read by
     * the work task (handle_apply_cv / handle_promote) to map the model_name
     * string to a camera_model_t enum and persist it.  Empty (length 0)
     * until cv arrives.  Note: parsed_model_name is the model identity
     * string (e.g. "HERO7 Black"), not a user-set device name — it is NOT
     * written into the slot's name field. */
    char                       parsed_model_name[32];   /* e.g. "HERO7 Black" */
    char                       parsed_firmware[40];     /* e.g. "HD7.01.01.90.00" */
    /* Updated by the UDP RX task on every datagram from the slot's IP
     * (keepalive ACK, `st` response, `SH` echo, `cv` response).  Compared by
     * keepalive_tick to drive the WoL retry watchdog. */
    TickType_t                 last_response_tick;
    esp_timer_handle_t         keepalive_timer;  /* 3 s periodic, armed per slot */
    esp_timer_handle_t         wol_retry_timer;  /* 2 s periodic, armed on silence */
} gopro_wifi_rc_ctx_t;

/* ---- Work queue message (§17.4) ------------------------------------------ */

typedef enum {
    RC_CMD_STATION_ASSOCIATED,
    RC_CMD_STATION_DHCP,
    RC_CMD_STATION_DISCONNECTED,
    RC_CMD_KEEPALIVE_TICK,
    RC_CMD_WOL_RETRY,
    RC_CMD_STATUS_POLL_ALL,
    RC_CMD_PROMOTE,                    /* RX task → flip wifi_ready, fire datetime */
    RC_CMD_APPLY_CV,                   /* RX task → apply parsed cv data on work task */
    RC_CMD_SYNC_TIME_ALL,
    /* RC_CMD_PROBE is removed — the old HTTP probe loop is gone; readiness is
     * driven by the first UDP response (see rc_handle_promote). */
} rc_work_cmd_type_t;

typedef struct {
    rc_work_cmd_type_t type;
    union {
        struct { uint8_t mac[6]; }              mac_only; /* ASSOCIATED, DISCONNECTED */
        struct { uint8_t mac[6]; uint32_t ip; } mac_ip;   /* DHCP */
        struct { int slot; }                    slot_cmd; /* KEEPALIVE_TICK, WOL_RETRY,
                                                             PROMOTE, APPLY_CV */
    };
} rc_work_cmd_t;

/* ---- Shutter queue message ---- *
 *
 * `ip` is the destination address: a slot's last_ip for unicast (used by the
 * mismatch poll and per-slot web-UI shutter), or 0xFFFFFFFFu for the broadcast
 * fan-out (used by set_desired_recording_all).  `repeat` controls how many
 * times the same SH packet is re-sent back-to-back; broadcasts use
 * RC_SHUTTER_BROADCAST_REPEAT to compensate for unacknowledged 802.11 frames,
 * unicasts use 1.
 */
typedef struct {
    bool     start;     /* false = stop */
    uint32_t ip;        /* network byte order; 0xFFFFFFFFu for broadcast */
    uint8_t  repeat;
} rc_shutter_cmd_t;

/* ---- Globals (defined in driver.c) --------------------------------------- */

extern gopro_wifi_rc_ctx_t s_ctx[CAMERA_MAX_SLOTS];
extern QueueHandle_t       s_work_queue;
extern QueueHandle_t       s_shutter_queue;
extern int                 s_udp_sock; /* shared UDP socket bound to RC_UDP_RX_PORT;
                                          -1 if not open */

/* ---- connection.c -------------------------------------------------------- */

void rc_handle_station_associated(const uint8_t mac[6]);
void rc_handle_station_dhcp(const uint8_t mac[6], uint32_t ip);
void rc_handle_station_disconnected(const uint8_t mac[6]);
void rc_handle_promote(int slot);            /* replaces rc_handle_probe */
void rc_handle_apply_cv(int slot);           /* apply ctx->parsed_model_name / firmware */
void rc_handle_keepalive_tick(int slot);
void rc_handle_wol_retry(int slot);
void rc_arm_keepalive_timer(gopro_wifi_rc_ctx_t *ctx);
void rc_disarm_keepalive_timer(gopro_wifi_rc_ctx_t *ctx);
void rc_arm_wol_retry_timer(gopro_wifi_rc_ctx_t *ctx);
void rc_disarm_wol_retry_timer(gopro_wifi_rc_ctx_t *ctx);

/* ---- command.c ----------------------------------------------------------- */

void rc_shutter_task(void *arg);
void rc_handle_sync_time_all(void);
void rc_send_datetime(int slot);

/*
 * Issue a plain HTTP/1.0 GET to camera at ip:RC_HTTP_PORT.  Used today only
 * by rc_send_datetime — identification went over UDP `cv` instead.
 *
 * timeout_ms applies independently to connect (via O_NONBLOCK + select),
 * send, and recv.  Returns HTTP status code (200 / 4xx / 5xx) or -1 on
 * transport failure.  resp_buf, if non-NULL and buf_len > 0, receives the
 * body (NUL-terminated, silently truncated on overflow).
 */
int rc_http_get(uint32_t ip, const char *path, int timeout_ms,
                char *resp_buf, size_t buf_len);

/* ---- status.c ------------------------------------------------------------ */

void rc_handle_status_poll_all(void);

/*
 * Decode a 20-byte `st` response: bytes 13/14/15 → recording_status.
 * Called by the UDP RX task when an opcode-`st` datagram arrives from a
 * known slot; updates ctx->recording_status in place.
 */
void rc_parse_st_response(int slot, const uint8_t *buf, int len);

/*
 * Decode a `cv` response into ctx->parsed_model_name and ctx->parsed_firmware,
 * then post RC_CMD_APPLY_CV so the work task applies the result.  Called by
 * the UDP RX task on receipt of an opcode-`cv` datagram from a known slot.
 */
void rc_parse_cv_response(int slot, const uint8_t *buf, int len);

/* ---- udp.c --------------------------------------------------------------- */

esp_err_t rc_udp_init(void);
void      rc_send_keepalive(uint32_t ip);
void      rc_send_st(uint32_t ip);                       /* status request */
void      rc_send_sh(uint32_t ip, bool start);           /* shutter start/stop */
void      rc_send_cv(uint32_t ip);                       /* camera version probe */
void      rc_send_wol(uint32_t ip, const uint8_t mac[6]);
void      rc_udp_rx_task(void *arg);
