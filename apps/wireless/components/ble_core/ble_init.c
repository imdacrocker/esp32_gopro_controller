#include "ble_core_internal.h"

#include "sdkconfig.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_log.h"

static const char *TAG = "ble_core";

ble_core_callbacks_t g_cbs;
volatile bool g_port_initialized = false;

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset: reason=%d", reason);
}

static void on_sync(void)
{
    /* The bond store has been loaded from NVS by now — drop bonds left by
     * cameras that were removed, before they can fill the store and block
     * pairing a new camera. */
    ble_core_purge_unknown_bonds_now();

    ESP_LOGI(TAG, "NimBLE synced — starting reconnect scan");
    start_scan_if_needed();
}

static void ble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_core_register_callbacks(const ble_core_callbacks_t *cbs)
{
    g_cbs = *cbs;
}

void ble_core_init(void)
{
    nimble_port_init();
    g_port_initialized = true;
    ble_core_scan_init();

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /* When the bond store is full, NimBLE asks the app to make room before
     * pairing a new peer; with no handler, ble_gap_security_initiate() just
     * fails and pairing never starts.  Evict the oldest bond instead.  The
     * stale-bond purge normally keeps the store from filling at all — this
     * is the backstop. */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Just Works pairing, bonding enabled, ENC + ID keys exchanged both ways. */
    ble_hs_cfg.sm_io_cap       = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding      = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gap_device_name_set(CONFIG_DEVICE_IDENTITY_NAME);

    nimble_port_freertos_init(ble_host_task);
}
