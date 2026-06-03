/*
 * http_server_wireless.h — Public API for the wireless variant's HTTP
 * server.
 *
 * Brings up http_server_core (LittleFS, httpd, static assets, shared
 * /api/... endpoints) and registers the wireless-specific endpoints
 * (camera pairing, RC) on top.
 *
 * Must be called last in app_main() — after all other components are up,
 * the SoftAP is on-air, and wifi_manager_wait_for_ap_ready() has returned.
 */
#pragma once

void http_server_wireless_init(void);
