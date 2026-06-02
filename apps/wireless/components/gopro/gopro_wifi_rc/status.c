/*
 * status.c — Periodic UDP `st` poll and binary status-response parser.
 *
 * The poll handler runs every RC_STATUS_POLL_INTERVAL_MS on the work task and
 * fires one UDP `st` datagram at every slot with last_ip != 0 (not just
 * wifi_ready ones — newly-added slots can't promote until they receive their
 * first response, see §17.5.1).
 *
 * The parser runs on the UDP RX task when an opcode-`st` datagram arrives and
 * decodes bytes 13/14/15 into ctx->recording_status.  recording_status is a
 * single enum (≤ 4 B) written only here; aligned 32-bit stores are atomic on
 * Xtensa LX7, so the single-writer / single-reader pattern (work-task readers
 * via `get_recording_status`) needs no mutex.
 *
 * §17.2.4, §17.4.1, §17.8 of docs/design/camera-manager.md.
 */

#include <string.h>
#include "esp_log.h"
#include "camera_manager.h"
#include "gopro_wifi_rc_internal.h"

static const char *TAG = "gopro_rc/status";

/* ---- Status-response decode (§17.2.4) ------------------------------------ */

void rc_parse_st_response(int slot, const uint8_t *buf, int len)
{
    if (slot < 0 || slot >= CAMERA_MAX_SLOTS) return;
    if (len < RC_RESP_MIN_BYTES) return;

    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];

    uint8_t pwr   = buf[RC_RESP_PWR_OFFSET];
    uint8_t state = buf[RC_RESP_STATE_OFFSET];

    camera_recording_status_t next;
    if (pwr == 1) {
        /* Camera reports off/sleeping — recording state is meaningless. */
        next = CAMERA_RECORDING_UNKNOWN;
    } else if (state == 1) {
        next = CAMERA_RECORDING_ACTIVE;
    } else {
        next = CAMERA_RECORDING_IDLE;
    }

    if (next != ctx->recording_status) {
        ESP_LOGD(TAG, "slot %d: %s → %s",
                 slot,
                 ctx->recording_status == CAMERA_RECORDING_ACTIVE ? "recording" :
                 ctx->recording_status == CAMERA_RECORDING_IDLE   ? "idle"      :
                                                                    "unknown",
                 next == CAMERA_RECORDING_ACTIVE ? "recording" :
                 next == CAMERA_RECORDING_IDLE   ? "idle"      :
                                                   "unknown");
    }
    ctx->recording_status = next;
}

/* ---- Camera-version (cv) response decode (§17.2.5) ---------------------- */

/*
 * Hero7 reference (44 bytes total):
 *   00 00 00 00 00 00 00 00  00 00 00 63 76 00 03 01     header + opcode + reserved
 *   0f 48 44 37 2e 30 31 2e  30 31 2e 39 30 2e 30 30     0x0f=15 + "HD7.01.01.90.00"
 *   0b 48 45 52 4f 37 20 42  6c 61 63 6b                 0x0b=11 + "HERO7 Black"
 *
 * Strings are not NUL-terminated; we use the length prefixes to carve them
 * out and copy into ctx with caps to fit the per-slot buffers.
 */
void rc_parse_cv_response(int slot, const uint8_t *buf, int len)
{
    if (slot < 0 || slot >= CAMERA_MAX_SLOTS) return;
    if (len <= RC_CV_RESP_FW_LEN_OFFSET) return;  /* not enough bytes for fw_len */

    int       fw_len_off = RC_CV_RESP_FW_LEN_OFFSET;
    uint8_t   fw_len     = buf[fw_len_off];
    int       fw_off     = fw_len_off + 1;
    int       fw_end     = fw_off + fw_len;
    if (fw_end >= len) {
        ESP_LOGW(TAG, "slot %d: cv malformed — fw_len=%u runs past end (%d bytes)",
                 slot, (unsigned)fw_len, len);
        return;
    }

    int       name_len_off = fw_end;
    uint8_t   name_len     = buf[name_len_off];
    int       name_off     = name_len_off + 1;
    int       name_end     = name_off + name_len;
    if (name_end > len) {
        ESP_LOGW(TAG, "slot %d: cv malformed — name_len=%u runs past end (%d bytes)",
                 slot, (unsigned)name_len, len);
        return;
    }

    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];

    int fw_copy = fw_len < (int)sizeof(ctx->parsed_firmware) - 1
                  ? fw_len : (int)sizeof(ctx->parsed_firmware) - 1;
    memcpy(ctx->parsed_firmware, &buf[fw_off], fw_copy);
    ctx->parsed_firmware[fw_copy] = '\0';

    int name_copy = name_len < (int)sizeof(ctx->parsed_model_name) - 1
                    ? name_len : (int)sizeof(ctx->parsed_model_name) - 1;
    memcpy(ctx->parsed_model_name, &buf[name_off], name_copy);
    ctx->parsed_model_name[name_copy] = '\0';

    ESP_LOGI(TAG, "slot %d: cv parsed — model='%s' fw='%s'",
             slot, ctx->parsed_model_name, ctx->parsed_firmware);

    /* Hand off to the work task to do the (lock-taking, NVS-writing) apply. */
    rc_work_cmd_t cmd = { .type = RC_CMD_APPLY_CV,
                          .slot_cmd = { .slot = slot } };
    xQueueSend(s_work_queue, &cmd, 0);
}

/* ---- Status poll (§17.8) ------------------------------------------------- */

/*
 * Called every RC_STATUS_POLL_INTERVAL_MS from the work task.  Sends one UDP
 * `st` request to each slot that has an IP — not gated on wifi_ready so that
 * newly-added slots can promote on the first response.  Replies are handled
 * asynchronously by rc_udp_rx_task → rc_parse_st_response().
 */
void rc_handle_status_poll_all(void)
{
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        gopro_wifi_rc_ctx_t *ctx = &s_ctx[i];
        if (ctx->last_ip == 0) continue;
        rc_send_st(ctx->last_ip);
    }
}
