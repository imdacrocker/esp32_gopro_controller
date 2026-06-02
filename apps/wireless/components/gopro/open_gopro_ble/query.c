/*
 * query.c — GPBS packet reassembly and response dispatch.
 *
 * GoPro cameras may fragment long responses across multiple ATT notifications.
 * Each slot/channel pair has an independent reassembly buffer.  When a complete
 * response is accumulated, it is dispatched to the appropriate handler.
 *
 * Reassembly header formats:
 *
 *   Start packet:
 *     General  (len ≤ 31):  byte[0] = 0b000LLLLL
 *     Ext-13   (len ≤ 8191): byte[0] = 0b001HHHHH, byte[1] = LLLLLLLL
 *     Ext-16   (len ≤ 65535): byte[0] = 0b010?????, byte[1-2] = length
 *
 *   Continuation packet:
 *     byte[0] = 0b1SSSSSSS  (S = 7-bit sequence number, counted from 0)
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/protocol/data_protocol.html
 */

#include <string.h>
#include "esp_log.h"
#include "open_gopro_ble_internal.h"

static const char *TAG = "gopro_ble/query";

/* ---- Per-slot/channel reassembly state ----------------------------------- */

typedef struct {
    uint8_t  buf[GPBS_MAX_RESPONSE_LEN];
    uint16_t expected_len;
    uint16_t received;
    bool     active;
    uint8_t  next_seq;
} reassembly_t;

/* One reassembly buffer per channel per slot (CMD, SETTINGS, QUERY, NW_MGMT). */
#define GOPRO_CHAN_COUNT  4
static reassembly_t s_asm[CAMERA_MAX_SLOTS][GOPRO_CHAN_COUNT];

static reassembly_t *get_asm(gopro_ble_ctx_t *ctx, gopro_channel_t chan)
{
    if (ctx->slot < 0 || ctx->slot >= CAMERA_MAX_SLOTS) {
        return NULL;
    }
    if ((int)chan < 0 || (int)chan >= GOPRO_CHAN_COUNT) {
        return NULL;
    }
    return &s_asm[ctx->slot][(int)chan];
}

/* ---- Response dispatch --------------------------------------------------- */

extern void gopro_readiness_on_response(gopro_ble_ctx_t *ctx,
                                         const uint8_t *data, uint16_t len);

/*
 * Parse a ResponseGeneric body and return field 1 (EnumResultGeneric).
 * Returns 0 if missing.
 */
static uint8_t parse_generic_result(const uint8_t *body, uint16_t len)
{
    uint16_t pos = 0;
    while (pos < len) {
        if (pos >= len) break;
        uint8_t tag = body[pos++];
        if (tag == GOPRO_RESP_GENERIC_RESULT_TAG) {
            if (pos >= len) return 0;
            return body[pos];
        }
        /* Skip unknown field. */
        uint8_t wire = tag & 0x07u;
        if (wire == 0) {
            if (pos < len) pos++;
        } else if (wire == 2) {
            if (pos >= len) return 0;
            uint8_t skip = body[pos++];
            pos += skip;
        } else {
            return 0;
        }
    }
    return 0;
}

/*
 * Protobuf feature response: [feature_id, action_id, body...].
 *
 * Feature 0xF1 (COMMAND) responses arrive on the cmd channel.
 * Feature 0x03 (NETWORK MANAGEMENT) responses arrive on the nw_mgmt channel.
 */
