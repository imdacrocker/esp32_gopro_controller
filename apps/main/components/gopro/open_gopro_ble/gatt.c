/*
 * gatt.c — Service/characteristic discovery and CCCD subscription.
 *
 * Runs entirely on the NimBLE host task.  All operations are chained through
 * NimBLE callbacks; nothing blocks.
 *
 * Flow:
 *   gopro_gatt_start_discovery()
 *     -> ble_gattc_disc_all_svcs()   [discover all primary services]
 *     -> on_svc_disc()               [record each service's handle range]
 *     -> begin_chr_discovery_next()  [when BLE_HS_EDONE]
 *     -> ble_gattc_disc_all_chrs()   [scoped to current service handle range]
 *     -> on_chr_disc()               [match each chr UUID to gatt handle table]
 *     -> begin_chr_discovery_next()  [advance to next service, or start CCCDs]
 *     -> gopro_gatt_write_next_cccd() [when all services done]
 *     -> on_cccd_write()             [sequential CCCD writes, one per notify/indicate chr]
 *     -> gopro_readiness_start()     [when all CCCDs written]
 *
 * MTU: GoPro cameras initiate the MTU exchange themselves shortly after
 * encryption.  We rely on NimBLE's preferred-MTU kconfig
 * (CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU) to advertise the larger MTU when the
 * camera asks; no explicit ble_gattc_exchange_mtu() is needed.
 */

#include <string.h>
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "open_gopro_ble_internal.h"

static const char *TAG = "gopro_ble/gatt";

/* Forward declaration — defined later in this file. */
static void gopro_gatt_write_next_cccd(gopro_ble_ctx_t *ctx);
static void begin_chr_discovery_next(gopro_ble_ctx_t *ctx);

/* ---- Known GoPro characteristic UUID table ------------------------------- */

/*
 * Maps each GoPro characteristic to the byte offset of its handle field
 * inside gopro_gatt_handles_t.  Used by on_chr_disc() to populate the table.
 */
typedef struct {
    ble_uuid128_t uuid;
    uint16_t      offset;   /* offsetof(gopro_gatt_handles_t, field) */
} chr_map_entry_t;

#define HANDLE_OFF(field)  ((uint16_t)offsetof(gopro_gatt_handles_t, field))

static const chr_map_entry_t k_chr_map[] = {
    { GOPRO_CHR_CMD_WRITE_UUID,           HANDLE_OFF(cmd_write)            },
    { GOPRO_CHR_CMD_RESP_NOTIFY_UUID,     HANDLE_OFF(cmd_resp_notify)      },
    { GOPRO_CHR_SETTINGS_WRITE_UUID,      HANDLE_OFF(settings_write)       },
    { GOPRO_CHR_SETTINGS_RESP_UUID,       HANDLE_OFF(settings_resp_notify) },
    { GOPRO_CHR_QUERY_WRITE_UUID,         HANDLE_OFF(query_write)          },
    { GOPRO_CHR_QUERY_RESP_NOTIFY_UUID,   HANDLE_OFF(query_resp_notify)    },
    { GOPRO_CHR_NW_MGMT_WRITE_UUID,       HANDLE_OFF(nw_mgmt_write)        },
    { GOPRO_CHR_NW_MGMT_RESP_NOTIFY_UUID, HANDLE_OFF(nw_mgmt_resp_notify)  },
    { GOPRO_CHR_WIFI_AP_STATE_UUID,       HANDLE_OFF(wifi_ap_state_indicate) },
    { GOPRO_CHR_WIFI_AP_SSID_UUID,        HANDLE_OFF(wifi_ap_ssid)         },
    { GOPRO_CHR_WIFI_AP_PASSWORD_UUID,    HANDLE_OFF(wifi_ap_password)     },
};
#define CHR_MAP_LEN  (sizeof(k_chr_map) / sizeof(k_chr_map[0]))

/* ---- Dynamic service list ------------------------------------------------ */

#define GATT_MAX_SERVICES  16

typedef struct {
    uint16_t start;
    uint16_t end;
} svc_range_t;

static svc_range_t s_svc_list[CAMERA_MAX_SLOTS][GATT_MAX_SERVICES];
static int         s_svc_count[CAMERA_MAX_SLOTS];
static int         s_svc_cur[CAMERA_MAX_SLOTS];

