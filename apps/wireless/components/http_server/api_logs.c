/*
 * api_logs.c — Diagnostic log endpoints.
 *
 * Endpoints:
 *   GET  /api/logs/download   — text/plain file with header block + ring contents
 *   POST /api/logs/clear      — empties the ring (lifetime counters preserved)
 *   GET  /api/logs/stats      — JSON {capacity, used, bytes_written_total, lines_dropped_total}
 *
 * The endpoints respond normally even when capture is disabled — the ring
 * will simply be empty. The UI hides the buttons in that case; gating
 * server-side would just produce confusing 4xxs if the toggle is flipped
 * between the UI render and the request.
 *
 * See docs/design/log-capture.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_mac.h"

#include "log_ring.h"
#include "can_manager.h"
#include "ota_io.h"
#include "http_server_internal.h"

static const char *TAG = "http_api_logs";

/* ---- helpers ------------------------------------------------------------- */

static void format_iso_utc(uint64_t utc_ms, char *out, size_t out_len)
{
    time_t secs = (time_t)(utc_ms / 1000ULL);
    struct tm tm_utc;
    gmtime_r(&secs, &tm_utc);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static void format_compact_utc(uint64_t utc_ms, char *out, size_t out_len)
{
    time_t secs = (time_t)(utc_ms / 1000ULL);
    struct tm tm_utc;
    gmtime_r(&secs, &tm_utc);
    strftime(out, out_len, "%Y%m%dT%H%M%SZ", &tm_utc);
}

/* ---- GET /api/logs/download ---------------------------------------------- */

/* Callback for log_ring_stream — forwards each chunk as an HTTP chunked frame. */
static esp_err_t send_chunk_cb(void *arg, const char *data, size_t n)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    return httpd_resp_send_chunk(req, data, (ssize_t)n);
}

static esp_err_t handler_get_download(httpd_req_t *req)
{
    /* Gather metadata for the header block + filename. */
    const esp_app_desc_t *app = esp_app_get_description();

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    char mac_full[18];
    format_mac(mac_full, mac);

    char mac6[7];
    snprintf(mac6, sizeof(mac6), "%02X%02X%02X", mac[3], mac[4], mac[5]);

    uint64_t uptime_s = (uint64_t)(esp_timer_get_time() / 1000000LL);

    uint64_t utc_ms = 0;
    bool utc_valid  = can_manager_get_utc_ms(&utc_ms);

    char iso_utc[32];
    if (utc_valid) {
        format_iso_utc(utc_ms, iso_utc, sizeof(iso_utc));
    } else {
        strncpy(iso_utc, "unset", sizeof(iso_utc));
    }

    char channel[OTA_IO_CHANNEL_MAX] = {0};
    ota_io_get_channel(channel, sizeof(channel));

    log_ring_stats_t st;
    log_ring_get_stats(&st);

    char fname_stamp[24];
    if (utc_valid) {
        format_compact_utc(utc_ms, fname_stamp, sizeof(fname_stamp));
    } else {
        snprintf(fname_stamp, sizeof(fname_stamp),
                 "up%llus", (unsigned long long)uptime_s);
    }

    char disposition[96];
    snprintf(disposition, sizeof(disposition),
             "attachment; filename=\"gopro-ctrl-%s-%s.txt\"",
             mac6, fname_stamp);

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "# device:    %s\n"
        "# firmware:  %s (%s %s) channel:%s\n"
        "# captured:  %s\n"
        "# uptime:    %llu s\n"
        "# ring:      %u bytes capacity, %u used, %llu lines dropped (lifetime)\n"
        "# enabled:   %s\n"
        "#\n",
        mac_full,
        app ? app->version : "unknown",
        app ? app->date    : "?",
        app ? app->time    : "?",
        channel[0] ? channel : "?",
        iso_utc,
        (unsigned long long)uptime_s,
        (unsigned)st.capacity,
        (unsigned)st.used,
        (unsigned long long)st.lines_dropped_total,
        log_ring_is_enabled() ? "true" : "false");

    if (hlen < 0 || hlen >= (int)sizeof(header)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "header format error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* No Content-Length — esp_httpd uses Transfer-Encoding: chunked when
     * httpd_resp_send_chunk is called instead of httpd_resp_send. */

    /* Header block goes out as the first chunk. */
    esp_err_t e = httpd_resp_send_chunk(req, header, hlen);
    if (e != ESP_OK) return e;

    /* Stream the ring body in 2 KB chunks (no heap allocation needed). */
    e = log_ring_stream(send_chunk_cb, req);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "log_ring_stream returned %s", esp_err_to_name(e));
        /* Headers and partial body already sent — best we can do is terminate. */
    }

    /* Terminate the chunked response. */
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---- POST /api/logs/clear ------------------------------------------------ */

static esp_err_t handler_post_clear(httpd_req_t *req)
{
    log_ring_stats_t before;
    log_ring_get_stats(&before);
    log_ring_clear();

    char body[64];
    snprintf(body, sizeof(body),
             "{\"cleared_bytes\":%u}", (unsigned)before.used);
    send_json(req, body);
    ESP_LOGI(TAG, "log ring cleared (%u bytes)", (unsigned)before.used);
    return ESP_OK;
}

/* ---- GET /api/logs/stats ------------------------------------------------- */

static esp_err_t handler_get_stats(httpd_req_t *req)
{
    log_ring_stats_t st;
    log_ring_get_stats(&st);

    char body[160];
    snprintf(body, sizeof(body),
             "{\"capacity\":%u,\"used\":%u,"
             "\"bytes_written_total\":%llu,\"lines_dropped_total\":%llu,"
             "\"enabled\":%s}",
             (unsigned)st.capacity,
             (unsigned)st.used,
             (unsigned long long)st.bytes_written_total,
             (unsigned long long)st.lines_dropped_total,
             log_ring_is_enabled() ? "true" : "false");
    send_json(req, body);
    return ESP_OK;
}

/* ---- Registration -------------------------------------------------------- */

void api_logs_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/logs/download", .method = HTTP_GET,  .handler = handler_get_download },
        { .uri = "/api/logs/clear",    .method = HTTP_POST, .handler = handler_post_clear   },
        { .uri = "/api/logs/stats",    .method = HTTP_GET,  .handler = handler_get_stats    },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}
