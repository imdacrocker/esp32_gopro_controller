/*
 * pair_complete.c — Legacy WiFi pair-complete orchestration (Hero6/7/8).
 *
 * After BLE pair + readiness, the camera bonds but does not register the
 * controller in its paired-apps list.  Without that registration the BLE
 * bond is volatile: commands work in real time but the camera UI shows
 * "no connection" and the bond is invalidated on power-cycle.  Registration
 * happens over HTTP on the camera's own WiFi AP at the legacy endpoint
 *
 *   GET http://10.5.5.9/gp/gpControl/command/wireless/pair/complete
 *       ?success=1&deviceName=<CONFIG_DEVICE_IDENTITY_NAME>
 *
 * Orchestration sequence (single one-shot task per pair):
 *
 *   READ_SSID       (BLE encrypted read GP-0002)
 *   READ_PASSWORD   (BLE encrypted read GP-0003)
 *   SET_WIFI_ON     (BLE TLV write cmd 0x17 = 1)
 *   WAIT_CAM_AP     (sleep to let the camera bring up its AP)
 *   STA_JOIN        (pause our SoftAP, switch to STA, connect to camera AP)
 *   HTTP_GET        (issue the wireless/pair/complete URL)
 *   STA_LEAVE       (always — leave radio back in AP mode)
 *   SET_WIFI_OFF    (BLE TLV write cmd 0x17 = 0; best-effort)
 *   COMPLETE        (mark first_pair_complete + advance pair_attempt)
 *
 * Any failure → terminate the BLE connection, fail the pair attempt with
 * PAIR_ERROR_PAIR_COMPLETE_FAIL, and remove the slot if it was registered
 * during this attempt (mirrors the frozen-model cleanup path in readiness.c).
 * No automatic retry — the user must re-initiate pairing.
 *
 * Concurrency: BLE reads/writes are issued from this task but their
 * callbacks fire on the NimBLE host task; we bridge via a single
 * binary-semaphore + scratch buffer.  Only one pair-complete may run at a
 * time (guarded by s_busy).
 */

#include <string.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "os/os_mbuf.h"

#include "sdkconfig.h"
#include "wifi_manager.h"
#include "open_gopro_ble_internal.h"
#include "open_gopro_ble.h"
#include "camera_manager.h"

static const char *TAG = "gopro_ble/pc";

#define PC_READ_TIMEOUT_MS       3000
/* Time we let the camera spend bringing up its WiFi AP after SetWifi(ON).
 * On a real Hero7 the AP takes ~6–10 s to start beaconing. */
#define PC_CAM_AP_SETTLE_MS      8000
#define PC_STA_JOIN_TIMEOUT_MS   15000
#define PC_STA_RETRY_COUNT       3
#define PC_STA_RETRY_DELAY_MS    3000
#define PC_HTTP_TIMEOUT_MS       5000

/* ---- BLE-read bridge ---------------------------------------------------- */

static SemaphoreHandle_t s_read_done;
static int               s_read_status;   /* 0 = OK, !=0 = ATT status */
static char              s_read_buf[33];
static uint16_t          s_read_len;

static int on_ble_read(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)arg;
    s_read_status = error->status;
    if (s_read_status == 0 && attr && attr->om) {
        uint16_t len  = OS_MBUF_PKTLEN(attr->om);
        uint16_t copy = len < sizeof(s_read_buf) - 1 ? len : sizeof(s_read_buf) - 1;
        memset(s_read_buf, 0, sizeof(s_read_buf));
        os_mbuf_copydata(attr->om, 0, copy, s_read_buf);
        s_read_len = copy;
    } else {
        s_read_len = 0;
        s_read_buf[0] = '\0';
    }
    xSemaphoreGive(s_read_done);
    return 0;
}

static esp_err_t read_handle_blocking(uint16_t conn_handle, uint16_t att_handle,
                                       char *out, size_t out_sz, const char *name)
{
    int rc = ble_gattc_read(conn_handle, att_handle, on_ble_read, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "%s read enqueue rc=%d", name, rc);
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_read_done, pdMS_TO_TICKS(PC_READ_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "%s read timed out", name);
        return ESP_ERR_TIMEOUT;
    }
    if (s_read_status != 0) {
        ESP_LOGE(TAG, "%s read failed status=0x%04x", name, s_read_status);
        return ESP_FAIL;
    }
    if (s_read_len == 0) {
        ESP_LOGE(TAG, "%s read returned 0 bytes", name);
        return ESP_FAIL;
    }
    strlcpy(out, s_read_buf, out_sz);
    return ESP_OK;
}

