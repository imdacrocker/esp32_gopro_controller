/*
 * gatt.c — Service/characteristic discovery and CCCD subscription.
 *
 * Runs entirely on the NimBLE host task.  All operations are chained through
 * NimBLE callbacks; nothing blocks.
 *
 * Flow:
 *   gopro_gatt_start_discovery()
 *     -> ble_gattc_disc_all_svcs()    [discover all primary services]
 *     -> on_svc_disc()                [record each service's handle range]
 *     -> begin_chr_discovery_next()   [when BLE_HS_EDONE]
 *     -> ble_gattc_disc_all_chrs()    [scoped to current service handle range]
 *     -> on_chr_disc()                [match each chr UUID to gatt handle table;
 *                                      queue notify/indicate chrs and compute
 *                                      each one's descriptor-range upper bound]
 *     -> begin_chr_discovery_next()   [advance to next service, or start dsc disc]
 *     -> gopro_gatt_disc_next_dscs()  [when all services done]
 *     -> ble_gattc_disc_all_dscs()    [per notify chr, scoped to (val_handle, end_handle]]
 *     -> on_dsc_disc()                [pick the 0x2902 (CCCD) descriptor's handle]
 *     -> gopro_gatt_write_next_cccd() [when all dsc discoveries done]
 *     -> on_cccd_write()              [sequential CCCD writes using discovered handle]
 *     -> gopro_readiness_start()      [when all CCCDs written]
 *
 * Why descriptor discovery is mandatory: the BLE spec does NOT guarantee that
 * a characteristic's CCCD (0x2902) sits at val_handle + 1.  Other descriptors
 * — most notably the writable Characteristic User Description (0x2901) — can
 * legally appear first.  Writing the 2-byte notify-enable value to a 0x2901
 * succeeds at the ATT layer but does NOT enable notifications, so the camera
 * silently never sends data and readiness polling times out.  Always querying
 * the GATT descriptor layout is the only way to be correct across firmwares.
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

/* Forward declarations — defined later in this file. */
static void gopro_gatt_write_next_cccd(gopro_ble_ctx_t *ctx);
static void gopro_gatt_disc_next_dscs(gopro_ble_ctx_t *ctx);
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
 *
 * end_handle is the inclusive upper bound for this characteristic's descriptor
 * range — the next characteristic's def_handle minus one, or the containing
 * service's end_handle for the last chr in a service.  It's seeded with the
 * service end during on_chr_disc and patched down when the next chr arrives.
 *
 * cccd_handle is the handle of the 0x2902 descriptor discovered for this chr,
 * populated by on_dsc_disc.  Zero means "not discovered" — the CCCD writer
 * MUST treat zero as "skip" rather than writing to a wild handle.
 */
#define GATT_MAX_NOTIFY_CHRS  16

typedef struct {
    uint16_t val_handle;
    uint16_t end_handle;
    uint16_t cccd_handle;
    uint16_t cccd_val;
} notify_chr_t;

static notify_chr_t s_notify_list[CAMERA_MAX_SLOTS][GATT_MAX_NOTIFY_CHRS];
static int          s_notify_count[CAMERA_MAX_SLOTS];
static int          s_dsc_cur[CAMERA_MAX_SLOTS];
static int          s_subscr_cur[CAMERA_MAX_SLOTS];

/*
 * Per-slot index of the most recently added notify chr in the service being
 * scanned, or -1 if none yet (or already patched).  Used to backfill
 * end_handle when the next chr arrives.  Reset before each service.
 */
static int s_last_notify_in_svc[CAMERA_MAX_SLOTS];

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

    /* Patch the previous notify chr's descriptor-range upper bound: a new
     * chr's def_handle marks the end of the previous chr's descriptors.  Done
     * once per notify chr — clearing the index after patching prevents the
     * next non-notify chr from overwriting the bound past the correct edge. */
    if (s_last_notify_in_svc[ctx->slot] >= 0) {
        int prev = s_last_notify_in_svc[ctx->slot];
        if (chr->def_handle > 0) {
            s_notify_list[ctx->slot][prev].end_handle = chr->def_handle - 1;
        }
        s_last_notify_in_svc[ctx->slot] = -1;
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
        uint16_t svc_end = s_svc_list[ctx->slot][s_svc_cur[ctx->slot]].end;
        s_notify_list[ctx->slot][n].val_handle  = chr->val_handle;
        s_notify_list[ctx->slot][n].end_handle  = svc_end;    /* patched on next chr arrival */
        s_notify_list[ctx->slot][n].cccd_handle = 0;          /* discovered later */
        s_notify_list[ctx->slot][n].cccd_val    = has_notify ? BLE_CCCD_NOTIFY : BLE_CCCD_INDICATE;
        s_last_notify_in_svc[ctx->slot] = n;
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
        /* All services scanned — check for missing handles then run descriptor
         * discovery to locate each notify chr's CCCD. */
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

        s_dsc_cur[slot] = 0;
        gopro_gatt_disc_next_dscs(ctx);
        return;
    }

    /* Starting a new service — clear the patch pointer so the first chr of
     * this service doesn't back-patch a notify chr from the previous service. */
    s_last_notify_in_svc[slot] = -1;

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

