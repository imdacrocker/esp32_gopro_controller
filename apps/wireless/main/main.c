
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "wifi_manager.h"
#include "ble_core.h"
#include "camera_manager.h"
#include "open_gopro_ble.h"
#include "gopro_wifi_rc.h"
#include "can_manager.h"
#include "http_server_wireless.h"
#include "log_ring.h"
#include "shutdown_manager.h"

static const char *TAG = "main";

/* ---- OTA rollback disarm (§11) ------------------------------------------ *
 * After an OTA commit, the bootloader marks the new app PENDING_VERIFY. If
 * the app never calls mark_valid_cancel_rollback before the next reset,
 * the bootloader auto-reverts to the previous slot. We disarm rollback as
 * soon as http_server_wireless_init() returns — httpd serving = "healthy enough"
 * given the closed CAN-bus threat model. A timer-based soak was overkill
 * and lost the dev-loop race against the USB-UART reset triggered when
 * idf.py monitor attaches post-OTA.
 *
 * Returns ESP_ERR_NOT_SUPPORTED when running from the factory partition
 * (recovery app) — silent no-op in that case.
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
    open_gopro_ble_sync_time_all();
    gopro_wifi_rc_sync_time_all();
}

/* ---- WiFi station callbacks (§21.3) -------------------------------------- */

static void on_station_associated(const uint8_t mac[6])
{
    camera_manager_on_station_associated(mac);
    gopro_wifi_rc_on_station_associated(mac);
}

static void on_station_disconnected(const uint8_t mac[6])
{
    camera_manager_on_station_disassociated(mac);
    gopro_wifi_rc_on_station_disassociated(mac);   /* RC-emulation path */
    /* BLE-control cameras never associate to the SoftAP, so no driver
     * notification is needed beyond the manager bookkeeping. */
}

static void on_station_ip_assigned(const uint8_t mac[6], uint32_t ip)
{
    camera_manager_on_station_ip(mac, ip);   /* updates last_ip for any matching slot */
    gopro_wifi_rc_on_station_dhcp(mac, ip);
}

void app_main(void)
{
    /* Diagnostic log capture (docs/design/log-capture.md). Must run before
     * any other init so pre-NVS boot logs are captured. The ring starts
     * ENABLED so this early window is caught; if the user's persisted
     * preference is OFF (the default) the load call below will clear it. */
    log_ring_init();

    ESP_LOGI(TAG, "boot: NimBLE core=%d, WiFi core=%d, channel=%d",
             CONFIG_BT_NIMBLE_PINNED_TO_CORE,
             CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0 ? 0 : 1,
             AP_CHANNEL);

    /* NVS is required by BLE for bonding info.  If the first init fails
     * with the standard "needs erase" errors, wipe and retry.  ESP_ERROR_CHECK
     * the second init's result: if even a fresh erase doesn't yield a usable
     * NVS partition the device is unrecoverable and we'd rather panic loudly
     * than proceed with every NVS write silently failing downstream. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Apply the persisted "Enable Logging" toggle. Default is OFF, so on a
     * fresh device the small handful of pre-NVS log lines captured above
     * are cleared here. */
    log_ring_load_persisted_enabled();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    camera_manager_init();

    /* Initialise shutdown state machine before any component that consults
     * shutdown_manager_is_active() (can_manager RX/TX gates, ble_core scan
     * gate, http_server reject_if_shutting_down helper).
     * See docs/design/shutdown.md §13. */
    shutdown_manager_init();

    /* Registers RC-emulation driver, starts work/shutter/UDP tasks. */
    gopro_wifi_rc_init();

    /* Registers BLE callbacks with ble_core, registers BLE-control driver
     * with camera_manager, and purges stale bonds.
     * Must be called before ble_core_init(). */
    open_gopro_ble_init();

    /* Wire WiFi station events to the RC-emulation driver and camera_manager.
     * Must be called before wifi_manager_init() so no events are lost. */
    wifi_manager_set_callbacks(on_station_associated,
                               on_station_disconnected,
                               on_station_ip_assigned);

    /* Raises the SoftAP — must come after all station callbacks are wired.
     *
     * WiFi MUST come up before ble_core_init() starts the NimBLE controller.
     * The ESP32 shares one antenna between WiFi and BLE; if BLE begins its
     * radio-intensive scanning/connection work before the AP has finished
     * bring-up, the coex scheduler can starve WiFi and the AP never gets
     * its beacon on air.  Blocking on AP_START gives WiFi the clear window
     * it needs before BLE engages the coex layer. */
    wifi_manager_init();
    wifi_manager_wait_for_ap_ready();

    /* Mount LittleFS, start esp_httpd, register all /api/ handlers.
     * TCP-only — does not touch the radio, safe to do before BLE. */
    http_server_wireless_init();

    /* REVIEW[main:M1] (minor/robustness — intentional tradeoff): rollback is
     * disarmed here, BEFORE ble_core_init() and can_manager_init() run. A
     * post-OTA regression that panics inside BLE or CAN bring-up would then
     * crash-loop with rollback already disarmed (no auto-revert to the previous
     * good slot). The early placement is deliberate — see the mark_ota_valid()
     * header: a later soak lost the dev-loop race against the USB-UART reset
     * when `idf.py monitor` attaches post-OTA. Flagged only so the tradeoff is
     * visible; moving it after can_manager_init() would widen rollback coverage
     * at the cost of reintroducing that race. */
    /* Disarm OTA rollback (§11). httpd up = "healthy enough." */
    mark_ota_valid();

    /* Starts the NimBLE host task. on_sync fires async and begins scanning.
     * Deferred until after the AP beacon is on-air (see comment above). */
    ble_core_init();

    /* Wire CAN callbacks before starting the TWAI driver. */
    can_manager_callbacks_t can_cbs = {
        .on_logging_state         = NULL,   /* camera_manager handles intent directly */
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
}
