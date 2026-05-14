/*
 * Recovery app entry point.
 *
 * Minimal: NVS, netif, default event loop, SoftAP, recovery HTTP server.
 * No BLE, no LittleFS, no CAN, no camera_manager — recovery's job is to
 * accept new app + storage images over WiFi and reboot into ota_0.
 *
 * See docs/design/ota.md §9 (Phase 1 — Recovery app).
 */

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "recovery_http.h"

static const char *TAG = "recovery_main";

/* recovery.html embedded by EMBED_TXTFILES — symbol names are derived from
 * the filename (dots → underscores). EMBED_TXTFILES appends a NUL byte;
 * we subtract 1 so html_len reflects HTML content only. */
extern const char recovery_html_start[] asm("_binary_recovery_html_start");
extern const char recovery_html_end[]   asm("_binary_recovery_html_end");

void app_main(void)
{
    ESP_LOGI(TAG, "GoPro CAN-Bus Controller — Recovery v%s",
             CONFIG_APP_PROJECT_VER);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Same SoftAP as main app — the user doesn't need to switch networks
     * to reach the recovery UI. SSID is HERO-RC-<MAC suffix>, IP 10.71.79.1. */
    wifi_manager_init();
    wifi_manager_wait_for_ap_ready();

    size_t html_len = (size_t)(recovery_html_end - recovery_html_start) - 1;
    ESP_ERROR_CHECK(recovery_http_init(recovery_html_start, html_len));

    ESP_LOGI(TAG, "ready — browse to http://10.71.79.1/");
}
