/*
 * driver_wireless.c — Wireless-variant HTTP server orchestrator.
 *
 * Brings up http_server_core (which mounts LittleFS, starts esp_httpd,
 * registers static assets and the variant-agnostic /api/... handlers), then
 * registers the wireless-specific /api/... handlers (camera management, RC)
 * on top of the returned handle.
 */

#include "esp_log.h"
#include "esp_http_server.h"
#include "http_server_core.h"
#include "http_server_wireless.h"

/* Wireless-specific /api/... registration entry points, defined in this
 * component's api_*.c files. */
void api_cameras_register(httpd_handle_t server);
void api_rc_register(httpd_handle_t server);

static const char *TAG = "http_server_wireless";

void http_server_wireless_init(void)
{
    httpd_handle_t server = http_server_core_start();
    if (!server) {
        ESP_LOGE(TAG, "http_server_core_start returned NULL");
        return;
    }

    api_cameras_register(server);
    api_rc_register(server);

    ESP_LOGI(TAG, "wireless HTTP endpoints registered");
}
