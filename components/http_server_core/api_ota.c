/*
 * api_ota.c — OTA endpoints for the main app (docs/design/ota.md §6).
 *
 * Endpoints (recovery's /api/ota/boot-main is recovery-only; main has its
 * own /reboot-recovery for the symmetric direction):
 *
 *   POST /api/ota/upload-app       — stream new app image into inactive OTA slot
 *   POST /api/ota/upload-ui        — stream new storage image
 *   POST /api/ota/commit           — set boot partition + reboot
 *   POST /api/ota/reboot-recovery  — switch boot to factory + reboot
 *   GET  /api/ota/channel          — report current + available channels
 *   POST /api/ota/channel          — persist new channel into NVS
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "ota_io.h"
#include "http_server_helpers.h"

static const char *TAG = "api_ota";

/* ---- session state ----------------------------------------------------- *
 * "Successful" = upload completed OR was SHA-skipped. Either gates /commit.
 * Cleared on app boot (statics start at 0).
 *
 * No mutex: ESP-IDF's httpd is a single-task select-based server (one
 * worker task, dispatches one handler at a time), so handler_upload_app /
 * handler_upload_ui / handler_commit can never run concurrently against
 * each other or against themselves.  Any client retry while a previous
 * upload is mid-stream just queues at the TCP layer; the second handler
 * invocation starts after the first returns.  The flags below are
 * therefore serialised by httpd's worker, not by a mutex.
 *
 * If the worker model ever changes (e.g. ESP-IDF gains a thread-pooled
 * httpd, or a second httpd_handle_t is started in the same process),
 * these statics must move under a mutex or become atomic.
 */
/*
 * Each flag is cleared to false at the top of its upload handler, before
 * *_writer_begin() erases the inactive slot, and set true only after the
 * upload completes (or is SHA-skipped).  This invariant matters: begin()
 * erases/invalidates the slot, so a re-upload that fails mid-stream must NOT
 * leave a stale "succeeded" flag pointing at a now-partial image — otherwise a
 * following /commit would boot-switch to a corrupt partition.  Resetting at
 * entry makes the flag always reflect the state of the slot as of the most
 * recent attempt.
 */
static bool s_app_uploaded;
static bool s_ui_uploaded;

#define UPLOAD_CHUNK_BYTES 2048

/* ---- helpers ----------------------------------------------------------- */

static esp_err_t send_status_json(httpd_req_t *req, const char *status,
                                   const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static bool parse_sha256_header(const char *s, char out[65])
{
    if (!s || strlen(s) != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        char c = s[i];
        if      (c >= '0' && c <= '9') out[i] = c;
        else if (c >= 'a' && c <= 'f') out[i] = c;
        else if (c >= 'A' && c <= 'F') out[i] = (char)(c + ('a' - 'A'));
        else return false;
    }
    out[64] = '\0';
    return true;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

static void schedule_reboot(void)
{
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
}

/* ---- streaming pump (shared between upload-app and upload-ui) ---------- *
 * Mirrors recovery_http.c::pump_body. Kept local to api_ota.c rather than
 * pushed into ota_io, since it's pure HTTP plumbing — ota_io shouldn't
 * depend on esp_http_server.
 */

typedef esp_err_t (*write_fn)(void *w, const void *data, size_t len);

static esp_err_t pump_body(httpd_req_t *req, void *writer, write_fn write)
{
    char *buf = malloc(UPLOAD_CHUNK_BYTES);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t want = remaining < UPLOAD_CHUNK_BYTES ? remaining : UPLOAD_CHUNK_BYTES;
        int n = httpd_req_recv(req, buf, want);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "recv failed (%d) at %zu remaining", n, remaining);
            free(buf);
            return ESP_FAIL;
        }
        if (writer) {
            esp_err_t err = write(writer, buf, (size_t)n);
            if (err != ESP_OK) { free(buf); return err; }
        }
        remaining -= (size_t)n;
    }
    free(buf);
    return ESP_OK;
}

static esp_err_t app_write_adapter(void *w, const void *data, size_t len)
{
    return ota_writer_write((ota_writer_t *)w, data, len);
}

static esp_err_t ui_write_adapter(void *w, const void *data, size_t len)
{
    return storage_writer_write((storage_writer_t *)w, data, len);
}

/* ---- header validation shared by both upload handlers ------------------ */

typedef struct {
    char   sha[65];
    size_t size;
} upload_hdrs_t;

