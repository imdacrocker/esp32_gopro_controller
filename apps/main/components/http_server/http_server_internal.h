/*
 * http_server_internal.h — Shared declarations for http_server source files.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_http_server.h"
#include "cJSON.h"
#include "shutdown_manager.h"

/* ---- Handler registration (called from driver.c) ------------------------- */

void api_cameras_register(httpd_handle_t server);
void api_rc_register(httpd_handle_t server);
void api_settings_register(httpd_handle_t server);
void api_system_register(httpd_handle_t server);
void api_ota_register(httpd_handle_t server);
void api_logs_register(httpd_handle_t server);
void api_shutdown_register(httpd_handle_t server);

/* ---- Shared helpers ------------------------------------------------------ */

/*
 * Read the entire POST body into buf.  buf_size must include room for '\0'.
 * Returns byte count on success, -1 on error (response already sent).
 *
 * Loops on httpd_req_recv until content_len bytes have been accumulated:
 * the underlying socket recv is standard TCP and may legitimately return
 * fewer bytes than requested when the body is split across segments.  A
 * single-call read truncated the body silently on segmented POSTs (most
 * relevant for the ~512 B CAN config payload under WiFi congestion).
 *
 * Unlike api_ota.c:pump_body we deliberately do NOT retry on
 * HTTPD_SOCK_ERR_TIMEOUT.  For pump_body's multi-MB OTA streams a 5 s
 * pause is a legitimate pacing artefact of a slow WiFi link.  For our
 * small (≤512 B) JSON payloads it means the client is broken or
 * malicious — bouncing the connection releases the socket back to the
 * pool instead of holding it indefinitely while we re-call recv in a
 * loop.  The default recv_wait_timeout (5 s, from HTTPD_DEFAULT_CONFIG)
 * thus caps the per-byte stall; combined with lru_purge_enable in
 * driver.c, slow-trickle slowloris no longer monopolises sockets.
 */
static inline int read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len == 0) {
        buf[0] = '\0';
        return 0;
    }
    if (req->content_len >= buf_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return -1;
    }

    size_t total = 0;
    while (total < (size_t)req->content_len) {
        int n = httpd_req_recv(req, buf + total,
                                (size_t)req->content_len - total);
        if (n <= 0) {
            /* Timeout, peer closed, or real socket error — bail.  Letting
             * the handler return closes the connection (keep_alive_enable
             * is false), releasing the socket for legitimate clients. */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return -1;
        }
        total += (size_t)n;
    }
    buf[total] = '\0';
    return (int)total;
}

/* Format a 6-byte MAC as "XX:XX:XX:XX:XX:XX" into buf (needs ≥18 bytes). */
static inline void format_mac(char *buf, const uint8_t mac[6])
{
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Parse "XX:XX:XX:XX:XX:XX" into mac[6].  Returns true on success. */
static inline bool parse_mac(const char *str, uint8_t mac[6])
{
    unsigned int b[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
    return true;
}

/* Format a uint32_t IP (lwIP little-endian) as "A.B.C.D". */
static inline void format_ip(char *buf, size_t len, uint32_t ip)
{
    snprintf(buf, len, "%d.%d.%d.%d",
             (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
             (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF));
}

/* Parse "A.B.C.D" into a lwIP little-endian uint32_t.  Returns 0 on failure. */
static inline uint32_t parse_ip(const char *str)
{
    int a, b, c, d;
    if (sscanf(str, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return 0;
    return (uint32_t)(a | (b << 8) | (c << 16) | (d << 24));
}

/* Send a minimal JSON object response. */
static inline void send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

/*
 * Reject action-style requests while shutdown is in progress.  Returns
 * ESP_FAIL after emitting a 503 response if the shutdown_manager state is
 * non-IDLE; returns ESP_OK and does nothing otherwise.  Call from the top
 * of POST handlers that mutate camera/connection state.
 *
 * Read-only GET handlers, the shutdown endpoints themselves, and
 * POST /api/reboot deliberately do NOT call this — the UI must still be
 * able to observe state and the user must keep an escape hatch.
 * See docs/design/shutdown.md §8.
 */
static inline esp_err_t reject_if_shutting_down(httpd_req_t *req)
{
    if (!shutdown_manager_is_active()) return ESP_OK;
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"shutdown in progress\"}");
    return ESP_FAIL;
}
