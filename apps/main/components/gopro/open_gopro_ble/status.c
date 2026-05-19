/*
 * status.c — GetStatusValue periodic poll for recording state.
 *
 * After camera-ready, a per-slot 5 s timer issues a GetStatusValue (TLV 0x13)
 * on the Query channel asking for status ID 10 (Encoding).
 * The response arrives on query_resp_notify (GP-0077) and is dispatched to
 * gopro_status_handle_response() by query.c.
 *
 * The cached enum lives directly on the gopro_ble_ctx_t — read non-blocking
 * by the camera_driver_t.get_recording_status() vtable entry.
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/query.html#get-status-value
 */

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "open_gopro_ble_internal.h"

static const char *TAG = "gopro_ble/status";

/*
 * GetStatusValue request payload (3 bytes):
 *   [0] 0x02     GPBS header (general, len=2)
 *   [1] 0x13     cmd_id = GetStatusValue
 *   [2] 0x0A     status_id = Encoding (0 = idle, 1 = recording)
 */
static const uint8_t k_get_status_pkt[3] = {
    0x02u,
    GOPRO_QUERY_GET_STATUS_VALUE,
    GOPRO_STATUS_ID_ENCODING_ACTIVE,
};

static void on_status_poll_timer(void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;

    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.query_write == 0) {
        return;
    }
    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.query_write,
                        k_get_status_pkt, sizeof(k_get_status_pkt));
}

void gopro_status_poll_start(gopro_ble_ctx_t *ctx)
{
    if (ctx->status_poll_timer) {
        return;  /* already running */
    }

    esp_timer_create_args_t args = {
        .callback        = on_status_poll_timer,
        .arg             = ctx,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "gopro_status",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &ctx->status_poll_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(ctx->status_poll_timer,
                                              GOPRO_STATUS_POLL_INTERVAL_MS * 1000ULL));
    ESP_LOGD(TAG, "slot %d: status poll started", ctx->slot);

    /* Fire one immediately so we don't wait the full interval for the first
     * cached_status update. */
    on_status_poll_timer(ctx);
}

void gopro_status_poll_stop(gopro_ble_ctx_t *ctx)
{
    if (!ctx->status_poll_timer) {
        return;
    }
    esp_timer_stop(ctx->status_poll_timer);
    esp_timer_delete(ctx->status_poll_timer);
    ctx->status_poll_timer = NULL;
    ESP_LOGD(TAG, "slot %d: status poll stopped", ctx->slot);
}

/*
 * Response body layout (after the [cmd_id, status] TLV header has been
 * stripped by the caller):
 *
 *   for each requested status:
 *     [id (1B), len (1B), value (len B)]
 *
 * We requested only status ID 10, so the body should be either empty (if the
 * camera couldn't supply it) or [0x0A, 0x01, 0x00|0x01].
 */
void gopro_status_handle_response(gopro_ble_ctx_t *ctx,
                                   const uint8_t *body, uint16_t body_len)
{
    uint16_t pos = 0;
    while (pos + 2 <= body_len) {
        uint8_t id  = body[pos++];
        uint8_t len = body[pos++];
        if (pos + len > body_len) {
            break;
        }
        if (id == GOPRO_STATUS_ID_ENCODING_ACTIVE && len >= 1) {
            uint8_t v = body[pos];
            camera_recording_status_t new_status =
                (v == 0) ? CAMERA_RECORDING_IDLE : CAMERA_RECORDING_ACTIVE;
            if (new_status != ctx->cached_status) {
                ESP_LOGI(TAG, "slot %d: recording → %s",
                         ctx->slot,
                         new_status == CAMERA_RECORDING_ACTIVE ? "ACTIVE" : "IDLE");
                ctx->cached_status = new_status;
            }
        }
        pos += len;
    }
}
