/*
 * api_shutdown.c — Shutdown API handlers.  See docs/design/shutdown.md.
 *
 * Endpoints:
 *   POST /api/shutdown   — request shutdown (idempotent)
 *   GET  /api/shutdown   — current state + failed-slot list
 *
 * POST returns immediately with the current state; the per-slot work runs
 * in shutdown_manager tasks.  GET is polled by the web UI's shutdown screen.
 */

#include <stdio.h>
#include "esp_log.h"
#include "shutdown_manager.h"
#include "camera_manager.h"
#include "http_server_internal.h"

static const char *TAG = "http_api_shutdown";

static const char *state_str(shutdown_state_t s)
{
    switch (s) {
    case SHUTDOWN_STATE_IDLE:          return "idle";
    case SHUTDOWN_STATE_SHUTTING_DOWN: return "shutting_down";
    case SHUTDOWN_STATE_COMPLETE:      return "complete";
    default:                           return "unknown";
    }
}

/* ---- POST /api/shutdown -------------------------------------------------- */

static esp_err_t handler_post_shutdown(httpd_req_t *req)
{
    /* Idempotent — repeat POSTs while non-IDLE return the current state
     * rather than 503; the shutdown endpoints are deliberately NOT gated. */
    esp_err_t err = shutdown_manager_request();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "shutdown failed");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "shutdown initiated via /api/shutdown");

    char buf[48];
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\"}",
             state_str(shutdown_manager_get_state()));
    send_json(req, buf);
    return ESP_OK;
}

/* ---- GET /api/shutdown --------------------------------------------------- */

static esp_err_t handler_get_shutdown(httpd_req_t *req)
{
    shutdown_state_t state = shutdown_manager_get_state();
    uint8_t  mask  = shutdown_manager_get_failed_slots_mask();
    int      count = camera_manager_get_slot_count();

    /* External (1-based) slot numbers — matches the rest of the API surface. */
    char buf[128];
    int  pos = snprintf(buf, sizeof(buf),
                        "{\"state\":\"%s\",\"failed_slots\":[",
                        state_str(state));
    bool first = true;
    for (int i = 0; i < count && i < 8; i++) {
        if (mask & (1u << i)) {
            int n = snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                             "%s%d", first ? "" : ",", i + 1);
            if (n < 0 || (size_t)(pos + n) >= sizeof(buf)) break;
            pos += n;
            first = false;
        }
    }
    if ((size_t)pos + 2 < sizeof(buf)) {
        buf[pos++] = ']';
        buf[pos++] = '}';
        buf[pos]   = '\0';
    }
    send_json(req, buf);
    return ESP_OK;
}

/* ---- Registration -------------------------------------------------------- */

void api_shutdown_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/shutdown", .method = HTTP_POST, .handler = handler_post_shutdown },
        { .uri = "/api/shutdown", .method = HTTP_GET,  .handler = handler_get_shutdown  },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}
