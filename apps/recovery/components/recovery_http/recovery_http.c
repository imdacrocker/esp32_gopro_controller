/*
 * recovery_http.c — minimal HTTP server for the recovery image.
 *
 * Endpoints (see docs/design/ota.md §6 for full contract):
 *   GET  /                        — embedded HTML
 *   GET  /api/version             — running firmware identity
 *   POST /api/ota/upload-app      — stream new app image into ota_0
 *   POST /api/ota/upload-ui       — stream new storage image
 *   POST /api/ota/commit          — set boot partition + reboot
 *   POST /api/ota/boot-main       — boot ota_0 without uploading anything
 *   POST /api/factory-reset       — erase NVS and reboot
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "sdkconfig.h"
#include "recovery_http.h"
#include "ota_io.h"

static const char *TAG = "recovery_http";

/* Streaming body chunk size — large enough that flash writes amortize the
 * recv overhead, small enough that we don't bloat the TCP stack. */
#define UPLOAD_CHUNK_BYTES 2048

/* Owned by main, lifetime > server. */
static const char *s_html;
static size_t      s_html_len;

/* Did at least one successful upload happen this session? gates /commit. */
static bool s_app_uploaded;
static bool s_ui_uploaded;

/* ---- helpers ------------------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_status_json(httpd_req_t *req, const char *status,
                                   const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* Validate that `s` is exactly 64 lowercase hex chars and copy into `out`. */
static bool parse_sha256_header(const char *s, char out[65])
{
    if (!s || strlen(s) != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            /* accept upper-case too, normalise */
            if (c >= 'A' && c <= 'F') out[i] = (char)(c + ('a' - 'A'));
            else return false;
        } else {
            out[i] = c;
        }
    }
    out[64] = '\0';
    return true;
}

/* Reboot helper — runs from a one-shot task so the HTTP response can flush. */
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

/* ---- GET / --------------------------------------------------------------- */

static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_html, s_html_len);
}

/* ---- GET /api/version ---------------------------------------------------- *
 *
 *   app:               recovery's own version (no main-app version is
 *                      readable from the running partition; we report
 *                      what's running, hence recovery's version)
 *   ui:                "none" — recovery's UI is embedded in the binary,
 *                      not a separate version
 *   recovery:          same as app field for clarity
 *   channel:           shared NVS ota/channel via ota_io
 *   mode:              "recovery"
 */
static esp_err_t handler_version(httpd_req_t *req)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    char channel[OTA_IO_CHANNEL_MAX];
    ota_io_get_channel(channel, sizeof(channel));

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"app\":\"%s\",\"ui\":\"none\",\"recovery\":\"%s\","
             "\"channel\":\"%s\",\"running_partition\":\"%s\","
             "\"mode\":\"recovery\","
             "\"ota_base_url\":\"%s\",\"ota_repo_path\":\"%s\"}",
             desc->version, desc->version, channel,
             running ? running->label : "unknown",
             CONFIG_OTA_BASE_URL, CONFIG_OTA_REPO_PATH);
    return send_json(req, buf);
}

/* ---- GET /api/ota/channel ----------------------------------------------- */

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
    return send_json(req, body);
}

/* ---- POST /api/ota/channel ---------------------------------------------- */

/* Read up to `cap` bytes of body into `buf`. Returns negative and emits an
 * error response on failure. */
static int read_body_capped(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len == 0 || req->content_len >= cap) {
        send_status_json(req, "400 Bad Request",
            "{\"error\":\"body too large or empty\"}");
        return -1;
    }
    int total = 0;
    while ((size_t)total < req->content_len) {
        int n = httpd_req_recv(req, buf + total, req->content_len - total);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            send_status_json(req, "400 Bad Request",
                "{\"error\":\"recv failed\"}");
            return -1;
        }
        total += n;
    }
    buf[total] = '\0';
    return total;
}

/* Extract a quoted string value for `key` from a flat JSON object.
 * Recovery only ever receives `{"channel":"<word>"}`, so we hand-roll
 * this to avoid pulling cJSON into recovery's binary (62 KB headroom).
 *
 * Writes the unescaped value to `out` (NUL-terminated). Returns true on
 * success. Tolerates whitespace around tokens but does NOT process JSON
 * escape sequences — fine because the only valid values are plain ASCII
 * channel names (stable / beta / dev).
 */
