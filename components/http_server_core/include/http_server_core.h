/*
 * http_server_core.h — Public API for the variant-shared HTTP server.
 *
 * Mounts LittleFS, starts esp_httpd, registers static asset handlers and
 * the variant-agnostic /api endpoints (version, ota/..., settings/..., logs,
 * shutdown, system). Returns the httpd handle so the variant-specific
 * HTTP server component (e.g. http_server_wireless) can register its own
 * additional handlers (camera pairing, RC, etc.) on top.
 *
 * Must be called from the variant's HTTP server init function — after all
 * other components are up, the SoftAP is on-air, and
 * wifi_manager_wait_for_ap_ready() has returned.
 */
#pragma once

#include "esp_http_server.h"

httpd_handle_t http_server_core_start(void);
