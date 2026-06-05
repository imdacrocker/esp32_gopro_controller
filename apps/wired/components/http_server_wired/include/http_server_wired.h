/*
 * http_server_wired.h — Public API for the wired variant's HTTP server.
 *
 * Brings up http_server_core (LittleFS, httpd, static assets, shared
 * /api/... endpoints) and will register the wired-specific endpoints
 * (single-camera status, manual start/stop) on top.
 *
 * Must be called from app_main() after wifi_manager_wait_for_ap_ready()
 * has returned — see docs/design/wired-variant.md §3.
 */
#pragma once

void http_server_wired_init(void);