static esp_err_t parse_upload_headers(httpd_req_t *req, upload_hdrs_t *out)
{
    char sha_hdr[80] = {0};
    char size_hdr[24] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Sha256", sha_hdr, sizeof(sha_hdr)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-Size",   size_hdr, sizeof(size_hdr)) != ESP_OK) {
        send_status_json(req, "400 Bad Request",
            "{\"error\":\"missing X-Sha256 or X-Size header\"}");
        return ESP_FAIL;
    }
    if (!parse_sha256_header(sha_hdr, out->sha)) {
        send_status_json(req, "400 Bad Request",
            "{\"error\":\"X-Sha256 must be 64 hex chars\"}");
        return ESP_FAIL;
    }
    out->size = (size_t)strtoul(size_hdr, NULL, 10);
    if (out->size == 0 || out->size != req->content_len) {
        send_status_json(req, "400 Bad Request",
            "{\"error\":\"X-Size mismatch with Content-Length\"}");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- POST /api/ota/upload-app ------------------------------------------ */

static esp_err_t handler_upload_app(httpd_req_t *req)
{
    upload_hdrs_t h;
    if (parse_upload_headers(req, &h) != ESP_OK) return ESP_OK; /* response already sent */

    /* Invalidate any prior success: ota_writer_begin() below erases the slot,
     * so from here on there is no valid app image until this attempt finishes. */
    s_app_uploaded = false;

    ota_writer_t *w = NULL;
    bool skipped = false;
    esp_err_t err = ota_writer_begin(h.sha, h.size, &w, &skipped);
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_status_json(req, "413 Payload Too Large",
            "{\"error\":\"image larger than partition\"}");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return send_status_json(req, "503 Service Unavailable",
            "{\"error\":\"no inactive OTA slot — partition table missing ota_1?\"}");
    }
    if (err != ESP_OK) {
        char body[96];
        snprintf(body, sizeof(body), "{\"error\":\"ota_writer_begin: %s\"}",
                 esp_err_to_name(err));
        return send_status_json(req, "500 Internal Server Error", body);
    }

    err = pump_body(req, w, app_write_adapter);
    if (err != ESP_OK) {
        if (w) ota_writer_abort(w);
        return send_status_json(req, "507 Insufficient Storage",
            "{\"error\":\"flash write failed\"}");
    }

    if (skipped) {
        s_app_uploaded = true;
        char body[160];
        snprintf(body, sizeof(body),
                 "{\"written\":0,\"sha256\":\"%s\",\"skipped\":true}", h.sha);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, body);
    }

    char actual_sha[65];
    err = ota_writer_finish(w, actual_sha);
    if (err == ESP_ERR_INVALID_CRC) {
        return send_status_json(req, "422 Unprocessable Entity",
            "{\"error\":\"SHA-256 mismatch\"}");
    }
    if (err != ESP_OK) {
        char body[96];
        snprintf(body, sizeof(body), "{\"error\":\"ota_writer_finish: %s\"}",
                 esp_err_to_name(err));
        return send_status_json(req, "507 Insufficient Storage", body);
    }

    s_app_uploaded = true;
    char body[200];
    snprintf(body, sizeof(body),
             "{\"written\":%u,\"sha256\":\"%s\",\"skipped\":false}",
             (unsigned)h.size, actual_sha);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* ---- POST /api/ota/upload-ui ------------------------------------------- */

static esp_err_t handler_upload_ui(httpd_req_t *req)
{
    upload_hdrs_t h;
    if (parse_upload_headers(req, &h) != ESP_OK) return ESP_OK;

    /* Invalidate any prior success: storage_writer_begin() below erases the
     * slot, so from here on there is no valid UI image until this attempt
     * finishes. */
    s_ui_uploaded = false;

    storage_writer_t *w = NULL;
    bool skipped = false;
    esp_err_t err = storage_writer_begin(h.sha, h.size, &w, &skipped);
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_status_json(req, "413 Payload Too Large",
            "{\"error\":\"image larger than partition\"}");
    }
    if (err != ESP_OK) {
        char body[96];
        snprintf(body, sizeof(body), "{\"error\":\"storage_writer_begin: %s\"}",
                 esp_err_to_name(err));
        return send_status_json(req, "500 Internal Server Error", body);
    }

    err = pump_body(req, w, ui_write_adapter);
    if (err != ESP_OK) {
        if (w) storage_writer_abort(w);
        return send_status_json(req, "507 Insufficient Storage",
            "{\"error\":\"flash write failed\"}");
    }

    if (skipped) {
        s_ui_uploaded = true;
        char body[160];
        snprintf(body, sizeof(body),
                 "{\"written\":0,\"sha256\":\"%s\",\"skipped\":true}", h.sha);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, body);
    }

    char actual_sha[65];
    err = storage_writer_finish(w, actual_sha);
    if (err == ESP_ERR_INVALID_CRC) {
        return send_status_json(req, "422 Unprocessable Entity",
            "{\"error\":\"SHA-256 mismatch\"}");
    }
    if (err != ESP_OK) {
        char body[96];
        snprintf(body, sizeof(body), "{\"error\":\"storage_writer_finish: %s\"}",
                 esp_err_to_name(err));
        return send_status_json(req, "507 Insufficient Storage", body);
    }

    s_ui_uploaded = true;
    char body[200];
    snprintf(body, sizeof(body),
             "{\"written\":%u,\"sha256\":\"%s\",\"skipped\":false}",
             (unsigned)h.size, actual_sha);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* ---- POST /api/ota/commit ---------------------------------------------- */

