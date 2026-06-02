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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "open_gopro_ble_internal.h"

static const char *TAG = "gopro_ble/status";

/* Forward declaration — defined below, called from the status-response parser. */
static void band_bridge_signal(uint8_t value);

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
        } else if (id == GOPRO_STATUS_ID_WIRELESS_BAND && len >= 1) {
            band_bridge_signal(body[pos]);
        }
        pos += len;
    }
}

/* ---- One-shot wireless-band query --------------------------------------- *
 *
 * Issues a GetStatusValue request for status ID 76 (WirelessBand) and blocks
 * the caller until either the camera responds (handled above) or the timeout
 * elapses.  Only one query may be in flight at a time — gated by s_busy.
 *
 * Camera firmwares older than Hero9 may not include the id=0x4C entry in
 * their response (the request returns OK but the requested status is just
 * omitted).  In that case we time out and report unknown — the caller falls
 * back to the optimistic "try STA join, see what happens" path.
 *
 * Lives in this file because the response handler above is what dispatches
 * incoming status entries.  Keeping the bridge co-located avoids spilling
 * status-id knowledge into a separate compilation unit.
 *
 * LOAD-BEARING SINGLE-FLIGHT ASSUMPTION: these globals have no slot dimension
 * because the only caller is pair_complete.c's pair_complete_task, and the
 * gate there (s_busy under s_gate_lock, with deferred slots queued in
 * s_pending[]) guarantees exactly one such task runs at a time.  If a
 * non-pair_complete caller is ever added — e.g. a UI-initiated "re-check
 * WiFi band" action, or any code that issues a status query from a different
 * orchestration task — these MUST become per-slot arrays keyed by ctx->slot,
 * and band_bridge_signal() must take a ctx/slot argument routed through
 * gopro_status_handle_response.  See refactor-plan.md (Phase 2, band-query).
 */

static SemaphoreHandle_t s_band_done;
static bool              s_band_busy;
static uint8_t           s_band_value;
static bool              s_band_known;

static void band_bridge_signal(uint8_t value)
{
    if (!s_band_busy) return;
    s_band_value = value;
    s_band_known = true;
    if (s_band_done) xSemaphoreGive(s_band_done);
}

esp_err_t gopro_status_query_band_blocking(gopro_ble_ctx_t *ctx,
                                            uint32_t  timeout_ms,
                                            bool     *out_known,
                                            bool     *out_is_5ghz)
{
    if (!ctx || !out_known || !out_is_5ghz) return ESP_ERR_INVALID_ARG;
    *out_known   = false;
    *out_is_5ghz = false;

    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.query_write == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_band_busy) return ESP_ERR_INVALID_STATE;

    if (s_band_done == NULL) {
        s_band_done = xSemaphoreCreateBinary();
        if (s_band_done == NULL) return ESP_ERR_NO_MEM;
    }
    /* Drain any stale signal so we wait fresh. */
    (void)xSemaphoreTake(s_band_done, 0);

    s_band_known = false;
    s_band_value = 0;
    s_band_busy  = true;

    /* GetStatusValue(76): [hdr=2, cmd=0x13, status_id=0x4C] */
    const uint8_t pkt[3] = {
        0x02u,
        GOPRO_QUERY_GET_STATUS_VALUE,
        GOPRO_STATUS_ID_WIRELESS_BAND,
    };
    ESP_LOGI(TAG, "slot %d: -> GetStatusValue(WirelessBand)", ctx->slot);
    int rc = ble_core_gatt_write(ctx->conn_handle, ctx->gatt.query_write,
                                  pkt, sizeof(pkt));
    if (rc != 0) {
        s_band_busy = false;
        ESP_LOGW(TAG, "slot %d: band query enqueue rc=%d", ctx->slot, rc);
        return ESP_FAIL;
    }

    (void)xSemaphoreTake(s_band_done, pdMS_TO_TICKS(timeout_ms));
    s_band_busy = false;

    if (s_band_known) {
        *out_known   = true;
        *out_is_5ghz = (s_band_value == GOPRO_WIRELESS_BAND_5GHZ);
        ESP_LOGI(TAG, "slot %d: WirelessBand = %s",
                 ctx->slot, *out_is_5ghz ? "5 GHz" : "2.4 GHz");
    } else {
        ESP_LOGI(TAG, "slot %d: WirelessBand not reported (camera too old or busy)",
                 ctx->slot);
    }
    return ESP_OK;
}
