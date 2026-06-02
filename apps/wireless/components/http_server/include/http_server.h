/*
 * http_server.h — Public API for the HTTP server component (§20).
 *
 * Serves the web UI from LittleFS and handles all /api/ endpoints.
 * Depends on every other component; no other component depends on this.
 */
#pragma once

/*
 * Mount LittleFS at /www, start the esp_httpd instance, and register all
 * static asset and /api/ endpoint handlers.
 *
 * Must be called last in app_main() — after all other components are up,
 * the SoftAP is on-air, and wifi_manager_wait_for_ap_ready() has returned.
 */
void http_server_init(void);
