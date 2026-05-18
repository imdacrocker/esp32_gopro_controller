
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "wifi_manager.h"
#include "ble_core.h"
#include "camera_manager.h"
#include "open_gopro_ble.h"
#include "gopro_wifi_rc.h"
#include "can_manager.h"
#include "http_server.h"
#include "log_ring.h"

static const char *TAG = "main";

/* ---- OTA rollback disarm (§11) ------------------------------------------ *
 * After an OTA commit, the bootloader marks the new app PENDING_VERIFY. If
 * the app crashes (or never calls mark_valid_cancel_rollback) before the
 * next reset, the bootloader auto-reverts to factory (recovery). Arming a
 * 30 s timer here gives us confidence the new app is stable before
 * disarming the rollback. Returns ESP_ERR_NOT_SUPPORTED when running from
 * the factory partition — that's the recovery app, not relevant.
 */
static void rollback_timer_cb(void *arg)
{
    (void)arg;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA rollback disarmed — app marked valid");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        /* running from factory; no rollback to cancel */
    } else {
        ESP_LOGW(TAG, "mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
    }
}

static void arm_rollback_timer(void)
{
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {
        .callback        = rollback_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "rollback_arm",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_once(timer, 30ULL * 1000 * 1000));  /* 30 s */
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

    /* NVS is required by BLE for bonding info. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Apply the persisted "Enable Logging" toggle. Default is OFF, so on a
     * fresh device the small handful of pre-NVS log lines captured above
     * are cleared here. */
    log_ring_load_persisted_enabled();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    camera_manager_init();

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
    http_server_init();

    /* Arm the OTA rollback-disarm timer (§11). 30 s of survival + httpd
     * up = "looks healthy enough to keep." */
    arm_rollback_timer();

    /* Starts the NimBLE host task. on_sync fires async and begins scanning.
     * Deferred until after the AP beacon is on-air (see comment above). */
    ble_core_init();

    /* Wire CAN callbacks before starting the TWAI driver. */
    can_manager_callbacks_t can_cbs = {
        .on_logging_state     = NULL,   /* camera_manager handles intent directly */
        .on_logging_state_arg = NULL,
        .on_utc_acquired      = on_gps_utc_acquired,
        .on_utc_acquired_arg  = NULL,
        .on_rx_frame          = NULL,
        .on_rx_frame_arg      = NULL,
    };
    can_manager_register_callbacks(&can_cbs);
    can_manager_init();
}