/* ---- Dynamic CCCD subscription list -------------------------------------- */

/*
 * Built at runtime: every 128-bit characteristic that advertises NOTIFY or
 * INDICATE during discovery is appended here.  This subscribes to whatever
 * the camera actually offers rather than a fixed list.
 */
#define GATT_MAX_NOTIFY_CHRS  16

typedef struct {
    uint16_t val_handle;
    uint16_t cccd_val;
} notify_chr_t;

static notify_chr_t s_notify_list[CAMERA_MAX_SLOTS][GATT_MAX_NOTIFY_CHRS];
static int          s_notify_count[CAMERA_MAX_SLOTS];
static int          s_subscr_cur[CAMERA_MAX_SLOTS];

/* ---- Characteristic discovery callback ----------------------------------- */

static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;

    if (error->status == BLE_HS_EDONE) {
        /* All characteristics discovered for this service — move to next. */
        begin_chr_discovery_next(ctx);
        return 0;
    }

    if (error->status != 0) {
        ESP_LOGE(TAG, "slot %d: chr discovery error 0x%04x (svc %d/%d)",
                 ctx->slot, error->status,
                 s_svc_cur[ctx->slot], s_svc_count[ctx->slot]);
        /* Advance to next service rather than aborting. */
        begin_chr_discovery_next(ctx);
        return 0;
    }

    /* Match UUID to our known table and record val_handle. */
    for (int i = 0; i < (int)CHR_MAP_LEN; i++) {
        if (ble_uuid_cmp(&chr->uuid.u, &k_chr_map[i].uuid.u) == 0) {
            uint8_t *handles = (uint8_t *)&ctx->gatt;
            memcpy(handles + k_chr_map[i].offset, &chr->val_handle, sizeof(uint16_t));
            ESP_LOGD(TAG, "slot %d: found chr offset=%u val_handle=0x%04x",
                     ctx->slot, k_chr_map[i].offset, chr->val_handle);
            break;
        }
    }

    /* Queue any 128-bit notifiable/indicatable characteristic for CCCD subscription. */
    bool has_notify   = (chr->properties & BLE_GATT_CHR_F_NOTIFY)  != 0;
    bool has_indicate = (chr->properties & BLE_GATT_CHR_F_INDICATE) != 0;
    if ((has_notify || has_indicate) &&
        chr->uuid.u.type == BLE_UUID_TYPE_128 &&
        s_notify_count[ctx->slot] < GATT_MAX_NOTIFY_CHRS) {
        int n = s_notify_count[ctx->slot]++;
        s_notify_list[ctx->slot][n].val_handle = chr->val_handle;
        s_notify_list[ctx->slot][n].cccd_val   = has_notify ? BLE_CCCD_NOTIFY : BLE_CCCD_INDICATE;
        ESP_LOGD(TAG, "slot %d: queuing CCCD val_handle=0x%04x (%s)",
                 ctx->slot, chr->val_handle, has_notify ? "NOTIFY" : "INDICATE");
    }

    return 0;
}

/* ---- Per-service characteristic discovery -------------------------------- */

static void begin_chr_discovery_next(gopro_ble_ctx_t *ctx)
{
    int slot = ctx->slot;

    s_svc_cur[slot]++;

    if (s_svc_cur[slot] >= s_svc_count[slot]) {
        /* All services scanned — check for missing handles then start CCCDs. */
        uint8_t *handles = (uint8_t *)&ctx->gatt;
        int missing = 0;
        for (int i = 0; i < (int)CHR_MAP_LEN; i++) {
            uint16_t h;
            memcpy(&h, handles + k_chr_map[i].offset, sizeof(h));
            if (h == 0) {
                ESP_LOGW(TAG, "slot %d: characteristic at offset %u not found",
                         slot, k_chr_map[i].offset);
                missing++;
            }
        }
        if (missing > 0) {
            ESP_LOGW(TAG, "slot %d: %d characteristic(s) missing", slot, missing);
        }

        ESP_LOGI(TAG, "slot %d: chr discovery done (%d svc(s), %d notify chr(s) queued)",
                 slot, s_svc_count[slot], s_notify_count[slot]);

        s_subscr_cur[slot] = 0;
        gopro_gatt_write_next_cccd(ctx);
        return;
    }

    uint16_t start = s_svc_list[slot][s_svc_cur[slot]].start;
    uint16_t end   = s_svc_list[slot][s_svc_cur[slot]].end;

    int rc = ble_gattc_disc_all_chrs(ctx->conn_handle, start, end,
                                      on_chr_disc, ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "slot %d: ble_gattc_disc_all_chrs svc %d rc=%d — skipping",
                 slot, s_svc_cur[slot], rc);
        begin_chr_discovery_next(ctx);
    }
}

