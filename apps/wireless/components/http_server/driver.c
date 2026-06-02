/*
 * driver.c — http_server init, LittleFS mount, and static asset handlers.
 *
 * §20 of docs/design/camera-manager.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "http_server_internal.h"

static const char *TAG = "http_server";

/* ---- Asset serving ------------------------------------------------------- */

/*
 * Read a file from LittleFS into a heap buffer.
 * Caller must free() the returned pointer.
 * Returns NULL on failure.
 */
static char *read_file(const char *path, long *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d (%s)",
                 path, errno, strerror(errno));
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "fseek(%s, END) failed: errno=%d", path, errno);
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        ESP_LOGE(TAG, "ftell(%s) failed: errno=%d", path, errno);
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek(%s, SET) failed: errno=%d", path, errno);
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)sz);
    if (!buf) {
        ESP_LOGE(TAG, "malloc(%ld) for %s failed: free_heap=%u largest_block=%u",
                 sz, path,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    if ((long)n != sz) {
        ESP_LOGE(TAG, "fread(%s) short read: got=%zu want=%ld errno=%d feof=%d ferror=%d",
                 path, n, sz, errno, feof(f), ferror(f));
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = sz;
    return buf;
}

/* Serve a file with explicit Content-Length (required for iOS Safari). */
static esp_err_t serve_file(httpd_req_t *req,
                             const char *path,
                             const char *content_type,
                             const char *encoding,    /* NULL if uncompressed */
                             const char *cache_ctrl)
{
    long sz = 0;
    char *buf = read_file(path, &sz);
    if (!buf) {
        /* Not programmed yet — return informative placeholder. */
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req,
            "<html><body><p>Web UI not flashed. "
            "Flash <code>web_ui.bin</code> to the storage partition.</p>"
            "</body></html>");
        return ESP_OK;
    }

    httpd_resp_set_type(req, content_type);
    if (encoding) {
        httpd_resp_set_hdr(req, "Content-Encoding", encoding);
    }
    if (cache_ctrl) {
        httpd_resp_set_hdr(req, "Cache-Control", cache_ctrl);
    }
    char len_str[16];
    snprintf(len_str, sizeof(len_str), "%ld", sz);
    httpd_resp_set_hdr(req, "Content-Length", len_str);

    httpd_resp_send(req, buf, sz);
    free(buf);
    return ESP_OK;
}

/* Check if the client's Accept-Encoding header includes "gzip". */
static bool client_accepts_gzip(httpd_req_t *req)
{
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding",
                                     hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    return strstr(hdr, "gzip") != NULL;
}

/* GET / and GET /index.html */
static esp_err_t handler_index(httpd_req_t *req)
{
    return serve_file(req, "/www/index.html", "text/html", NULL,
                      "no-cache");
}

/* GET /app.js — serve gzipped if client supports it */
static esp_err_t handler_app_js(httpd_req_t *req)
{
    if (client_accepts_gzip(req)) {
        return serve_file(req, "/www/app.js.gz",
                          "application/javascript", "gzip", "no-cache");
    }
    return serve_file(req, "/www/app.js",
                      "application/javascript", NULL, "no-cache");
}

/* GET /style.css — serve gzipped if client supports it */
static esp_err_t handler_style_css(httpd_req_t *req)
{
    if (client_accepts_gzip(req)) {
        return serve_file(req, "/www/style.css.gz",
                          "text/css", "gzip", "no-cache");
    }
    return serve_file(req, "/www/style.css",
                      "text/css", NULL, "no-cache");
}

/* GET /updates.js — serve gzipped if client supports it */
static esp_err_t handler_updates_js(httpd_req_t *req)
{
    if (client_accepts_gzip(req)) {
        return serve_file(req, "/www/updates.js.gz",
                          "application/javascript", "gzip", "no-cache");
    }
    return serve_file(req, "/www/updates.js",
                      "application/javascript", NULL, "no-cache");
}

/* ---- Health-check fallback ----------------------------------------------- *
 * If the web UI is missing from LittleFS the main app is functionally dead
 * to the user — no UI means no way to upload a new UI, no way to trigger
 * recovery from the gear menu. Reboot into factory (recovery) so the user
 * can re-flash via recovery's embedded HTML upload form.
 *
 * Triggers on a fresh device where LittleFS was wiped by `idf.py
 * erase-flash`, and on any future case where the storage partition gets
 * corrupted enough that format-then-mount yields an empty filesystem.
 *
 * See "Option B" in the recovery-fallback discussion alongside ota.md.
 */
static void reboot_to_factory(const char *reason)
{
    ESP_LOGE(TAG, "FALLBACK: %s — rebooting to factory (recovery)", reason);

    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) {
        ESP_LOGE(TAG, "factory partition not found; staying on main app");
        return;
    }

    esp_err_t e = esp_ota_set_boot_partition(factory);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s — staying on main app",
                 esp_err_to_name(e));
        return;
    }

    /* Let UART drain the log line before the restart cuts it off. */
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

