/*
 * control.c — TLV / protobuf control commands and BLE keepalive.
 *
 *   - SetDateTime (0x0D, TLV)
 *   - SetCameraControlStatus(EXTERNAL) (Feature 0xF1, Action 0x69, protobuf)
 *   - SetShutter (0x01, TLV)
 *   - Keepalive (cmd 0x5B with value 0x42 to settings_write GP-0074, every 3 s)
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/control.html
 */

#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "open_gopro_ble_internal.h"
#include "can_manager.h"
#include "sdkconfig.h"

static const char *TAG = "gopro_ble/ctrl";

/* ---- SetDateTime (§15.5) ------------------------------------------------- */

void gopro_control_set_datetime(gopro_ble_ctx_t *ctx)
{
    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.cmd_write == 0) {
        ESP_LOGI(TAG, "slot %d: SetDateTime (BLE) skipped — not connected or no cmd_write",
                 ctx->slot);
        return;
    }

    /* Only push time to the camera when UTC has been live-synced this session
     * (CAN GPS frame or web-UI manual set).  An NVS-restored value at boot
     * is "close" but not authoritative — we'd rather let the camera keep its
     * own clock than push a stale value. */
    if (!can_manager_utc_is_session_synced()) {
        ESP_LOGI(TAG, "slot %d: SetDateTime (BLE) deferred — UTC not session-synced",
                 ctx->slot);
        return;
    }

    int8_t tz = can_manager_get_tz_offset();
    struct timeval tv;
    gettimeofday(&tv, NULL);
    tv.tv_sec += (time_t)tz * 3600;
    struct tm t;
    gmtime_r(&tv.tv_sec, &t);

    int year = t.tm_year + 1900;

    uint8_t pkt[13] = {
        0x0Cu,                            /* GPBS header: general, len=12 */
        GOPRO_CMD_SET_DATE_TIME,          /* command ID */
        GOPRO_DT_PARAM_DATE,              /* param 1: date */
        GOPRO_DT_PARAM_DATE_LEN,          /* param 1 length = 4 */
        (uint8_t)(year >> 8),             /* year high byte */
        (uint8_t)(year & 0xFF),           /* year low byte */
        (uint8_t)(t.tm_mon + 1),          /* month 1-12 */
        (uint8_t)(t.tm_mday),             /* day 1-31 */
        GOPRO_DT_PARAM_TIME,              /* param 2: time */
        GOPRO_DT_PARAM_TIME_LEN,          /* param 2 length = 3 */
        (uint8_t)(t.tm_hour),
        (uint8_t)(t.tm_min),
        (uint8_t)(t.tm_sec),
    };

    ESP_LOGI(TAG, "slot %d: SetDateTime (BLE) sending %04d-%02d-%02d %02d:%02d:%02d (tz=UTC%+d)",
             ctx->slot, year, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, (int)tz);

    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.cmd_write,
                        pkt, sizeof(pkt));
}

/* ---- SetCameraControlStatus(EXTERNAL) ------------------------------------ */

/*
 * Packet layout (5 bytes total):
 *   [0] 0x04             GPBS header (general, len=4)
 *   [1] 0xF1             Feature ID (COMMAND)
 *   [2] 0x69             Action ID  (RequestSetCameraControlStatus)
 *   [3] 0x08             Protobuf field 1 tag (varint)
 *   [4] 0x02             EnumCameraControlStatus.EXTERNAL
 *
 * Response (Feature 0xF1, Action 0xE9, ResponseGeneric) arrives on
 * cmd_resp_notify and is dispatched by query.c.
 */
static const uint8_t k_set_cam_ctrl_pkt[5] = {
    0x04u,
    GOPRO_PROTO_FEATURE_COMMAND,
    GOPRO_CMD_ACTION_SET_CAM_CTRL,
    GOPRO_CAM_CTRL_PB_STATUS_TAG,
    GOPRO_CAM_CTRL_EXTERNAL,
};

int gopro_control_send_set_cam_ctrl(gopro_ble_ctx_t *ctx)
{
    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.cmd_write == 0) {
        ESP_LOGW(TAG, "slot %d: SetCameraControlStatus skipped — not connected",
                 ctx->slot);
        return -1;
    }
    ESP_LOGI(TAG, "slot %d: → SetCameraControlStatus(EXTERNAL)", ctx->slot);
    return ble_core_gatt_write(ctx->conn_handle, ctx->gatt.cmd_write,
                                k_set_cam_ctrl_pkt, sizeof(k_set_cam_ctrl_pkt));
}

