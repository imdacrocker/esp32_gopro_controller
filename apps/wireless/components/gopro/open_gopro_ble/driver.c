/*
 * driver.c — Component init, per-slot context table, discovery list, and
 * the camera_driver_t vtable for BLE-control GoPro models.
 *
 * Threading:
 *   - drv_start_recording / drv_stop_recording / get_recording_status:
 *     called from camera_manager mismatch poll task / web UI / mismatch
 *     timer.  All forward to gopro_control_send_shutter() which posts a
 *     GATT write through ble_core (NimBLE host task).  cached_status is a
 *     single-enum atomic read.
 *   - on_wifi_disconnected vtable entry: NULL — BLE-control cameras never
 *     associate to the SoftAP.
 */

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "open_gopro_ble_internal.h"
#include "camera_manager_ble.h"   /* camera_manager_is_known_ble_addr */
#include "ble_core.h"
#include "gopro_model.h"
#include "shutdown_manager.h"

static const char *TAG = "gopro_ble";

/* ---- Per-slot context table ---------------------------------------------- */

static gopro_ble_ctx_t s_ctx[CAMERA_MAX_SLOTS];

gopro_ble_ctx_t *gopro_ctx_by_slot(int slot)
{
    if (slot < 0 || slot >= CAMERA_MAX_SLOTS) {
        return NULL;
    }
    return &s_ctx[slot];
}

gopro_ble_ctx_t *gopro_ctx_by_conn(uint16_t conn_handle)
{
    if (conn_handle == GOPRO_CONN_NONE) {
        return NULL;
    }
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        if (s_ctx[i].conn_handle == conn_handle) {
            return &s_ctx[i];
        }
    }
    return NULL;
}

/* ---- Discovery list (§15.2) ---------------------------------------------- */

static SemaphoreHandle_t s_disc_mutex;
static gopro_device_t    s_disc_list[GOPRO_DISC_MAX];
static int               s_disc_count;

/* 16-bit GoPro service UUID, used to filter advertisements. */
static const ble_uuid16_t k_gopro_svc_uuid = BLE_UUID16_INIT(GOPRO_SVC_UUID16);

static void on_disc_cb(ble_addr_t addr, int8_t rssi,
                       const uint8_t *data, int len)
{
    /* Filter: advertisement must contain the GoPro 16-bit service UUID. */
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, data, len) != 0) {
        return;
    }

    bool found = false;
    for (int i = 0; i < fields.num_uuids16; i++) {
        if (ble_uuid_cmp(&fields.uuids16[i].u, &k_gopro_svc_uuid.u) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }

    /* Already known? Update RSSI. */
    xSemaphoreTake(s_disc_mutex, portMAX_DELAY);
    for (int i = 0; i < s_disc_count; i++) {
        if (memcmp(&s_disc_list[i].addr, &addr, sizeof(ble_addr_t)) == 0) {
            s_disc_list[i].rssi = rssi;
            xSemaphoreGive(s_disc_mutex);
            return;
        }
    }

    if (s_disc_count < GOPRO_DISC_MAX) {
        gopro_device_t *d = &s_disc_list[s_disc_count++];
        d->addr = addr;
        d->rssi = rssi;
        /* Best-effort name extraction from complete or shortened local name. */
        const uint8_t *name   = NULL;
        uint8_t        namelen = 0;
        if (fields.name_len > 0) {
            name    = fields.name;
            namelen = fields.name_len;
        }
        if (name && namelen > 0) {
            size_t copy = namelen < sizeof(d->name) - 1 ? namelen : sizeof(d->name) - 1;
            memcpy(d->name, name, copy);
            d->name[copy] = '\0';
        } else {
            snprintf(d->name, sizeof(d->name), "GoPro-%02X%02X",
                     addr.val[1], addr.val[0]);
        }
        ESP_LOGI(TAG, "disc: %s rssi=%d", d->name, rssi);
    }
    xSemaphoreGive(s_disc_mutex);
}

void open_gopro_ble_start_discovery(void)
{
    xSemaphoreTake(s_disc_mutex, portMAX_DELAY);
    s_disc_count = 0;
    memset(s_disc_list, 0, sizeof(s_disc_list));
    xSemaphoreGive(s_disc_mutex);

    ble_core_start_discovery(120000 /* 120 s */);
}

void open_gopro_ble_stop_discovery(void)
{
    ble_core_stop_discovery();
}

int open_gopro_ble_get_discovered(gopro_device_t *out, int max_count)
{
    xSemaphoreTake(s_disc_mutex, portMAX_DELAY);
    int n = s_disc_count < max_count ? s_disc_count : max_count;
    memcpy(out, s_disc_list, n * sizeof(gopro_device_t));
    xSemaphoreGive(s_disc_mutex);
    return n;
}

