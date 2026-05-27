/*
 * driver.c — Component init, driver vtable, per-slot context table, and the
 * work + shutter task loops.
 *
 * Threading model (§17.4):
 *   - start_recording / stop_recording: post to s_shutter_queue (non-blocking).
 *   - Station callbacks (on_station_*): post to s_work_queue (non-blocking,
 *     called on the wifi_manager event task).
 *   - get_recording_status: direct ctx read — safe because recording_status is
 *     a single enum (≤ 4 bytes) written only by the work task on Xtensa LX7.
 *   - on_wifi_disconnected vtable entry is NULL.  RC cameras' connection
 *     lifecycle is driven entirely by the gopro_wifi_rc_on_station_* public API.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "camera_manager.h"
#include "gopro_model.h"
#include "gopro_wifi_rc.h"
#include "gopro_wifi_rc_internal.h"

static const char *TAG = "gopro_rc";

/* ---- Globals ------------------------------------------------------------- */

gopro_wifi_rc_ctx_t s_ctx[CAMERA_MAX_SLOTS];
QueueHandle_t       s_work_queue;
QueueHandle_t       s_shutter_queue;
int                 s_udp_sock = -1;

/* ---- Helper: resolve slot only for RC-emulation cameras ------------------ */

static int find_managed_slot(const uint8_t mac[6])
{
    int slot = camera_manager_find_by_mac(mac);
    if (slot < 0) return -1;
    if (!gopro_model_uses_rc_emulation(camera_manager_get_model(slot))) return -1;
    return slot;
}

/* ---- Work task (priority 5, core 0) -------------------------------------- */

static void rc_work_task(void *arg)
{
    (void)arg;
    rc_work_cmd_t cmd;
    while (1) {
        xQueueReceive(s_work_queue, &cmd, portMAX_DELAY);
        switch (cmd.type) {
        case RC_CMD_STATION_ASSOCIATED:
            rc_handle_station_associated(cmd.mac_only.mac);
            break;
        case RC_CMD_STATION_DHCP:
            rc_handle_station_dhcp(cmd.mac_ip.mac, cmd.mac_ip.ip);
            break;
        case RC_CMD_STATION_DISCONNECTED:
            rc_handle_station_disconnected(cmd.mac_only.mac);
            break;
        case RC_CMD_KEEPALIVE_TICK:
            rc_handle_keepalive_tick(cmd.slot_cmd.slot);
            break;
        case RC_CMD_WOL_RETRY:
            rc_handle_wol_retry(cmd.slot_cmd.slot);
            break;
        case RC_CMD_STATUS_POLL_ALL:
            rc_handle_status_poll_all();
            break;
        case RC_CMD_PROMOTE:
            rc_handle_promote(cmd.slot_cmd.slot);
            break;
        case RC_CMD_APPLY_CV:
            rc_handle_apply_cv(cmd.slot_cmd.slot);
            break;
        case RC_CMD_SYNC_TIME_ALL:
            rc_handle_sync_time_all();
            break;
        }
    }
}

/* ---- Global 5 s status-poll timer ---------------------------------------- */

static void status_poll_timer_cb(void *arg)
{
    (void)arg;
    rc_work_cmd_t cmd = { .type = RC_CMD_STATUS_POLL_ALL };
    xQueueSend(s_work_queue, &cmd, 0);
}

/* ---- Driver vtable ------------------------------------------------------- */

/* Per-slot unicast: used by the mismatch poll and set_desired_recording_slot.
 * Targets only this camera's IP — won't disturb peers that may already be
 * recording (a duplicate Start to a running Hero4 has been observed to flip
 * it off, hence the deliberate single-target path). */
