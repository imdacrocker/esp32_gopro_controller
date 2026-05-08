/*
 * command.c — Shutter task (UDP), minimal HTTP/1.0 GET helper used only by
 * identify + datetime, JSON substring extractor for the identify response,
 * and the date/time setter.
 *
 * §17.2.5 (identify), §17.2.6 (date/time), §17.7 (shutter), §17.8 (sync time).
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera_manager.h"
#include "can_manager.h"
#include "gopro_model.h"
#include "gopro_wifi_rc_internal.h"

static const char *TAG = "gopro_rc/cmd";

/* ---- Plain HTTP/1.0 GET helper ------------------------------------------- */

/*
 * Minimal HTTP/1.0 GET via raw lwIP BSD sockets.  Used only for the identify
 * probe and date/time set; not on the recurring control path.  HTTP/1.0 closes
 * the connection after the response, so we read until EOF (or buf_len-1).
 *
 * Returns the HTTP status code (e.g. 200) on success, or -1 on transport
 * failure (connect / send / empty recv).  resp_buf, if non-NULL, receives the
 * body (after stripping the response headers) NUL-terminated; silently
 * truncated if buf_len-1 is exceeded.
 */
int rc_http_get(uint32_t ip, const char *path, int timeout_ms,
                char *resp_buf, size_t buf_len)
{
    /* Format IP for logs and Host header (ip is in network byte order). */
    char ip_str[16];
    ip4_addr_t a = { .addr = ip };
    ip4addr_ntoa_r(&a, ip_str, sizeof(ip_str));

    /* DEBUG-TRACE: log each HTTP transaction stage at INFO so we can see
     * exactly where a camera-specific failure (RST, slow recv, etc.) lands.
     * Lower to LOGD once the legacy-camera HTTP paths are confirmed. */
    ESP_LOGI(TAG, "HTTP GET http://%s%s (timeout %d ms)",
             ip_str, path, timeout_ms);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGW(TAG, "  └ socket() failed: errno=%d", errno);
        return -1;
    }

    /* Non-blocking connect + select-based timeout.
     *
     * ESP-IDF lwIP's blocking connect() does NOT honor SO_SNDTIMEO — observed
     * 18 s waits with a 2 s SO_SNDTIMEO when the camera silently drops the
     * SYN (Hero3/Hero7 in RC mode have no HTTP server on their STA interface
     * and exhaust the full TCP SYN-retry window before failing with errno=113
     * EHOSTUNREACH).
     *
     * Using O_NONBLOCK + select() bounds the wait to exactly timeout_ms,
     * which keeps the work task responsive (≤ 2 s blocked on the identify
     * probe instead of ≤ 18 s — short enough that keepalive ticks for ready
     * slots aren't starved). */
    int flags = fcntl(sock, F_GETFL, 0);
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "  └ fcntl O_NONBLOCK failed: errno=%d", errno);
        close(sock);
        return -1;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RC_HTTP_PORT),
        .sin_addr.s_addr = ip,
    };

    TickType_t t0 = xTaskGetTickCount();
    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        ESP_LOGI(TAG, "  └ connect failed immediately: errno=%d", errno);
        close(sock);
        return -1;
    }

    if (rc != 0) {
        /* Wait for socket to become writable = connect resolved (success or
         * fail).  select() respects timeout_ms exactly. */
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval ctv = {
            .tv_sec  = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        int sel = select(sock + 1, NULL, &wfds, NULL, &ctv);
        if (sel <= 0) {
            ESP_LOGI(TAG, "  └ connect timed out after %lu ms (sel=%d errno=%d) "
                          "— camera has no HTTP server on STA",
                     (unsigned long)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS),
                     sel, errno);
            close(sock);
            return -1;
        }

        /* Connect resolved — check whether it succeeded. */
        int conn_err = 0;
        socklen_t conn_err_len = sizeof(conn_err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &conn_err, &conn_err_len) < 0
            || conn_err != 0) {
            ESP_LOGI(TAG, "  └ connect failed after %lu ms: errno=%d (%s)",
                     (unsigned long)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS),
                     conn_err,
                     conn_err == 104 ? "ECONNRESET — no HTTP server on STA" :
                     conn_err == 110 ? "ETIMEDOUT" :
                     conn_err == 111 ? "ECONNREFUSED" :
                     conn_err == 113 ? "EHOSTUNREACH — SYN retries exhausted" : "?");
            close(sock);
            return -1;
        }
    }
    ESP_LOGI(TAG, "  └ connected in %lu ms",
             (unsigned long)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS));

    /* Restore blocking mode for send/recv (driven by SO_RCVTIMEO/SO_SNDTIMEO,
     * which DO apply correctly outside of connect). */
    fcntl(sock, F_SETFL, flags);

    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Minimal HTTP/1.0 request — no extra headers that confuse Hero4. */
    char req[256];
    int  req_len = snprintf(req, sizeof(req),
                            "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n",
                            path, ip_str);
    TickType_t t1 = xTaskGetTickCount();
    int sent = send(sock, req, req_len, 0);
    if (sent < 0) {
        ESP_LOGI(TAG, "  └ send failed: errno=%d", errno);
        close(sock);
        return -1;
    }
    ESP_LOGI(TAG, "  └ sent %d/%d bytes in %lu ms",
             sent, req_len,
             (unsigned long)((xTaskGetTickCount() - t1) * portTICK_PERIOD_MS));

    /* If the caller didn't supply a buffer, drain into a tiny scratch just
     * long enough to parse the status line. */
    char  scratch[RC_HTTP_CMD_RESP_MAX];
    char *rbuf;
    int   rbuf_size;
    if (resp_buf && buf_len > 0) {
        rbuf      = resp_buf;
        rbuf_size = (int)buf_len;
    } else {
        rbuf      = scratch;
        rbuf_size = (int)sizeof(scratch);
    }

    TickType_t t2 = xTaskGetTickCount();
    int total = 0;
    int n;
    while ((n = recv(sock, rbuf + total, rbuf_size - 1 - total, 0)) > 0) {
        total += n;
        if (total >= rbuf_size - 1) break;
    }
    int recv_errno = (n < 0) ? errno : 0;
    rbuf[total] = '\0';
    close(sock);

    ESP_LOGI(TAG, "  └ recv: %d bytes in %lu ms (last n=%d errno=%d)",
             total,
             (unsigned long)((xTaskGetTickCount() - t2) * portTICK_PERIOD_MS),
             n, recv_errno);

    if (total == 0) return -1;

    /* Parse "HTTP/1.x NNN ..." status line. */
    int status = -1;
    if (strncmp(rbuf, "HTTP/", 5) == 0) {
        const char *sp = strchr(rbuf, ' ');
        if (sp) status = atoi(sp + 1);
    }
    ESP_LOGI(TAG, "  └ HTTP status=%d", status);

    /* If the caller asked for the body, strip headers in-place. */
    if (resp_buf && buf_len > 0) {
        char *body = strstr(rbuf, "\r\n\r\n");
        if (body) {
            body += 4;
            size_t body_len = (size_t)(rbuf + total - body);
            memmove(rbuf, body, body_len + 1);  /* +1 to copy NUL */
        } else {
            rbuf[0] = '\0';
        }
    }

    return status;
}

