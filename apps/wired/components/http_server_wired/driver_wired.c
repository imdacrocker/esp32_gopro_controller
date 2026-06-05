/*
 * driver_wired.c — Wired-variant HTTP server orchestrator.
 *
 * Brings up http_server_core (which mounts LittleFS, starts esp_httpd,
 * registers static assets and the variant-agnostic /api/... handlers).
 *
 * Phase 1 registers no wired-specific endpoints yet — the single-camera
 * status + manual start/stop handlers land in Phase 4 alongside the USB
 * camera driver (see docs/design/wired-variant.md §4).
 */

#include "esp_log.h"
#include "esp_http_server.h"
#include "http_server_core.h"
#include "http_server_wired.h"

static const char *TAG = "http_server_wired";

void http_server_wired_init(void)
{
    httpd_handle_t server = http_server_core_start();
    if (!server) {
        ESP_LOGE(TAG, "http_server_core_start returned NULL");
        return;
    }

    /* Phase 4: api_camera_wired_register(server); */

    ESP_LOGI(TAG, "wired HTTP server up (core endpoints only)");
}