static esp_err_t handler_commit(httpd_req_t *req)
{
    if (!s_app_uploaded && !s_ui_uploaded) {
        return send_status_json(req, "409 Conflict",
            "{\"error\":\"no successful upload in this session\"}");
    }

    /* If only the UI was uploaded (no app change), there's nothing to
     * boot-switch to — UI is just a data partition, no reboot needed.
     * Reply OK; storage is already live for next page load. */
    if (!s_app_uploaded) {
        send_json(req, "{\"rebooting\":false,\"reason\":\"ui-only-update\"}");
        s_ui_uploaded = false;  /* clear so future commit isn't a no-op */
        return ESP_OK;
    }

    char label[17];
    esp_err_t err = ota_io_commit_pending(label);
    if (err != ESP_OK) {
        char body[96];
        snprintf(body, sizeof(body), "{\"error\":\"set_boot_partition: %s\"}",
                 esp_err_to_name(err));
        return send_status_json(req, "422 Unprocessable Entity", body);
    }

    char body[96];
    snprintf(body, sizeof(body),
             "{\"rebooting\":true,\"boot_partition\":\"%s\"}", label);
    send_json(req, body);
    schedule_reboot();
    return ESP_OK;
}

/* ---- POST /api/ota/reboot-recovery ------------------------------------- */

static esp_err_t handler_reboot_recovery(httpd_req_t *req)
{
    esp_err_t err = ota_io_set_boot_factory();
    if (err == ESP_ERR_NOT_FOUND) {
        return send_status_json(req, "503 Service Unavailable",
            "{\"error\":\"factory partition not found\"}");
    }
    if (err != ESP_OK) {
        char body[96];
        snprintf(body, sizeof(body), "{\"error\":\"set_boot_factory: %s\"}",
                 esp_err_to_name(err));
        return send_status_json(req, "500 Internal Server Error", body);
    }

    send_json(req, "{\"rebooting\":true,\"boot_partition\":\"factory\"}");
    schedule_reboot();
    return ESP_OK;
}

/* ---- GET /api/ota/channel ---------------------------------------------- */

static esp_err_t handler_get_channel(httpd_req_t *req)
{
    char current[OTA_IO_CHANNEL_MAX];
    ota_io_get_channel(current, sizeof(current));

    char body[128];
#if CONFIG_OTA_ALLOW_DEV_CHANNEL
    snprintf(body, sizeof(body),
             "{\"current\":\"%s\",\"available\":[\"stable\",\"beta\",\"dev\"]}",
             current);
#else
    snprintf(body, sizeof(body),
             "{\"current\":\"%s\",\"available\":[\"stable\",\"beta\"]}",
             current);
#endif
    send_json(req, body);
    return ESP_OK;
}

/* ---- POST /api/ota/channel --------------------------------------------- */

static esp_err_t handler_post_channel(httpd_req_t *req)
{
    char buf[64];
    int n = read_body(req, buf, sizeof(buf));
    if (n < 0) return ESP_OK;  /* response sent */

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return send_status_json(req, "400 Bad Request",
            "{\"error\":\"invalid JSON\"}");
    }
    cJSON *ch = cJSON_GetObjectItem(root, "channel");
    if (!cJSON_IsString(ch) || !ota_io_channel_allowed(ch->valuestring)) {
        cJSON_Delete(root);
        return send_status_json(req, "400 Bad Request",
            "{\"error\":\"channel must be one of stable / beta"
#if CONFIG_OTA_ALLOW_DEV_CHANNEL
            " / dev"
#endif
            "\"}");
    }

    esp_err_t err = ota_io_set_channel(ch->valuestring);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        char body[96];
        snprintf(body, sizeof(body), "{\"error\":\"nvs: %s\"}",
                 esp_err_to_name(err));
        return send_status_json(req, "500 Internal Server Error", body);
    }

    char body[64];
    snprintf(body, sizeof(body), "{\"current\":\"%s\"}", ch->valuestring);
    cJSON_Delete(root);
    send_json(req, body);
    return ESP_OK;
}

/* ---- registration ------------------------------------------------------ */

void api_ota_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/ota/upload-app",      .method = HTTP_POST, .handler = handler_upload_app      },
        { .uri = "/api/ota/upload-ui",       .method = HTTP_POST, .handler = handler_upload_ui       },
        { .uri = "/api/ota/commit",          .method = HTTP_POST, .handler = handler_commit          },
        { .uri = "/api/ota/reboot-recovery", .method = HTTP_POST, .handler = handler_reboot_recovery },
        { .uri = "/api/ota/channel",         .method = HTTP_GET,  .handler = handler_get_channel     },
        { .uri = "/api/ota/channel",         .method = HTTP_POST, .handler = handler_post_channel    },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s: %s", routes[i].uri, esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "registered %zu URIs", sizeof(routes) / sizeof(routes[0]));
}
