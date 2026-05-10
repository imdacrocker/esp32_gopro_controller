/*
 * driver.c — http_server init, LittleFS mount, and static asset handlers.
 *
 * §20 of camera_manager_design.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_http_server.h"
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
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if ((long)fread(buf, 1, (size_t)sz, f) != sz) {
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
    } else {
        ESP_LOGI(TAG, "LittleFS mounted at /www");
    }

    /* Start esp_httpd (§20.2). */
    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = 12288;
    config.max_open_sockets  = 8;
    config.max_uri_handlers  = 35;   /* 4 assets + 7 system + 10 cameras + 2 rc + 3 settings + 6 ota = 32, +3 margin */
    config.uri_match_fn      = httpd_uri_match_wildcard;

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
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_app_js);
    httpd_register_uri_handler(server, &uri_style_css);

    /* API handler registration. */
    api_system_register(server);
    api_cameras_register(server);
    api_rc_register(server);
    api_settings_register(server);
    api_ota_register(server);

    ESP_LOGI(TAG, "HTTP server started");
}
