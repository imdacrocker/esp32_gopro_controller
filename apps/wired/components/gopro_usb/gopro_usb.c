/*
 * gopro_usb.c — Open GoPro wired (USB) camera driver.  See gopro_usb.h.
 *
 * Threading model (mirrors gopro_wifi_rc):
 *   - Driver vtable start/stop/get_status are invoked under the cam_core lock
 *     and MUST NOT block: start/stop post to s_queue; get_status reads a cached
 *     enum.  All blocking HTTP runs on s_worker.
 *   - gopro_usb_on_link_up/down and sync_time_all post to s_queue too, so the
 *     worker is the single owner of the camera HTTP session and of
 *     cam_core_slot_set_ready (which must not run from a vtable callback).
 */

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "cam_core.h"
#include "camera_types.h"
#include "gopro_model.h"
#include "can_manager.h"
#include "gopro_usb.h"

static const char *TAG = "gopro_usb";

/* ---- Tunables ------------------------------------------------------------ */

#define GOPRO_USB_SLOT            0
#define GOPRO_HTTP_PORT           8080
#define HTTP_TIMEOUT_MS           5000
#define STATUS_POLL_INTERVAL_MS   3000   /* also serves as the keep-alive tick */
#define STATE_BUF_BYTES           12288  /* full Open GoPro state JSON is several KB */
#define INFO_BUF_BYTES            1024
#define WORKER_STACK_BYTES        6144
#define WORKER_PRIORITY           5
#define WORKER_CORE               0
#define QUEUE_DEPTH               8

/* Open GoPro status field IDs (camera state JSON "status" object). */
#define GP_STATUS_ENCODING        "10"   /* 1 while recording */

/* ---- Command queue ------------------------------------------------------- */

typedef enum {
    CMD_LINK_UP = 0,
    CMD_LINK_DOWN,
    CMD_START,
    CMD_STOP,
    CMD_STATUS_POLL,
    CMD_SYNC_TIME,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint32_t   ip;     /* CMD_LINK_UP only (network byte order) */
} usb_cmd_t;

/* ---- Per-slot context ---------------------------------------------------- */

typedef struct {
    volatile uint32_t                  camera_ip;   /* 0 when link down */
    volatile camera_recording_status_t cached;      /* read by drv_get_recording_status */
    camera_model_t                     model;       /* from /gopro/camera/info */
    char                               model_name[40]; /* camera-reported, from info */
} gopro_usb_ctx_t;

static gopro_usb_ctx_t s_ctx;
static cam_core_slot_t s_slot;                       /* embedded, zero-init at .bss */
static QueueHandle_t   s_queue;

/* ---- HTTP helper --------------------------------------------------------- *
 * Blocking GET against the camera.  Returns the HTTP status code, or -1 on a
 * transport failure.  If resp != NULL, up to resp_sz-1 body bytes are captured
 * and NUL-terminated. */
static int http_get(uint32_t ip_be, const char *path, char *resp, size_t resp_sz)
{
    /* ip_be is in network byte order; its 4 bytes are the dotted-quad octets. */
    const uint8_t *o = (const uint8_t *)&ip_be;
    char url[192];
    snprintf(url, sizeof(url), "http://%u.%u.%u.%u:%d%s",
             o[0], o[1], o[2], o[3], GOPRO_HTTP_PORT, path);

    esp_http_client_config_t cfg = {
        .url        = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http init failed");
        return -1;
    }

    int status = -1;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET %s open failed: %s", path, esp_err_to_name(err));
        goto out;
    }

    esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);

    if (resp && resp_sz) {
        int total = 0;
        while (total < (int)resp_sz - 1) {
            int r = esp_http_client_read(client, resp + total,
                                         (int)resp_sz - 1 - total);
            if (r <= 0) break;
            total += r;
        }
        resp[total] = '\0';
    }

out:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

/* ---- State parsing ------------------------------------------------------- */

static camera_recording_status_t parse_recording(const char *json)
{
    camera_recording_status_t out = CAMERA_RECORDING_UNKNOWN;
    cJSON *root = cJSON_Parse(json);
    if (!root) return out;
    cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (cJSON_IsObject(st)) {
        cJSON *enc = cJSON_GetObjectItemCaseSensitive(st, GP_STATUS_ENCODING);
        if (cJSON_IsNumber(enc)) {
            out = enc->valueint ? CAMERA_RECORDING_ACTIVE : CAMERA_RECORDING_IDLE;
        }
    }
    cJSON_Delete(root);
    return out;
}

/* ---- Worker command handlers (all run on s_worker) ----------------------- */