static void dispatch_proto_resp(gopro_ble_ctx_t *ctx,
                                 const uint8_t *data, uint16_t len)
{
    if (len < GOPRO_PROTO_RESP_HDR_LEN) {
        ESP_LOGW(TAG, "slot %d: short proto resp len=%d", ctx->slot, len);
        return;
    }
    uint8_t feature_id = data[GOPRO_PROTO_RESP_FEATURE_IDX];
    uint8_t action_id  = data[GOPRO_PROTO_RESP_ACTION_IDX];
    const uint8_t *body = data + GOPRO_PROTO_RESP_HDR_LEN;
    uint16_t body_len   = len - GOPRO_PROTO_RESP_HDR_LEN;

    if (feature_id == GOPRO_PROTO_FEATURE_COMMAND &&
        action_id  == GOPRO_CMD_RESP_SET_CAM_CTRL) {
        uint8_t result = parse_generic_result(body, body_len);
        gopro_readiness_handle_cam_ctrl_acked(ctx, result);
        return;
    }

    if (feature_id == GOPRO_PROTO_FEATURE_NW_MGMT &&
        action_id  == GOPRO_NW_MGMT_RESP_PAIRING_FINISH) {
        uint8_t result = parse_generic_result(body, body_len);
        ESP_LOGI(TAG, "slot %d: PairingFinish ack result=%u", ctx->slot, result);
        return;
    }

    ESP_LOGD(TAG, "slot %d: unhandled proto resp feat=0x%02x act=0x%02x",
             ctx->slot, feature_id, action_id);
}

static void dispatch(gopro_ble_ctx_t *ctx, gopro_channel_t chan,
                     const uint8_t *data, uint16_t len)
{
    if (len == 0) {
        return;
    }

    /*
     * Protobuf feature responses arrive on the cmd channel (feature 0xF1) and
     * the nw_mgmt channel (feature 0x03).  On the cmd channel we detect by
     * byte 0; on nw_mgmt every response is protobuf so we route unconditionally.
     */
    if (chan == GOPRO_CHAN_NW_MGMT) {
        dispatch_proto_resp(ctx, data, len);
        return;
    }

    uint8_t b0 = data[0];
    if (b0 == GOPRO_PROTO_FEATURE_COMMAND) {
        dispatch_proto_resp(ctx, data, len);
        return;
    }

    /* Otherwise, fall through to channel-specific TLV handling. */
    switch (chan) {
    case GOPRO_CHAN_CMD:
        if (len >= GOPRO_RESP_HDR_LEN &&
            data[GOPRO_RESP_CMD_IDX] == GOPRO_CMD_GET_HARDWARE_INFO) {
            gopro_readiness_on_response(ctx, data, len);
        } else if (len >= GOPRO_RESP_HDR_LEN &&
                   data[GOPRO_RESP_CMD_IDX] == GOPRO_CMD_SET_THIRD_PARTY_CLIENT) {
            gopro_readiness_handle_third_party_acked(ctx,
                                                      data[GOPRO_RESP_STATUS_IDX]);
        } else if (len >= GOPRO_RESP_HDR_LEN) {
            ESP_LOGD(TAG, "slot %d: cmd resp cmd=0x%02x status=0x%02x",
                     ctx->slot,
                     data[GOPRO_RESP_CMD_IDX],
                     data[GOPRO_RESP_STATUS_IDX]);
        }
        break;

    case GOPRO_CHAN_SETTINGS:
        if (len >= GOPRO_RESP_HDR_LEN) {
            uint8_t setting_id = data[GOPRO_RESP_CMD_IDX];
            uint8_t status     = data[GOPRO_RESP_STATUS_IDX];
            /* Keepalive (0x5B) fires every 3 s — keep it at DEBUG.  Everything
             * else (band changes, mode changes, etc.) is rare enough to log
             * at INFO so the pair-complete diagnostic doesn't need a custom
             * response handler. */
            if (setting_id == GOPRO_CMD_KEEPALIVE) {
                ESP_LOGD(TAG, "slot %d: keepalive ack status=0x%02x",
                         ctx->slot, status);
            } else {
                ESP_LOGI(TAG, "slot %d: settings resp id=0x%02x status=0x%02x",
                         ctx->slot, setting_id, status);
            }
        }
        break;

    case GOPRO_CHAN_QUERY:
        if (len >= GOPRO_RESP_HDR_LEN &&
            data[GOPRO_RESP_CMD_IDX] == GOPRO_QUERY_GET_STATUS_VALUE) {
            /* GetStatusValue response: strip the 2-byte TLV header. */
            gopro_status_handle_response(ctx,
                                          data + GOPRO_RESP_HDR_LEN,
                                          len - GOPRO_RESP_HDR_LEN);
        } else {
            ESP_LOGD(TAG, "slot %d: query resp len=%d", ctx->slot, len);
        }
        break;

    case GOPRO_CHAN_NW_MGMT:
        /* Handled above. */
        break;
    }
}