bool open_gopro_ble_lookup_disc_name(const uint8_t mac[6], char *out, size_t out_len)
{
    if (!out || out_len == 0) return false;
    bool found = false;
    xSemaphoreTake(s_disc_mutex, portMAX_DELAY);
    for (int i = 0; i < s_disc_count; i++) {
        if (memcmp(s_disc_list[i].addr.val, mac, 6) == 0) {
            snprintf(out, out_len, "%s", s_disc_list[i].name);
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_disc_mutex);
    return found;
}

/* ---- Connection ---------------------------------------------------------- */

void open_gopro_ble_connect_by_addr(const ble_addr_t *addr)
{
    ble_core_connect_by_addr(addr);
}

/* ---- Shutdown helper (docs/design/shutdown.md) -------------------------- */

void open_gopro_ble_terminate_slot(int slot)
{
    gopro_ble_ctx_t *ctx = gopro_ctx_by_slot(slot);
    if (!ctx) return;
    if (ctx->conn_handle == GOPRO_CONN_NONE) {
        ESP_LOGI(TAG, "slot %d: terminate skipped — not connected", slot);
        return;
    }
    int rc = ble_gap_terminate(ctx->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    ESP_LOGI(TAG, "slot %d: ble_gap_terminate rc=%d", slot, rc);
    /* gopro_on_disconnected will clear ctx->conn_handle when the controller
     * acknowledges the disconnect. */
}

/* ---- UTC sync (§15.5) ---------------------------------------------------- */

void open_gopro_ble_sync_time_all(void)
{
    ESP_LOGI(TAG, "time sync (BLE) requested for all connected slots");
    int considered = 0;
    int skipped_disconnected = 0;
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        gopro_ble_ctx_t *ctx = &s_ctx[i];
        if (!gopro_model_uses_ble_control(camera_manager_get_model(i))) continue;
        considered++;
        if (ctx->conn_handle == GOPRO_CONN_NONE) {
            ESP_LOGI(TAG, "slot %d: datetime (BLE) skipped — not connected", i);
            skipped_disconnected++;
            continue;
        }
        gopro_control_set_datetime(ctx);
        ctx->datetime_pending_utc = false;
    }
    ESP_LOGI(TAG, "sync_time_all (BLE): %d BLE slot(s) considered, %d disconnected",
             considered, skipped_disconnected);
}

/* ---- camera_driver_t vtable --------------------------------------------- */

static esp_err_t drv_start_recording(void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;
    if (gopro_control_send_shutter(ctx, true) != 0) {
        return ESP_FAIL;
    }
    /* Optimistically reflect the intent so the next mismatch poll cycle
     * observes the state we just commanded; the next status poll will
     * confirm or correct it. */
    ctx->cached_status = CAMERA_RECORDING_ACTIVE;
    return ESP_OK;
}

static esp_err_t drv_stop_recording(void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;
    if (gopro_control_send_shutter(ctx, false) != 0) {
        return ESP_FAIL;
    }
    ctx->cached_status = CAMERA_RECORDING_IDLE;
    return ESP_OK;
}

static camera_recording_status_t drv_get_recording_status(void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;
    return ctx->cached_status;
}

static esp_err_t drv_sleep(void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;
    if (gopro_control_send_sleep(ctx) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void drv_teardown(void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;
    gopro_status_poll_stop(ctx);
    gopro_keepalive_stop(ctx);
    gopro_readiness_cancel(ctx);
    ctx->cached_status = CAMERA_RECORDING_UNKNOWN;
    ESP_LOGI(TAG, "slot %d: teardown complete", ctx->slot);
}

static void drv_update_slot_index(void *arg, int new_slot)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;
    ctx->slot = new_slot;
}

static const camera_driver_t k_gopro_ble_driver = {
    .start_recording      = drv_start_recording,
    .stop_recording       = drv_stop_recording,
    .get_recording_status = drv_get_recording_status,
    .teardown             = drv_teardown,
    .update_slot_index    = drv_update_slot_index,
    .on_wifi_disconnected = NULL,
    .sleep                = drv_sleep,
};

static bool model_matches(camera_model_t model)
{
    return gopro_model_uses_ble_control(model);
}

static void *ctx_create(int slot)
{
    /* The context already exists in s_ctx[]; just hand back the pointer.
     * conn_handle / cached_status are zero-initialised in open_gopro_ble_init. */
    gopro_ble_ctx_t *ctx = &s_ctx[slot];
    ctx->slot          = slot;
    ctx->cached_status = CAMERA_RECORDING_UNKNOWN;
    return ctx;
}

/* ---- Component init (§15.9) ---------------------------------------------- */

/* Forward declarations for callbacks defined in pairing.c and notify.c */
extern void gopro_on_connected(uint16_t conn_handle, ble_addr_t addr);
extern void gopro_on_encrypted(uint16_t conn_handle, ble_addr_t addr);
extern void gopro_on_disconnected(uint16_t conn_handle, ble_addr_t addr,
                                   uint8_t reason);

void open_gopro_ble_init(void)
{
    /* Initialise context table */
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        memset(&s_ctx[i], 0, sizeof(gopro_ble_ctx_t));
        s_ctx[i].conn_handle   = GOPRO_CONN_NONE;
        s_ctx[i].slot          = i;
        s_ctx[i].cached_status = CAMERA_RECORDING_UNKNOWN;
    }

    s_disc_mutex = xSemaphoreCreateMutex();
    configASSERT(s_disc_mutex);

    /* Register BLE event callbacks */
    ble_core_callbacks_t cbs = {
        .on_disc                   = on_disc_cb,
        .on_connected              = gopro_on_connected,
        .on_encrypted              = gopro_on_encrypted,
        .on_disconnected           = gopro_on_disconnected,
        .on_notify_rx              = gopro_notify_rx,
        .is_known_addr             = camera_manager_is_known_ble_addr,
        .has_disconnected_cameras  = camera_manager_has_disconnected_cameras,
        .is_shutdown_active        = shutdown_manager_is_active,
    };
    ble_core_register_callbacks(&cbs);

    /* Register the camera_driver_t with camera_manager.  requires_ble=true:
     * BLE is the sole control transport for these models, so a slot without
     * an active BLE connection counts as "disconnected" for ble_core's
     * background reconnect gate. */
    ESP_ERROR_CHECK(
        camera_manager_register_driver(&k_gopro_ble_driver,
                                        model_matches,
                                        ctx_create,
                                        true /* requires_ble */));

    ESP_LOGI(TAG, "init OK");
}