/* ---- Descriptor discovery (locate each notify chr's CCCD) ---------------- */

/*
 * Called once per descriptor returned by ble_gattc_disc_all_dscs() for the
 * notify chr at s_dsc_cur[slot].  We keep the FIRST 0x2902 we see; because
 * end_handle was computed precisely (bounded by the next chr's def_handle
 * minus one), no foreign-chr CCCD can land in this range.  Subsequent
 * descriptors are ignored until EDONE, at which point we advance.
 */
static int on_dsc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                        uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                        void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;
    int slot = ctx->slot;
    int cur  = s_dsc_cur[slot];

    if (error->status == BLE_HS_EDONE) {
        if (s_notify_list[slot][cur].cccd_handle == 0) {
            ESP_LOGW(TAG, "slot %d: no CCCD descriptor found in (0x%04x, 0x%04x] "
                          "— this notify chr will not be subscribed",
                     slot,
                     s_notify_list[slot][cur].val_handle,
                     s_notify_list[slot][cur].end_handle);
        }
        s_dsc_cur[slot]++;
        gopro_gatt_disc_next_dscs(ctx);
        return 0;
    }

    if (error->status != 0) {
        ESP_LOGW(TAG, "slot %d: dsc discovery error 0x%04x for val_handle=0x%04x "
                      "— skipping this chr",
                 slot, error->status, s_notify_list[slot][cur].val_handle);
        s_dsc_cur[slot]++;
        gopro_gatt_disc_next_dscs(ctx);
        return 0;
    }

    if (s_notify_list[slot][cur].cccd_handle == 0 &&
        dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
        ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        s_notify_list[slot][cur].cccd_handle = dsc->handle;
        ESP_LOGD(TAG, "slot %d: CCCD found val_handle=0x%04x cccd_handle=0x%04x",
                 slot, s_notify_list[slot][cur].val_handle, dsc->handle);
    }

    return 0;
}

static void gopro_gatt_disc_next_dscs(gopro_ble_ctx_t *ctx)
{
    int slot = ctx->slot;
    int cur  = s_dsc_cur[slot];

    if (cur >= s_notify_count[slot]) {
        ESP_LOGI(TAG, "slot %d: descriptor discovery done", slot);
        s_subscr_cur[slot] = 0;
        gopro_gatt_write_next_cccd(ctx);
        return;
    }

    uint16_t val_handle = s_notify_list[slot][cur].val_handle;
    uint16_t end_handle = s_notify_list[slot][cur].end_handle;

    /* Defensive: a notify chr whose end_handle was never patched would
     * still hold svc_end — fine in itself, but if val_handle is the very
     * last attribute (no descriptors at all), NimBLE returns immediately
     * with EDONE and we move on with cccd_handle = 0. */
    if (end_handle <= val_handle) {
        ESP_LOGW(TAG, "slot %d: empty descriptor range for val_handle=0x%04x "
                      "(end=0x%04x) — skipping", slot, val_handle, end_handle);
        s_dsc_cur[slot]++;
        gopro_gatt_disc_next_dscs(ctx);
        return;
    }

    int rc = ble_gattc_disc_all_dscs(ctx->conn_handle, val_handle, end_handle,
                                      on_dsc_disc, ctx);
    if (rc != 0) {
        ESP_LOGW(TAG, "slot %d: ble_gattc_disc_all_dscs rc=%d val_handle=0x%04x "
                      "— skipping this chr", slot, rc, val_handle);
        s_dsc_cur[slot]++;
        gopro_gatt_disc_next_dscs(ctx);
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
    uint16_t cccd_handle = s_notify_list[ctx->slot][cur].cccd_handle;
    uint16_t cccd_val    = s_notify_list[ctx->slot][cur].cccd_val;

    if (cccd_handle == 0) {
        /* Descriptor discovery did not find a 0x2902 for this chr — do NOT
         * write to a guessed handle (e.g. val_handle+1 may be a writable
         * Characteristic User Description, which would accept the write
         * silently without enabling notifications).  If this chr is required
         * for readiness, the higher-level polling will time out and surface
         * the failure; better than a phantom "subscribed" state. */
        ESP_LOGW(TAG, "slot %d: skipping CCCD write — no descriptor for val_handle=0x%04x",
                 ctx->slot, val_handle);
        s_subscr_cur[ctx->slot]++;
        gopro_gatt_write_next_cccd(ctx);
        return;
    }

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
    s_notify_count[ctx->slot]       = 0;
    s_dsc_cur[ctx->slot]            = 0;
    s_subscr_cur[ctx->slot]         = 0;
    s_last_notify_in_svc[ctx->slot] = -1;

    ESP_LOGI(TAG, "slot %d: starting service discovery", ctx->slot);

    int rc = ble_gattc_disc_all_svcs(ctx->conn_handle, on_svc_disc, ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "slot %d: ble_gattc_disc_all_svcs rc=%d", ctx->slot, rc);
        ble_gap_terminate(ctx->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}