/* ---- RequestPairingFinish (Network Management) -------------------------- */

/*
 * Packet layout (variable length, fits in a single GPBS general header):
 *   [0]   GPBS header (general, len = N)
 *   [1]   Feature ID 0x03 (NETWORK_MANAGEMENT)
 *   [2]   Action ID  0x01 (RequestPairingFinish)
 *   [3]   PB tag 0x08 (field 1 varint)
 *   [4]   EnumPairingFinishState.SUCCESS = 0x01
 *   [5]   PB tag 0x12 (field 2 length-delimited)
 *   [6]   phoneName length L
 *   [7..] phoneName bytes
 *
 * Spec requires phoneName non-empty but states the value has no effect.
 * phoneName comes from CONFIG_DEVICE_IDENTITY_NAME so it stays in sync with
 * the NimBLE GAP device name and the legacy wireless/pair/complete deviceName.
 */

int gopro_control_send_pairing_finish(gopro_ble_ctx_t *ctx)
{
    if (ctx->conn_handle == GOPRO_CONN_NONE) {
        ESP_LOGW(TAG, "slot %d: PairingFinish skipped — not connected", ctx->slot);
        return -1;
    }
    if (ctx->gatt.nw_mgmt_write == 0) {
        ESP_LOGI(TAG, "slot %d: PairingFinish skipped — camera has no GP-0091 (older model)",
                 ctx->slot);
        return -1;
    }

    const char *name      = CONFIG_DEVICE_IDENTITY_NAME;
    uint8_t     name_len  = (uint8_t)strlen(name);
    uint8_t     payload   = (uint8_t)(2u    /* feature + action */
                                    + 2u    /* tag + value (result) */
                                    + 2u    /* tag + len (phoneName) */
                                    + name_len);
    if (payload > GPBS_HDR_GENERAL_MAX) {
        ESP_LOGW(TAG, "slot %d: PairingFinish payload too long (%u)", ctx->slot, payload);
        return -1;
    }

    uint8_t pkt[1 + GPBS_HDR_GENERAL_MAX];
    uint8_t i = 0;
    pkt[i++] = payload;
    pkt[i++] = GOPRO_PROTO_FEATURE_NW_MGMT;
    pkt[i++] = GOPRO_NW_MGMT_ACTION_PAIRING_FINISH;
    pkt[i++] = GOPRO_PAIRING_FINISH_PB_RESULT_TAG;
    pkt[i++] = GOPRO_PAIRING_FINISH_STATE_SUCCESS;
    pkt[i++] = GOPRO_PAIRING_FINISH_PB_NAME_TAG;
    pkt[i++] = name_len;
    memcpy(&pkt[i], name, name_len);
    i = (uint8_t)(i + name_len);

    ESP_LOGI(TAG, "slot %d: -> RequestPairingFinish(SUCCESS, \"%s\")", ctx->slot, name);
    return ble_core_gatt_write(ctx->conn_handle, ctx->gatt.nw_mgmt_write, pkt, i);
}

/* ---- SetThirdPartyClient (legacy app-pairing handshake) ----------------- */

int gopro_control_send_third_party_client(gopro_ble_ctx_t *ctx)
{
    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.cmd_write == 0) {
        ESP_LOGW(TAG, "slot %d: SetThirdPartyClient skipped — not connected", ctx->slot);
        return -1;
    }

    /* TLV: [GPBS hdr=1, cmd=0x50] — no parameters */
    uint8_t pkt[2] = {
        0x01u,
        GOPRO_CMD_SET_THIRD_PARTY_CLIENT,
    };

    ESP_LOGI(TAG, "slot %d: -> SetThirdPartyClient", ctx->slot);
    return ble_core_gatt_write(ctx->conn_handle, ctx->gatt.cmd_write,
                                pkt, sizeof(pkt));
}

/* ---- SetMode (legacy mode selection) ------------------------------------ */

int gopro_control_send_set_mode_video(gopro_ble_ctx_t *ctx)
{
    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.cmd_write == 0) {
        ESP_LOGW(TAG, "slot %d: SetMode skipped — not connected", ctx->slot);
        return -1;
    }

    /* TLV: [GPBS hdr=3, cmd=0x02, param_len=1, value=0x00 (Video)] */
    uint8_t pkt[4] = {
        0x03u,
        GOPRO_CMD_SET_MODE,
        0x01u,
        GOPRO_MODE_VIDEO,
    };

    ESP_LOGI(TAG, "slot %d: -> SetMode(Video)", ctx->slot);
    return ble_core_gatt_write(ctx->conn_handle, ctx->gatt.cmd_write,
                                pkt, sizeof(pkt));
}