static esp_err_t drv_start_recording(void *arg)
{
    gopro_wifi_rc_ctx_t *ctx = (gopro_wifi_rc_ctx_t *)arg;
    if (!ctx->last_ip) {
        ESP_LOGW(TAG, "slot %d: start_recording skipped — no IP", ctx->slot);
        return ESP_ERR_INVALID_STATE;
    }
    rc_shutter_cmd_t cmd = { .start = true, .ip = ctx->last_ip, .repeat = 1 };
    if (xQueueSend(s_shutter_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "start_recording: shutter queue full");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t drv_stop_recording(void *arg)
{
    gopro_wifi_rc_ctx_t *ctx = (gopro_wifi_rc_ctx_t *)arg;
    if (!ctx->last_ip) {
        ESP_LOGW(TAG, "slot %d: stop_recording skipped — no IP", ctx->slot);
        return ESP_ERR_INVALID_STATE;
    }
    rc_shutter_cmd_t cmd = { .start = false, .ip = ctx->last_ip, .repeat = 1 };
    if (xQueueSend(s_shutter_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "stop_recording: shutter queue full");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Broadcast: used by set_desired_recording_all.  One queued command fans out
 * to every RC camera on the AP via 255.255.255.255:8484. */
static esp_err_t drv_start_recording_all(void)
{
    rc_shutter_cmd_t cmd = {
        .start  = true,
        .ip     = 0xFFFFFFFFu,
        .repeat = RC_SHUTTER_BROADCAST_REPEAT,
    };
    if (xQueueSend(s_shutter_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "start_recording_all: shutter queue full");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t drv_stop_recording_all(void)
{
    rc_shutter_cmd_t cmd = {
        .start  = false,
        .ip     = 0xFFFFFFFFu,
        .repeat = RC_SHUTTER_BROADCAST_REPEAT,
    };
    if (xQueueSend(s_shutter_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "stop_recording_all: shutter queue full");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static camera_recording_status_t drv_get_recording_status(void *arg)
{
    gopro_wifi_rc_ctx_t *ctx = (gopro_wifi_rc_ctx_t *)arg;
    return ctx->recording_status;
}

static void drv_teardown(void *arg)
{
    gopro_wifi_rc_ctx_t *ctx = (gopro_wifi_rc_ctx_t *)arg;
    rc_disarm_keepalive_timer(ctx);
    rc_disarm_wol_retry_timer(ctx);
    if (ctx->keepalive_timer) {
        esp_timer_delete(ctx->keepalive_timer);
        ctx->keepalive_timer = NULL;
    }
    if (ctx->wol_retry_timer) {
        esp_timer_delete(ctx->wol_retry_timer);
        ctx->wol_retry_timer = NULL;
    }
    ctx->wifi_ready        = false;
    ctx->recording_status  = CAMERA_RECORDING_UNKNOWN;
    ESP_LOGI(TAG, "slot %d: teardown complete", ctx->slot);
}

static void drv_update_slot_index(void *arg, int new_slot)
{
    gopro_wifi_rc_ctx_t *ctx = (gopro_wifi_rc_ctx_t *)arg;
    ctx->slot = new_slot;
}

static esp_err_t drv_sleep(void *arg)
{
    /* rc_send_sleep blocks for up to RC_HTTP_TIMEOUT_MS on the HTTP GET, so
     * the caller (shutdown_manager_slot_task) must already be running on its
     * own per-slot FreeRTOS task — never on a NimBLE / wifi-event context. */
    gopro_wifi_rc_ctx_t *ctx = (gopro_wifi_rc_ctx_t *)arg;
    return rc_send_sleep(ctx->slot);
}

/* on_wifi_disconnected is NULL — RC cameras' lifecycle is driven entirely
 * via the gopro_wifi_rc_on_station_* public API. */

static const camera_driver_t k_gopro_rc_driver = {
    .start_recording      = drv_start_recording,
    .stop_recording       = drv_stop_recording,
    .get_recording_status = drv_get_recording_status,
    .teardown             = drv_teardown,
    .update_slot_index    = drv_update_slot_index,
    .on_wifi_disconnected = NULL,
    .broadcasts_to_all    = true,
    .start_recording_all  = drv_start_recording_all,
    .stop_recording_all   = drv_stop_recording_all,
    .sleep                = drv_sleep,
};

/* ---- Driver registration ------------------------------------------------- */

static bool model_matches(camera_model_t model)
{
    return gopro_model_uses_rc_emulation(model);
}

static void *ctx_create(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    memset(ctx, 0, sizeof(*ctx));
    ctx->slot             = slot;
    ctx->recording_status = CAMERA_RECORDING_UNKNOWN;
    /* Copy MAC now — it is already set in camera_manager by the time ctx_create
     * is called (camera_manager_set_model fires after register_new sets mac). */
    camera_slot_info_t info;
    if (camera_manager_get_slot_info(slot, &info) == ESP_OK) {
        memcpy(ctx->mac, info.mac, sizeof(ctx->mac));
    }
    return ctx;
}

/* ---- Public API ---------------------------------------------------------- */

void gopro_wifi_rc_on_station_associated(const uint8_t mac[6])
{
    rc_work_cmd_t cmd = { .type = RC_CMD_STATION_ASSOCIATED };
    memcpy(cmd.mac_only.mac, mac, 6);
    xQueueSend(s_work_queue, &cmd, 0);
}

void gopro_wifi_rc_on_station_dhcp(const uint8_t mac[6], uint32_t ip)
{
    rc_work_cmd_t cmd = { .type = RC_CMD_STATION_DHCP };
    memcpy(cmd.mac_ip.mac, mac, 6);
    cmd.mac_ip.ip = ip;
    xQueueSend(s_work_queue, &cmd, 0);
}

void gopro_wifi_rc_on_station_disassociated(const uint8_t mac[6])
{
    rc_work_cmd_t cmd = { .type = RC_CMD_STATION_DISCONNECTED };
    memcpy(cmd.mac_only.mac, mac, 6);
    xQueueSend(s_work_queue, &cmd, 0);
}

void gopro_wifi_rc_add_camera(const uint8_t mac[6], uint32_t ip)
{
    int slot = camera_manager_register_new(mac);
    if (slot < 0) {
        ESP_LOGE(TAG, "add_camera: no free slots");
        /* If a pair attempt is in flight for this MAC (web UI Add flow),
         * fail it so the modal surfaces "All slots in use" instead of
         * spinning until the watchdog fires. */
        if (pair_attempt_addr_matches(mac)) {
            pair_attempt_fail(PAIR_ERROR_SLOTS_FULL,
                              "All camera slots are in use");
        }
        return;
    }
    /* Register as the legacy/unidentified RC fallback.  The UDP `cv`
     * identify (sent below in the prime burst, retried by keepalive_tick
     * every 3 s while identify_attempted is false) will upgrade this to
     * the specific HERO4_BLACK / SILVER / HERO7 / etc. enum once the camera
     * answers — handled asynchronously by the RX task and rc_handle_apply_cv.
     * If the camera never answers `cv` (e.g. Hero3-class), the slot stays
     * LEGACY_RC and UDP control still works.
     * set_model assigns the gopro_wifi_rc driver and calls ctx_create. */
    camera_manager_set_model(slot, CAMERA_MODEL_GOPRO_HERO_LEGACY_RC);

    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    ctx->last_ip = ip;
    memcpy(ctx->mac, mac, 6);
    camera_manager_save_slot(slot);
    /* RC-emulation cameras have no multi-step pairing handshake — the user
     * picks them from the unmanaged-stations list and they're remembered
     * immediately.  Mark first_pair_complete so the WiFi status mapping
     * stays semantically clean. */
    camera_manager_mark_first_pair_complete(slot);

    /* Prime the camera with keepalive + status + camera-version so it
     * responds within ms instead of waiting up to 3-5 s for the next
     * scheduled tick.  cv gives us model + firmware over UDP — the RX task
     * decodes it and posts CMD_APPLY_CV, which sets the model on the slot.
     * CMD_PROMOTE fires on the first response of any kind; if cv has
     * already arrived by then, promote applies the model inline. */
    rc_send_keepalive(ip);
    rc_send_st(ip);
    rc_send_cv(ip);

    rc_arm_keepalive_timer(ctx);

    ESP_LOGI(TAG, "slot %d: add_camera mac=%02x:%02x:%02x:%02x:%02x:%02x",
             slot, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void gopro_wifi_rc_remove_camera(int slot)
{
    /* driver teardown handles timer cleanup; camera_manager removes the slot. */
    camera_manager_remove_slot(slot);
}

bool gopro_wifi_rc_is_managed_slot(int slot)
{
    return gopro_model_uses_rc_emulation(camera_manager_get_model(slot));
}

bool gopro_wifi_rc_is_managed_mac(const uint8_t mac[6])
{
    return find_managed_slot(mac) >= 0;
}

void gopro_wifi_rc_sync_time_all(void)
{
    ESP_LOGI(TAG, "time sync (HTTP) requested for all WiFi RC slots");
    rc_work_cmd_t cmd = { .type = RC_CMD_SYNC_TIME_ALL };
    xQueueSend(s_work_queue, &cmd, 0);
}

/* ---- Component init ------------------------------------------------------ */

void gopro_wifi_rc_init(void)
{
    s_work_queue    = xQueueCreate(RC_WORK_QUEUE_DEPTH,    sizeof(rc_work_cmd_t));
    s_shutter_queue = xQueueCreate(RC_SHUTTER_QUEUE_DEPTH, sizeof(rc_shutter_cmd_t));
    configASSERT(s_work_queue);
    configASSERT(s_shutter_queue);

    ESP_ERROR_CHECK(rc_udp_init());

    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(rc_work_task, "gopro_rc_work",
                                 RC_WORK_TASK_STACK_BYTES / sizeof(StackType_t),
                                 NULL, RC_WORK_TASK_PRIORITY, NULL, 0);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(rc_shutter_task, "gopro_rc_shutter",
                                 RC_SHUTTER_TASK_STACK_BYTES / sizeof(StackType_t),
                                 NULL, RC_SHUTTER_TASK_PRIORITY, NULL, 0);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(rc_udp_rx_task, "gopro_rc_udp_rx",
                                 RC_UDP_RX_TASK_STACK_BYTES / sizeof(StackType_t),
                                 NULL, RC_UDP_RX_TASK_PRIORITY, NULL, 0);
    configASSERT(ok == pdPASS);

    /* Global 5 s status-poll timer (iterates wifi_ready slots — empty at init). */
    esp_timer_handle_t poll_timer;
    const esp_timer_create_args_t poll_args = {
        .callback = status_poll_timer_cb,
        .name     = "rc_status_poll",
    };
    ESP_ERROR_CHECK(esp_timer_create(&poll_args, &poll_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(poll_timer,
                                             (uint64_t)RC_STATUS_POLL_INTERVAL_MS * 1000));

    ESP_ERROR_CHECK(
        camera_manager_register_driver(&k_gopro_rc_driver,
                                       model_matches,
                                       ctx_create,
                                       false /* RC cameras don't require BLE */));

    ESP_LOGI(TAG, "init OK");
}