static void refresh_status(void)
{
    if (!s_ctx.camera_ip) return;
    char *buf = malloc(STATE_BUF_BYTES);
    if (!buf) return;
    int status = http_get(s_ctx.camera_ip, "/gopro/camera/state", buf, STATE_BUF_BYTES);
    if (status == 200) {
        camera_recording_status_t rs = parse_recording(buf);
        if (rs != CAMERA_RECORDING_UNKNOWN) {
            s_ctx.cached = rs;
        }
    }
    free(buf);
}

static void handle_link_up(uint32_t ip)
{
    s_ctx.camera_ip = ip;
    s_ctx.cached    = CAMERA_RECORDING_UNKNOWN;

    /* 1. Take control of the camera over the wired link. */
    int rc = http_get(ip, "/gopro/camera/control/wired_usb?p=1", NULL, 0);
    ESP_LOGI(TAG, "enable wired control -> %d", rc);

    /* 2. Identify the model (best-effort) and warn if it's not USB-capable. */
    char *info = malloc(INFO_BUF_BYTES);
    if (info) {
        if (http_get(ip, "/gopro/camera/info", info, INFO_BUF_BYTES) == 200) {
            cJSON *root = cJSON_Parse(info);
            cJSON *infoo = root ? cJSON_GetObjectItemCaseSensitive(root, "info") : NULL;
            cJSON *mn = infoo ? cJSON_GetObjectItemCaseSensitive(infoo, "model_number") : NULL;
            cJSON *nm = infoo ? cJSON_GetObjectItemCaseSensitive(infoo, "model_name")   : NULL;
            if (cJSON_IsNumber(mn)) {
                s_ctx.model = (camera_model_t)mn->valueint;
            }
            if (nm && cJSON_IsString(nm)) {
                strlcpy(s_ctx.model_name, nm->valuestring, sizeof(s_ctx.model_name));
            }
            ESP_LOGI(TAG, "camera model=%d (%s)", (int)s_ctx.model,
                     s_ctx.model_name[0] ? s_ctx.model_name : "?");
            if (!gopro_model_uses_usb_control(s_ctx.model)) {
                ESP_LOGW(TAG, "model %d is not on the USB-control allowlist "
                              "(Hero10+); proceeding anyway", (int)s_ctx.model);
            }
            cJSON_Delete(root);
        }
        free(info);
    }

    /* 3. Seed the recording-status cache from a state probe. */
    refresh_status();

    /* 4. Mark the slot ready so cam_core dispatches recording intent + the CAN
     *    status frame reports IDLE/RECORDING.  Safe here — we are NOT inside a
     *    vtable callback. */
    cam_core_slot_set_ready(GOPRO_USB_SLOT, true);
    ESP_LOGI(TAG, "slot %d ready", GOPRO_USB_SLOT);
}

static void handle_link_down(void)
{
    cam_core_slot_set_ready(GOPRO_USB_SLOT, false);
    s_ctx.camera_ip     = 0;
    s_ctx.cached        = CAMERA_RECORDING_UNKNOWN;
    s_ctx.model         = CAMERA_MODEL_UNKNOWN;
    s_ctx.model_name[0] = '\0';
    ESP_LOGI(TAG, "slot %d not ready (link down)", GOPRO_USB_SLOT);
}

static void handle_shutter(bool start)
{
    if (!s_ctx.camera_ip) return;
    const char *path = start ? "/gopro/camera/shutter/start"
                             : "/gopro/camera/shutter/stop";
    int rc = http_get(s_ctx.camera_ip, path, NULL, 0);
    ESP_LOGI(TAG, "shutter %s -> %d", start ? "start" : "stop", rc);
    /* Confirm/correct the optimistic cache the vtable set. */
    refresh_status();
}

static void handle_sync_time(void)
{
    if (!s_ctx.camera_ip) {
        ESP_LOGI(TAG, "sync_time skipped — link down");
        return;
    }
    uint64_t utc_ms;
    if (!can_manager_get_utc_ms(&utc_ms)) {
        ESP_LOGI(TAG, "sync_time skipped — no UTC anchor");
        return;
    }
    time_t   local = (time_t)(utc_ms / 1000) + (time_t)can_manager_get_tz_offset() * 3600;
    struct tm tmv;
    gmtime_r(&local, &tmv);

    char path[128];
    snprintf(path, sizeof(path),
             "/gopro/camera/set_date_time?date=%04d_%02d_%02d&time=%02d_%02d_%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    int rc = http_get(s_ctx.camera_ip, path, NULL, 0);
    ESP_LOGI(TAG, "set_date_time -> %d", rc);
}

static void worker_task(void *arg)
{
    (void)arg;
    usb_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd.type) {
        case CMD_LINK_UP:     handle_link_up(cmd.ip);  break;
        case CMD_LINK_DOWN:   handle_link_down();      break;
        case CMD_START:       handle_shutter(true);    break;
        case CMD_STOP:        handle_shutter(false);   break;
        case CMD_SYNC_TIME:   handle_sync_time();      break;
        case CMD_STATUS_POLL:
            if (s_ctx.camera_ip) {
                http_get(s_ctx.camera_ip, "/gopro/camera/keep_alive", NULL, 0);
                refresh_status();
            }
            break;
        }
    }
}