/* ---- CCCD write callback ------------------------------------------------- */

static int on_cccd_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;

    if (error->status != 0) {
        /* Non-fatal: log and continue — camera may not have this chr. */
        ESP_LOGW(TAG, "slot %d: CCCD write error 0x%04x (continuing)",
                 ctx->slot, error->status);
    }

    s_subscr_cur[ctx->slot]++;

    gopro_gatt_write_next_cccd(ctx);
    return 0;
}

/* ---- Sequential CCCD writer ---------------------------------------------- */

static void gopro_gatt_write_next_cccd(gopro_ble_ctx_t *ctx)
{
    int cur   = s_subscr_cur[ctx->slot];
    int total = s_notify_count[ctx->slot];

    if (cur >= total) {
        ESP_LOGI(TAG, "slot %d: all CCCDs subscribed (%d)", ctx->slot, total);
        /* Heavy GATT traffic for this slot is done — let ble_core look for
         * other paired cameras while readiness polling continues. */
        ble_core_resume_background_scan();
        gopro_readiness_start(ctx);
        return;
    }

    uint16_t val_handle  = s_notify_list[ctx->slot][cur].val_handle;
    uint16_t cccd_handle = val_handle + 1;
    uint16_t cccd_val    = s_notify_list[ctx->slot][cur].cccd_val;

    int rc = ble_gattc_write_flat(ctx->conn_handle, cccd_handle,
                                   &cccd_val, sizeof(cccd_val),
                                   on_cccd_write, ctx);
    if (rc != 0) {
        ESP_LOGW(TAG, "slot %d: CCCD write enqueue failed rc=%d, skipping",
                 ctx->slot, rc);
        s_subscr_cur[ctx->slot]++;
        gopro_gatt_write_next_cccd(ctx);
    }
}

/* ---- Service discovery callback ------------------------------------------ */

static int on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_svc *svc, void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;
    int slot = ctx->slot;

    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "slot %d: service discovery done — %d service(s) found",
                 slot, s_svc_count[slot]);
        /* Initialise cursor to -1 so begin_chr_discovery_next() increments to 0. */
        s_svc_cur[slot] = -1;
        begin_chr_discovery_next(ctx);
        return 0;
    }

    if (error->status != 0) {
        ESP_LOGE(TAG, "slot %d: service discovery error 0x%04x", slot, error->status);
        ble_gap_terminate(ctx->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (s_svc_count[slot] < GATT_MAX_SERVICES) {
        ESP_LOGD(TAG, "slot %d: svc start=0x%04x end=0x%04x",
                 slot, svc->start_handle, svc->end_handle);
        s_svc_list[slot][s_svc_count[slot]].start = svc->start_handle;
        s_svc_list[slot][s_svc_count[slot]].end   = svc->end_handle;
        s_svc_count[slot]++;
    }

    return 0;
}

/* ---- Entry point --------------------------------------------------------- */

void gopro_gatt_start_discovery(gopro_ble_ctx_t *ctx)
{
    memset(&ctx->gatt, 0, sizeof(ctx->gatt));
    memset(s_svc_list[ctx->slot], 0, sizeof(s_svc_list[ctx->slot]));
    s_svc_count[ctx->slot]  = 0;
    s_svc_cur[ctx->slot]    = 0;
    memset(s_notify_list[ctx->slot], 0, sizeof(s_notify_list[ctx->slot]));
    s_notify_count[ctx->slot] = 0;
    s_subscr_cur[ctx->slot]   = 0;

    ESP_LOGI(TAG, "slot %d: starting service discovery", ctx->slot);

    int rc = ble_gattc_disc_all_svcs(ctx->conn_handle, on_svc_disc, ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "slot %d: ble_gattc_disc_all_svcs rc=%d", ctx->slot, rc);
        ble_gap_terminate(ctx->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}