/* ---- Feed ---------------------------------------------------------------- */

void gopro_query_feed(gopro_ble_ctx_t *ctx, gopro_channel_t chan,
                      const uint8_t *data, uint16_t len)
{
    reassembly_t *a = get_asm(ctx, chan);
    if (!a || len == 0) {
        return;
    }

    bool is_continuation = (data[0] & GPBS_HDR_CONTINUATION) != 0;

    if (!is_continuation) {
        /* --- Start packet --- */
        uint8_t hdr_type = (data[0] >> 5) & 0x03u;
        uint16_t payload_len;
        uint16_t hdr_bytes;

        switch (hdr_type) {
        case 0: /* General: length in bits[4:0] */
            payload_len = data[0] & 0x1Fu;
            hdr_bytes   = 1;
            break;
        case 1: /* Extended 13-bit */
            if (len < 2) { return; }
            payload_len = ((uint16_t)(data[0] & 0x1Fu) << 8) | data[1];
            hdr_bytes   = 2;
            break;
        case 2: /* Extended 16-bit */
            if (len < 3) { return; }
            payload_len = ((uint16_t)data[1] << 8) | data[2];
            hdr_bytes   = 3;
            break;
        default:
            ESP_LOGW(TAG, "slot %d: reserved GPBS header type", ctx->slot);
            return;
        }

        if (payload_len > GPBS_MAX_RESPONSE_LEN) {
            ESP_LOGW(TAG, "slot %d: response too large (%d B), discarding",
                     ctx->slot, payload_len);
            return;
        }

        a->expected_len = payload_len;
        a->received     = 0;
        a->active       = true;
        a->next_seq     = 0;

        uint16_t body_in_pkt = len - hdr_bytes;
        if (body_in_pkt > payload_len) {
            body_in_pkt = payload_len;
        }
        memcpy(a->buf, data + hdr_bytes, body_in_pkt);
        a->received = body_in_pkt;

    } else {
        /* --- Continuation packet --- */
        if (!a->active) {
            ESP_LOGW(TAG, "slot %d chan=%d: continuation without start, discarding",
                     ctx->slot, (int)chan);
            return;
        }
        uint8_t seq = data[0] & 0x7Fu;
        if (seq != a->next_seq) {
            ESP_LOGW(TAG, "slot %d: seq mismatch exp=%d got=%d, resetting",
                     ctx->slot, a->next_seq, seq);
            a->active = false;
            return;
        }
        a->next_seq++;

        uint16_t body_in_pkt = len - 1;
        uint16_t remaining   = a->expected_len - a->received;
        if (body_in_pkt > remaining) {
            body_in_pkt = remaining;
        }
        memcpy(a->buf + a->received, data + 1, body_in_pkt);
        a->received += body_in_pkt;
    }

    if (a->received >= a->expected_len) {
        a->active = false;
        dispatch(ctx, chan, a->buf, a->expected_len);
    }
}

/* ---- Free ---------------------------------------------------------------- */

void gopro_query_free(gopro_ble_ctx_t *ctx)
{
    if (ctx->slot < 0 || ctx->slot >= CAMERA_MAX_SLOTS) {
        return;
    }
    memset(s_asm[ctx->slot], 0, sizeof(s_asm[ctx->slot]));
}