static bool extract_string_field(const char *json, const char *key,
                                  char *out, size_t out_len)
{
    /* Find "key" */
    size_t key_len = strlen(key);
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        if (strncmp(p + 1, key, key_len) == 0 && p[1 + key_len] == '"') {
            p += 2 + key_len;
            break;
        }
        p++;
    }
    if (!p) return false;

    /* Skip whitespace and the colon */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != ':') return false;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    /* Expect opening quote */
    if (*p++ != '"') return false;

    /* Copy until closing quote */
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) {
        out[i++] = *p++;
    }
    if (*p != '"') return false;
    out[i] = '\0';
    return true;
}

static esp_err_t handler_post_channel(httpd_req_t *req)
{
    char buf[64];
    if (read_body_capped(req, buf, sizeof(buf)) < 0) return ESP_OK;

    char channel[OTA_IO_CHANNEL_MAX];
    if (!extract_string_field(buf, "channel", channel, sizeof(channel)) ||
        !ota_io_channel_allowed(channel)) {
        return send_status_json(req, "400 Bad Request",
            "{\"error\":\"channel must be one of stable / beta"
#if CONFIG_OTA_ALLOW_DEV_CHANNEL
            " / dev"
#endif
            "\"}");
    }

    esp_err_t err = ota_io_set_channel(channel);
    if (err != ESP_OK) {
        char body[96];
        snprintf(body, sizeof(body), "{\"error\":\"nvs: %s\"}",
                 esp_err_to_name(err));
        return send_status_json(req, "500 Internal Server Error", body);
    }

    char body[64];
    snprintf(body, sizeof(body), "{\"current\":\"%s\"}", channel);
    return send_json(req, body);
}

/* ---- shared upload pipeline --------------------------------------------- */

typedef esp_err_t (*write_fn)(void *w, const void *data, size_t len);

/*
 * Generic streaming pipeline used by both upload-app and upload-ui.
 *
 * `writer` is opaque. Caller has already invoked the begin() that produced
 * it (writer == NULL means "skipped" per SHA-skip; we just drain the body
 * to keep the connection healthy).
 *
 * On return, if writer != NULL the caller is responsible for finish/abort.
 */
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

/* ---- POST /api/ota/upload-app ------------------------------------------- */

static esp_err_t handler_upload_app(httpd_req_t *req)
{
    char sha_hdr[80] = {0};
    char size_hdr[24] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Sha256", sha_hdr, sizeof(sha_hdr)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-Size",   size_hdr, sizeof(size_hdr)) != ESP_OK) {
        return send_status_json(req, "400 Bad Request",
            "{\"error\":\"missing X-Sha256 or X-Size header\"}");
    }

    char expected_sha[65];
    if (!parse_sha256_header(sha_hdr, expected_sha)) {
        return send_status_json(req, "400 Bad Request",
            "{\"error\":\"X-Sha256 must be 64 hex chars\"}");
    }
    size_t expected_size = (size_t)strtoul(size_hdr, NULL, 10);
    if (expected_size == 0 || expected_size != req->content_len) {
        return send_status_json(req, "400 Bad Request",
            "{\"error\":\"X-Size mismatch with Content-Length\"}");
    }

    ota_writer_t *w = NULL;
    bool skipped = false;
    esp_err_t err = ota_writer_begin(expected_sha, expected_size, &w, &skipped);
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_status_json(req, "413 Payload Too Large",
            "{\"error\":\"image larger than partition\"}");
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
                 "{\"written\":0,\"sha256\":\"%s\",\"skipped\":true}",
                 expected_sha);
        return send_json(req, body);
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
             (unsigned)expected_size, actual_sha);
    esp_err_t send_err = send_json(req, body);
    ESP_LOGI(TAG, "upload-app response: %s", esp_err_to_name(send_err));
    return send_err;
}

/* ---- POST /api/ota/upload-ui -------------------------------------------- */