/* ---- Shutter task (priority 7) ------------------------------------------- */

/*
 * Loop forever on s_shutter_queue.  Each enqueued command carries its own
 * destination IP and repeat count, so the task is transport-agnostic:
 *   - Broadcast (set_desired_recording_all)  → ip = 0xFFFFFFFFu, repeat = 3
 *   - Unicast   (per-slot / mismatch poll)   → ip = slot's last_ip, repeat = 1
 *
 * Repeats are sent back-to-back with no delay; at 54 Mbps a 14-byte SH frame
 * is sub-millisecond of airtime, so three sends complete in well under 1 ms.
 */
void rc_shutter_task(void *arg)
{
    (void)arg;
    rc_shutter_cmd_t cmd;
    while (1) {
        xQueueReceive(s_shutter_queue, &cmd, portMAX_DELAY);
        for (int i = 0; i < cmd.repeat; i++) {
            rc_send_sh(cmd.ip, cmd.start);
        }
        ESP_LOGI(TAG, "SH %s %s x%u",
                 cmd.start ? "start" : "stop",
                 cmd.ip == 0xFFFFFFFFu ? "broadcast" : "unicast",
                 (unsigned)cmd.repeat);
    }
}

/* ---- Date/time sync (§17.2.6) -------------------------------------------- */

/*
 * Send the current local time to a single slot's camera over HTTP/1.0.
 *
 * Gated by:
 *   - gopro_model_supports_http_datetime() — false for HERO_LEGACY_RC, so
 *     Hero3-class slots silently skip without a noisy log.
 *   - can_manager_utc_is_session_synced() — true only after a live UTC source
 *     has won this session (CAN GPS frame or web-UI manual set).  An NVS-
 *     restored UTC at boot does NOT unlock this path; we'd rather leave the
 *     camera's clock untouched than overwrite it with a stale value.
 *
 * URL format follows the Lua-verified template: each of the six time fields
 * (year mod 100, month, day, hour, minute, second) is URL-encoded as %XX hex.
 *
 * TODO(§17.13): apply can_manager tz offset before encoding so the camera's
 * local-time fields are correct.  Currently sends UTC as-is.
 */
void rc_send_datetime(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    if (!ctx->last_ip) return;

    camera_model_t model = camera_manager_get_model(slot);
    if (!gopro_model_supports_http_datetime(model)) {
        ESP_LOGD(TAG, "slot %d: datetime skipped — model has no HTTP datetime path",
                 slot);
        return;
    }

    if (!can_manager_utc_is_session_synced()) {
        ESP_LOGD(TAG, "slot %d: skipping datetime — UTC not session-synced", slot);
        return;
    }

    time_t  t;
    struct tm now;
    time(&t);
    gmtime_r(&t, &now);

    char path[80];
    snprintf(path, sizeof(path), RC_HTTP_PATH_DATETIME_FMT,
             (now.tm_year + 1900) % 100,
             now.tm_mon + 1,
             now.tm_mday,
             now.tm_hour,
             now.tm_min,
             now.tm_sec);

    int code = rc_http_get(ctx->last_ip, path, RC_HTTP_TIMEOUT_MS, NULL, 0);
    if (code == 200) {
        ESP_LOGI(TAG, "slot %d: datetime set OK", slot);
    } else {
        ESP_LOGD(TAG, "slot %d: datetime set → HTTP %d", slot, code);
    }
}

/* ---- Sync time all handler (called from work task on RC_CMD_SYNC_TIME_ALL) */

void rc_handle_sync_time_all(void)
{
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        if (s_ctx[i].wifi_ready) {
            rc_send_datetime(i);
        }
    }
}