static void post(cmd_type_t type, uint32_t ip)
{
    usb_cmd_t cmd = { .type = type, .ip = ip };
    if (s_queue) xQueueSend(s_queue, &cmd, 0);
}

/* ---- Periodic status poll ------------------------------------------------ */

static void status_poll_timer_cb(void *arg)
{
    (void)arg;
    post(CMD_STATUS_POLL, 0);
}

/* ---- Driver vtable (invoked under the cam_core lock — never block) -------- */

static esp_err_t drv_start_recording(void *arg)
{
    gopro_usb_ctx_t *ctx = (gopro_usb_ctx_t *)arg;
    if (!ctx->camera_ip) return ESP_ERR_INVALID_STATE;
    ctx->cached = CAMERA_RECORDING_ACTIVE;   /* optimistic; refreshed by worker */
    post(CMD_START, 0);
    return ESP_OK;
}

static esp_err_t drv_stop_recording(void *arg)
{
    gopro_usb_ctx_t *ctx = (gopro_usb_ctx_t *)arg;
    if (!ctx->camera_ip) return ESP_ERR_INVALID_STATE;
    ctx->cached = CAMERA_RECORDING_IDLE;
    post(CMD_STOP, 0);
    return ESP_OK;
}

static camera_recording_status_t drv_get_recording_status(void *arg)
{
    gopro_usb_ctx_t *ctx = (gopro_usb_ctx_t *)arg;
    return ctx->cached;
}

static void drv_teardown(void *arg)
{
    gopro_usb_ctx_t *ctx = (gopro_usb_ctx_t *)arg;
    ctx->cached = CAMERA_RECORDING_UNKNOWN;
}

/* sleep: Open GoPro's HTTP API has no reliable power-off; the camera is bus-
 * powered and loses power at system shutdown anyway.  Report NOT_SUPPORTED so
 * shutdown_manager budgets it as a no-op.  terminate_link is NULL — the USB
 * netif is owned by CherryUSB, not this driver. */

static const camera_driver_t k_gopro_usb_driver = {
    .start_recording      = drv_start_recording,
    .stop_recording       = drv_stop_recording,
    .get_recording_status = drv_get_recording_status,
    .teardown             = drv_teardown,
    .update_slot_index    = NULL,   /* fixed single slot */
    .on_wifi_disconnected = NULL,
    .broadcasts_to_all    = false,
    .start_recording_all  = NULL,
    .stop_recording_all   = NULL,
    .sleep                = NULL,
    .terminate_link       = NULL,
};

/* ---- Public API ---------------------------------------------------------- */

void gopro_usb_init(void)
{
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(usb_cmd_t));
    configASSERT(s_queue);

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cached = CAMERA_RECORDING_UNKNOWN;
    s_ctx.model  = CAMERA_MODEL_UNKNOWN;

    /* Register the single slot with cam_core and attach our driver.  The slot
     * starts not-ready; handle_link_up flips it ready once the camera answers. */
    ESP_ERROR_CHECK(cam_core_register_slot(GOPRO_USB_SLOT, &s_slot));
    ESP_ERROR_CHECK(cam_core_slot_attach_driver(GOPRO_USB_SLOT, &k_gopro_usb_driver,
                                                &s_ctx, false /* not BLE */));

    BaseType_t ok = xTaskCreatePinnedToCore(
        worker_task, "gopro_usb",
        WORKER_STACK_BYTES / sizeof(StackType_t),
        NULL, WORKER_PRIORITY, NULL, WORKER_CORE);
    configASSERT(ok == pdPASS);

    esp_timer_handle_t poll_timer;
    const esp_timer_create_args_t poll_args = {
        .callback = status_poll_timer_cb,
        .name     = "gopro_usb_poll",
    };
    ESP_ERROR_CHECK(esp_timer_create(&poll_args, &poll_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(poll_timer,
                                             (uint64_t)STATUS_POLL_INTERVAL_MS * 1000));

    ESP_LOGI(TAG, "init OK (slot %d registered)", GOPRO_USB_SLOT);
}

void gopro_usb_on_link_up(uint32_t camera_ip)
{
    post(CMD_LINK_UP, camera_ip);
}

void gopro_usb_on_link_down(void)
{
    post(CMD_LINK_DOWN, 0);
}

void gopro_usb_sync_time_all(void)
{
    post(CMD_SYNC_TIME, 0);
}

void gopro_usb_get_model_name(char *out, size_t out_len)
{
    if (!out || !out_len) return;
    /* Single writer (worker task); a cosmetic torn read is acceptable. */
    strlcpy(out, s_ctx.model_name, out_len);
}