static esp_err_t handler_upload_ui(httpd_req_t *req)
{
    char sha_hdr[80] = {0};
    char size_hdr[24] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Sha256", sha_hdr, sizeof(sha_hdr)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-Size",   size_hdr, sizeof(size_hdr)) != ESP_OK) {
        return send_status_json(req, "400 Bad Request",
            "{\"error\":\"missing X-Sha256 or X-Size header\"}");
    }

    char expected_sha[65];
    if (!parse_sha256_header(sha_hdr, expected_sha)) {
        return send_status_json(req, "400 Bad Request",
            "{\"error\":\"X-Sha256 must be 64 hex chars\"}");
    }
    size_t expected_size = (size_t)strtoul(size_hdr, NULL, 10);
    if (expected_size == 0 || expected_size != req->content_len) {
        return send_status_json(req, "400 Bad Request",
            "{\"error\":\"X-Size mismatch with Content-Length\"}");
    }

    storage_writer_t *w = NULL;
    bool skipped = false;
    esp_err_t err = storage_writer_begin(expected_sha, expected_size, &w, &skipped);
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
                 "{\"written\":0,\"sha256\":\"%s\",\"skipped\":true}",
                 expected_sha);
        return send_json(req, body);
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
             (unsigned)expected_size, actual_sha);
    esp_err_t send_err = send_json(req, body);
    ESP_LOGI(TAG, "upload-ui response: %s", esp_err_to_name(send_err));
    return send_err;
}

/* ---- POST /api/ota/commit ----------------------------------------------- */

static esp_err_t handler_commit(httpd_req_t *req)
{
    if (!s_app_uploaded && !s_ui_uploaded) {
        return send_status_json(req, "409 Conflict",
            "{\"error\":\"no successful upload in this session\"}");
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

/* ---- POST /api/ota/boot-main -------------------------------------------- */

static esp_err_t handler_boot_main(httpd_req_t *req)
{
    char label[17];
    esp_err_t err = ota_io_set_boot_main(label);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_status_json(req, "409 Conflict",
            "{\"error\":\"no OTA slot holds a valid app image\"}");
    }
    if (err != ESP_OK) {
        char body[96];
        snprintf(body, sizeof(body), "{\"error\":\"boot_main: %s\"}",
                 esp_err_to_name(err));
        return send_status_json(req, "500 Internal Server Error", body);
    }

    char body[96];
    snprintf(body, sizeof(body),
             "{\"rebooting\":true,\"boot_partition\":\"%s\"}", label);
    send_json(req, body);
    schedule_reboot();
    return ESP_OK;
}

/* ---- POST /api/factory-reset -------------------------------------------- */

static esp_err_t handler_factory_reset(httpd_req_t *req)
{
    ESP_LOGW(TAG, "factory reset — erasing NVS");
    send_json(req, "{}");
    nvs_flash_erase();
    schedule_reboot();
    return ESP_OK;
}

/* ---- registration -------------------------------------------------------- */

esp_err_t recovery_http_init(const char *html, size_t html_len)
{
    s_html     = html;
    s_html_len = html_len;
    s_app_uploaded = false;
    s_ui_uploaded  = false;

    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    /* Storage erase + write of a 3 MB image takes ~85 s, during which the
     * client is just waiting for our response. Generous timeouts so the
     * idle wait doesn't trip httpd's per-call recv/send guards. */
    cfg.recv_wait_timeout = 120;
    cfg.send_wait_timeout = 120;

    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",                     .method = HTTP_GET,  .handler = handler_root         },
        { .uri = "/api/version",          .method = HTTP_GET,  .handler = handler_version      },
        { .uri = "/api/ota/upload-app",   .method = HTTP_POST, .handler = handler_upload_app   },
        { .uri = "/api/ota/upload-ui",    .method = HTTP_POST, .handler = handler_upload_ui    },
        { .uri = "/api/ota/commit",       .method = HTTP_POST, .handler = handler_commit       },
        { .uri = "/api/ota/boot-main",    .method = HTTP_POST, .handler = handler_boot_main    },
        { .uri = "/api/ota/channel",      .method = HTTP_GET,  .handler = handler_get_channel  },
        { .uri = "/api/ota/channel",      .method = HTTP_POST, .handler = handler_post_channel },
        { .uri = "/api/factory-reset",    .method = HTTP_POST, .handler = handler_factory_reset},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "listening on :80 (%zu URIs registered)",
             sizeof(routes) / sizeof(routes[0]));
    return ESP_OK;
}
