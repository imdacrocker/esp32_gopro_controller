/*
 * main.c — Wired (USB) GoPro controller app entry point.
 *
 * Boot composition mirrors the wireless app's app_main minus BLE/WiFi-RC,
 * plus (from Phase 2) USB host bring-up. See docs/design/wired-variant.md §3.
 *
 * Phases 1–2 (this file): boots the shared infrastructure — log ring, NVS,
 * cam_core, shutdown_manager, WiFi SoftAP (web UI only), http_server_core,
 * OTA-rollback disarm, the CAN manager — and the USB host bus (usb_host_net),
 * which surfaces the camera link via on_usb_link. The USB camera driver
 * (gopro_usb) that turns that link into a cam_core slot lands in Phase 3; the
 * proof-of-concept USB+HTTP sketch that previously lived here is preserved in
 * git history (commit aa4e7d7).
 */

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "cam_core.h"
#include "wifi_manager.h"
#include "can_manager.h"
#include "http_server_wired.h"
#include "log_ring.h"
#include "shutdown_manager.h"
#include "usb_host_net.h"
#include "gopro_usb.h"

static const char *TAG = "main";

/* ---- OTA rollback disarm ------------------------------------------------- *
 * After an OTA commit the bootloader marks the new app PENDING_VERIFY; if it
 * never calls mark_valid_cancel_rollback before the next reset the bootloader
 * auto-reverts. We disarm as soon as http_server_wired_init() returns —
 * httpd serving = "healthy enough" given the closed CAN-bus threat model.
 * Returns ESP_ERR_NOT_SUPPORTED from the factory partition (recovery app) —
 * silent no-op there. See docs/design/ota.md §11.
 */
static void mark_ota_valid(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA image marked valid (rollback disarmed)");
    } else if (err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
    }
}

/* ---- CAN callbacks ------------------------------------------------------- */

static void on_gps_utc_acquired(uint64_t utc_ms, void *arg)
{
    (void)utc_ms; (void)arg;
    gopro_usb_sync_time_all();
}

/* ---- USB camera link callback ------------------------------------------- */

static void on_usb_link(bool up, uint32_t camera_ip)
{
    if (up) {
        ESP_LOGI(TAG, "USB camera link up");
        gopro_usb_on_link_up(camera_ip);
    } else {
        ESP_LOGI(TAG, "USB camera link down");
        gopro_usb_on_link_down();
    }
}

void app_main(void)
{
    /* Diagnostic log capture (docs/design/log-capture.md). Must run before
     * any other init so pre-NVS boot logs are captured. The ring starts
     * ENABLED; the persisted-preference load below clears it if the user's
     * default (OFF) applies. */
    log_ring_init();

    /* NVS — required by wifi_manager, can_manager, ota_io, log_ring. Wipe and
     * retry on the standard "needs erase" errors; ESP_ERROR_CHECK the retry so
     * an unrecoverable NVS partition panics loudly rather than silently
     * failing every downstream write. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    log_ring_load_persisted_enabled();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* The wireless app calls cam_core_init() from inside camera_manager_init();
     * the wired variant has no camera_manager, so we init cam_core directly.
     * Must precede shutdown_manager_init() and can_manager_init(). */
    cam_core_init();

    /* Shutdown state machine — before any component that consults
     * shutdown_manager_is_active() (can_manager RX/TX gates,
     * http_server reject helpers). See docs/design/shutdown.md §13. */
    shutdown_manager_init();

    /* SoftAP for the web UI + browser OTA only. No station callbacks: no
     * cameras join the AP in the wired variant (the camera is on the USB
     * netif). No BLE means no radio coex ordering concern. */
    wifi_manager_init();
    wifi_manager_wait_for_ap_ready();

    /* Mount LittleFS, start esp_httpd, register all shared /api/ handlers. */
    http_server_wired_init();

    /* httpd up = "healthy enough"; disarm OTA rollback. */
    mark_ota_valid();

    /* Register the single USB camera slot + driver with cam_core and start its
     * worker.  Must precede usb_host_net_init() so the slot exists when the
     * first link-up event fires. */
    gopro_usb_init();

    /* Bring up the USB host bus; the camera link surfaces via on_usb_link
     * once the camera is plugged in and DHCP completes. Safe to start after
     * httpd — the UI is reachable with no camera attached. */
    usb_host_net_init(on_usb_link);

    /* Wire CAN callbacks before starting the TWAI driver. */
    can_manager_callbacks_t can_cbs = {
        .on_logging_state         = NULL,
        .on_logging_state_arg     = NULL,
        .on_utc_acquired          = on_gps_utc_acquired,
        .on_utc_acquired_arg      = NULL,
        .on_rx_frame              = NULL,
        .on_rx_frame_arg          = NULL,
        .on_shutdown_request      = shutdown_manager_on_can_request,
        .on_shutdown_request_arg  = NULL,
    };
    can_manager_register_callbacks(&can_cbs);
    can_manager_init();

    ESP_LOGI(TAG, "wired controller boot complete — USB host up, awaiting camera");
}