/* ---- Component init ------------------------------------------------------ */

void http_server_init(void)
{
    /* Mount LittleFS at /www (§19.2). */
    esp_vfs_littlefs_conf_t lfs_conf = {
        .base_path              = "/www",
        .partition_label        = "storage",
        .format_if_mount_failed = true,   /* blank/corrupted partition → format on first boot */
        .dont_mount             = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&lfs_conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed (%s) — web UI unavailable",
                 esp_err_to_name(err));
        reboot_to_factory("LittleFS mount failed");
        /* If reboot_to_factory returns we have no UI and no way back; press on. */
    } else {
        ESP_LOGI(TAG, "LittleFS mounted at /www");

        /* Verify the web UI is actually present. After erase-flash + format,
         * the mount succeeds but the filesystem is empty. */
        struct stat st;
        if (stat("/www/index.html", &st) != 0 || st.st_size == 0) {
            reboot_to_factory("/www/index.html missing or empty");
        }
    }

    /* Start esp_httpd (§20.2).
     *
     * Default 5 s recv_wait_timeout / send_wait_timeout from HTTPD_DEFAULT_CONFIG
     * are retained — they bound any single recv/send stall.  lru_purge_enable
     * is the slow-trickle slowloris defence: when all 8 sockets are in use and
     * a 9th client connects, ESP-IDF evicts the least-recently-used socket.
     * Without LRU purge, a client sending one byte every 4 s could hold a
     * socket for 30+ minutes per request and 8 such clients would exhaust the
     * pool with no recourse.  Trade-off: a legitimate slow client could be
     * evicted under burst load, but this is a single-user controller so the
     * concurrent-fast-client scenario isn't realistic. */
    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = 12288;
    config.max_open_sockets  = 8;
    config.max_uri_handlers  = 44;   /* 5 assets + 7 system + 10 cameras + 2 rc + 7 settings + 6 ota + 3 logs + 2 shutdown = 42, +2 margin */
    config.uri_match_fn      = httpd_uri_match_wildcard;
    config.lru_purge_enable  = true;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    /* Static asset handlers. */
    httpd_uri_t uri_root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = handler_index,
    };
    httpd_uri_t uri_index = {
        .uri      = "/index.html",
        .method   = HTTP_GET,
        .handler  = handler_index,
    };
    httpd_uri_t uri_app_js = {
        .uri      = "/app.js",
        .method   = HTTP_GET,
        .handler  = handler_app_js,
    };
    httpd_uri_t uri_style_css = {
        .uri      = "/style.css",
        .method   = HTTP_GET,
        .handler  = handler_style_css,
    };
    httpd_uri_t uri_updates_js = {
        .uri      = "/updates.js",
        .method   = HTTP_GET,
        .handler  = handler_updates_js,
    };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_app_js);
    httpd_register_uri_handler(server, &uri_style_css);
    httpd_register_uri_handler(server, &uri_updates_js);

    /* API handler registration. */
    api_system_register(server);
    api_cameras_register(server);
    api_rc_register(server);
    api_settings_register(server);
    api_ota_register(server);
    api_logs_register(server);
    api_shutdown_register(server);

    ESP_LOGI(TAG, "HTTP server started");
}