/* ---- Wireless Band setting (force camera AP to 2.4 / 5 GHz) ------------- */

int gopro_control_send_set_wifi_band(gopro_ble_ctx_t *ctx, uint8_t band)
{
    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.settings_write == 0) {
        ESP_LOGW(TAG, "slot %d: SetWifiBand skipped — not connected", ctx->slot);
        return -1;
    }

    /* TLV: [GPBS hdr=3, setting_id=0xB2, param_len=1, value=0|1]
     * Same wire format as keepalive — settings IDs share the cmd byte slot. */
    uint8_t pkt[4] = {
        0x03u,
        GOPRO_SETTING_WIRELESS_BAND,
        0x01u,
        band,
    };

    ESP_LOGI(TAG, "slot %d: -> SetWifiBand(%s)",
             ctx->slot,
             band == GOPRO_WIFI_BAND_2_4GHZ ? "2.4 GHz" :
             band == GOPRO_WIFI_BAND_5GHZ   ? "5 GHz"   : "?");
    return ble_core_gatt_write(ctx->conn_handle, ctx->gatt.settings_write,
                                pkt, sizeof(pkt));
}

/* ---- SetWifi (camera AP on/off) ----------------------------------------- */

int gopro_control_send_set_wifi(gopro_ble_ctx_t *ctx, bool on)
{
    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.cmd_write == 0) {
        ESP_LOGW(TAG, "slot %d: SetWifi skipped — not connected", ctx->slot);
        return -1;
    }

    /* TLV: [GPBS hdr=3, cmd=0x17, param_len=1, value=0|1] */
    uint8_t pkt[4] = {
        0x03u,
        GOPRO_CMD_SET_WIFI,
        0x01u,
        on ? GOPRO_WIFI_ON : GOPRO_WIFI_OFF,
    };

    ESP_LOGI(TAG, "slot %d: -> SetWifi(%s)", ctx->slot, on ? "ON" : "OFF");
    return ble_core_gatt_write(ctx->conn_handle, ctx->gatt.cmd_write,
                                pkt, sizeof(pkt));
}

/* ---- SetShutter (start/stop recording) ----------------------------------- */

int gopro_control_send_shutter(gopro_ble_ctx_t *ctx, bool on)
{
    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.cmd_write == 0) {
        ESP_LOGW(TAG, "slot %d: SetShutter skipped — not connected", ctx->slot);
        return -1;
    }

    /* TLV: [GPBS hdr=3, cmd=0x01, param_len=0x01, value=0|1] */
    uint8_t pkt[4] = {
        0x03u,                       /* GPBS header: general, len=3 */
        GOPRO_CMD_SET_SHUTTER,
        0x01u,                       /* param length = 1 */
        on ? 0x01u : 0x00u,
    };

    ESP_LOGI(TAG, "slot %d: → SetShutter(%s)", ctx->slot, on ? "ON" : "OFF");
    return ble_core_gatt_write(ctx->conn_handle, ctx->gatt.cmd_write,
                                pkt, sizeof(pkt));
}

/* ---- BLE keepalive (§15.1) ----------------------------------------------- */

static void on_keepalive_timer(void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;

    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.settings_write == 0) {
        return;
    }
    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.settings_write,
                        k_gopro_keepalive_pkt, sizeof(k_gopro_keepalive_pkt));
}

void gopro_keepalive_start(gopro_ble_ctx_t *ctx)
{
    if (ctx->keepalive_timer) {
        return;  /* already running */
    }

    esp_timer_create_args_t args = {
        .callback        = on_keepalive_timer,
        .arg             = ctx,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "gopro_ka",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &ctx->keepalive_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(ctx->keepalive_timer,
                                              GOPRO_KEEPALIVE_PERIOD_MS * 1000ULL));
    ESP_LOGD(TAG, "slot %d: keepalive started", ctx->slot);
}

void gopro_keepalive_stop(gopro_ble_ctx_t *ctx)
{
    if (!ctx->keepalive_timer) {
        return;
    }
    esp_timer_stop(ctx->keepalive_timer);
    esp_timer_delete(ctx->keepalive_timer);
    ctx->keepalive_timer = NULL;
    ESP_LOGD(TAG, "slot %d: keepalive stopped", ctx->slot);
}