/* ---- URL encoding ------------------------------------------------------- */

/*
 * Percent-encode anything that isn't unreserved per RFC 3986 (ALPHA / DIGIT /
 * "-" / "." / "_" / "~").  Defensive — CONFIG_DEVICE_IDENTITY_NAME currently contains
 * a space which absolutely needs escaping; future identities might include
 * other punctuation.
 */
static void urlencode(const char *in, char *out, size_t out_sz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t oi = 0;
    for (size_t i = 0; in[i] != '\0' && oi + 3 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        bool unreserved = (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out[oi++] = (char)c;
        } else {
            out[oi++] = '%';
            out[oi++] = hex[(c >> 4) & 0x0F];
            out[oi++] = hex[c & 0x0F];
        }
    }
    out[oi] = '\0';
}

/* ---- Orchestration task ------------------------------------------------- */

static bool s_busy;

typedef struct {
    int      slot;
    uint16_t conn_handle;
} pc_args_t;

static void fail_and_cleanup(int slot, uint16_t conn_handle, const char *reason)
{
    ESP_LOGE(TAG, "slot %d: pair-complete failed: %s", slot, reason);

    /* No SetWifi(OFF) cleanup — we never turned the camera AP on. */

    pair_attempt_fail(PAIR_ERROR_PAIR_COMPLETE_FAIL, reason);
    if (conn_handle != GOPRO_CONN_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    camera_manager_remove_slot(slot);
}

static void pair_complete_task(void *arg)
{
    pc_args_t *args = (pc_args_t *)arg;
    int      slot        = args->slot;
    uint16_t conn_handle = args->conn_handle;
    free(args);

    ESP_LOGI(TAG, "slot %d: pair-complete orchestration starting", slot);

    gopro_ble_ctx_t *ctx = gopro_ctx_by_slot(slot);
    if (!ctx || ctx->conn_handle != conn_handle) {
        ESP_LOGW(TAG, "slot %d: BLE link gone before pair-complete started", slot);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    if (ctx->gatt.wifi_ap_ssid == 0 || ctx->gatt.wifi_ap_password == 0) {
        fail_and_cleanup(slot, conn_handle, "missing WiFi AP creds handles");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    /* 1. Read SSID */
    char ssid[33];
    if (read_handle_blocking(conn_handle, ctx->gatt.wifi_ap_ssid,
                              ssid, sizeof(ssid), "SSID") != ESP_OK) {
        fail_and_cleanup(slot, conn_handle, "SSID read failed");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "slot %d: read SSID=\"%s\"", slot, ssid);

    /* 2. Read password */
    char password[33];
    if (read_handle_blocking(conn_handle, ctx->gatt.wifi_ap_password,
                              password, sizeof(password), "password") != ESP_OK) {
        fail_and_cleanup(slot, conn_handle, "password read failed");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "slot %d: read password (%d bytes)", slot, (int)strlen(password));

    /* 3. SetWifi(ON) skipped — the camera's AP is already up after the BLE
     * session is established, and explicitly toggling it appears to push the
     * radio onto 5 GHz, which our ESP32 STA can't join.  Leaving the camera's
     * AP in whatever state it negotiated for itself. */

    /* 4. Pause our SoftAP, switch to STA, join the camera.  Retried — the
     * camera may not be beaconing yet on the first attempt even after the
     * settle delay above; each retry gives it another 15 s window.  If all
     * attempts miss, the camera is most likely advertising on 5 GHz (the
     * ESP32-S3 radio is 2.4 GHz only) and the user needs to set the
     * camera's Wi-Fi band manually. */
    uint32_t gw_ip = 0;
    wifi_mgr_err_t we = WIFI_MGR_ERR_INTERNAL;
    for (int attempt = 1; attempt <= PC_STA_RETRY_COUNT; attempt++) {
        we = wifi_manager_sta_join(ssid, password, &gw_ip,
                                    PC_STA_JOIN_TIMEOUT_MS);
        if (we == WIFI_MGR_OK && gw_ip != 0) {
            break;
        }
        ESP_LOGW(TAG, "slot %d: STA join attempt %d/%d failed (err=%d)",
                 slot, attempt, PC_STA_RETRY_COUNT, (int)we);
        if (attempt < PC_STA_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(PC_STA_RETRY_DELAY_MS));
        }
    }
    if (we != WIFI_MGR_OK || gw_ip == 0) {
        ESP_LOGE(TAG, "slot %d: STA join failed after %d attempts (last err=%d) — "
                       "check Hero7 Preferences -> Connections -> Wi-Fi Band is 2.4 GHz",
                 slot, PC_STA_RETRY_COUNT, (int)we);
        fail_and_cleanup(slot, conn_handle, "STA join failed (check Wi-Fi band on camera)");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    /* 5. Issue HTTP GET wireless/pair/complete?success=1&deviceName=... */
    char encoded_name[3 * sizeof(CONFIG_DEVICE_IDENTITY_NAME) + 1];
    urlencode(CONFIG_DEVICE_IDENTITY_NAME, encoded_name, sizeof(encoded_name));

    esp_ip4_addr_t gw = { .addr = gw_ip };
    char url[192];
    snprintf(url, sizeof(url),
             "http://" IPSTR "/gp/gpControl/command/wireless/pair/complete"
             "?success=1&deviceName=%s",
             IP2STR(&gw), encoded_name);
    ESP_LOGI(TAG, "slot %d: GET %s", slot, url);

    esp_http_client_config_t http_cfg = {
        .url             = url,
        .timeout_ms      = PC_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_err_t herr = esp_http_client_perform(client);
    /* Status is set as soon as headers parse, even if a later body error
     * fires.  Grab it unconditionally so partial-response cases (e.g.
     * GoPro's non-RFC-compliant chunked encoding) still tell us whether the
     * camera processed the request. */
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    /* 6. Leave STA, restore AP — regardless of HTTP outcome. */
    wifi_manager_sta_leave();

    /* Hero7's HTTP server returns malformed chunked responses that trip
     * esp_http_client with ESP_ERR_HTTP_INCOMPLETE_DATA.  By the time the
     * body is streaming back the camera has already processed the request
     * and registered the controller — whether we can parse the reply
     * doesn't change camera-side state.  Treat the partial-data error as
     * success when the response status is 2xx (or unset, which happens
     * when GoPro returns no status line we recognise). */
    bool http_ok = (herr == ESP_OK && status >= 200 && status < 300) ||
                   (herr == ESP_ERR_HTTP_INCOMPLETE_DATA &&
                    (status == 0 || (status >= 200 && status < 300)));

    if (!http_ok) {
        ESP_LOGE(TAG, "slot %d: pair-complete HTTP failed err=%s status=%d",
                 slot, esp_err_to_name(herr), status);
        fail_and_cleanup(slot, conn_handle, "wireless/pair/complete HTTP failed");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "slot %d: wireless/pair/complete OK (err=%s status=%d)",
             slot, esp_err_to_name(herr), status);

    /* SetWifi(OFF) cleanup skipped — we never turned the camera AP on (it
     * was already up), so we leave it in whatever state we found it. */

    /* 8. Persist + advance.  Mirrors the tail of complete_connection_sequence. */
    camera_manager_mark_first_pair_complete(slot);
    pair_attempt_advance(PAIR_ATTEMPT_SUCCESS);

    ESP_LOGI(TAG, "slot %d: pair-complete orchestration done", slot);
    s_busy = false;
    vTaskDelete(NULL);
}

/* ---- Public entry point ------------------------------------------------- */

void gopro_pair_complete_run(gopro_ble_ctx_t *ctx)
{
    if (!ctx || ctx->conn_handle == GOPRO_CONN_NONE) {
        ESP_LOGW(TAG, "pair_complete_run called without an active link");
        return;
    }
    if (s_busy) {
        ESP_LOGW(TAG, "slot %d: pair-complete already in progress, ignoring",
                 ctx->slot);
        return;
    }
    if (s_read_done == NULL) {
        s_read_done = xSemaphoreCreateBinary();
    }

    pc_args_t *args = malloc(sizeof(*args));
    if (!args) {
        ESP_LOGE(TAG, "slot %d: pair-complete malloc failed", ctx->slot);
        fail_and_cleanup(ctx->slot, ctx->conn_handle, "OOM");
        return;
    }
    args->slot        = ctx->slot;
    args->conn_handle = ctx->conn_handle;

    s_busy = true;
    BaseType_t ok = xTaskCreate(pair_complete_task, "gopro_pc", 5120,
                                 args, tskIDLE_PRIORITY + 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "slot %d: pair-complete task create failed", ctx->slot);
        free(args);
        s_busy = false;
        fail_and_cleanup(ctx->slot, ctx->conn_handle, "task create failed");
    }
}
