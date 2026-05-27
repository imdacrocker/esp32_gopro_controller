#include "ble_core_internal.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_npl.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ble_core";

volatile bool s_connecting  = false;
volatile bool s_discovering = false;

/*
 * Deferred-rescan cooldown.
 *
 * After a supervisor-timeout disconnect the host fires DISCONNECT immediately
 * but the controller's connection table can take longer to drop the peer. If
 * the scanner reports a (cached/stale) advertisement during that window,
 * ble_gap_connect() returns BLE_HS_EDONE ("already connected"). Restarting
 * the scan inline causes a tight loop of EDONE failures at the controller's
 * advertising-report rate. Defer the rescan instead; by the time the callout
 * fires, the controller has fully torn the connection down.
 */
#define EDONE_RESCAN_DELAY_MS 3000

static struct ble_npl_callout s_rescan_co;

static void rescan_co_cb(struct ble_npl_event *ev)
{
    ESP_LOGI(TAG, "deferred rescan: resuming background scan");
    start_scan_if_needed();
}

void ble_core_scan_init(void)
{
    ble_npl_callout_init(&s_rescan_co, nimble_port_get_dflt_eventq(),
                         rescan_co_cb, NULL);
}

static void defer_rescan(void)
{
    ble_npl_callout_reset(&s_rescan_co,
                          ble_npl_time_ms_to_ticks32(EDONE_RESCAN_DELAY_MS));
}

/* ---------------------------------------------------------------------------
 * scan_event_cb
 *
 * Shared handler for both background and discovery scan events.
 *
 * Background mode (s_discovering == false):
 *   BLE_GAP_EVENT_DISC — if the advertising address is a known camera and no
 *   connection attempt is already in progress, cancel the scan and connect.
 *
 * Discovery mode (s_discovering == true):
 *   BLE_GAP_EVENT_DISC — forward every advertisement to on_disc.
 *
 * BLE_GAP_EVENT_DISC_COMPLETE:
 *   Reason BLE_HS_EPREEMPTED means the scan was cancelled by us (to connect
 *   or to start discovery); the cancelling code is responsible for any
 *   follow-up.  Any other reason means the discovery timeout elapsed
 *   naturally — restart background scan if needed.
 * -------------------------------------------------------------------------*/
static int scan_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_DISC:
        if (s_discovering) {
            if (g_cbs.on_disc) {
                g_cbs.on_disc(event->disc.addr, event->disc.rssi,
                              event->disc.data, event->disc.length_data);
            }
        } else {
            /* Background reconnect: connect to known cameras. */
            if (!s_connecting &&
                g_cbs.is_known_addr &&
                g_cbs.is_known_addr(event->disc.addr) &&
                !(g_cbs.is_shutdown_active && g_cbs.is_shutdown_active())) {

                s_connecting = true;
                ble_gap_disc_cancel();

                int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr,
                                         BLE_HS_FOREVER, NULL,
                                         connection_event_cb, NULL);
                if (rc != 0) {
                    ESP_LOGE(TAG, "connect failed: rc=%d", rc);
                    s_connecting = false;
                    if (rc == BLE_HS_EDONE) {
                        defer_rescan();
                    } else {
                        start_scan_if_needed();
                    }
                }
            }
        }
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_discovering = false;
        if (event->disc_complete.reason != BLE_HS_EPREEMPTED) {
            /* Natural completion (discovery timeout) — resume background scan. */
            ESP_LOGI(TAG, "discovery scan timeout — resuming background scan");
            start_scan_if_needed();
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * start_scan_if_needed
 *
 * Starts the passive background reconnect scan unless something prevents it:
 *   - a connect attempt or discovery scan is already running
 *   - has_disconnected_cameras() says there is nothing to reconnect to
 *
 * Uses software filtering (is_known_addr) rather than the HW whitelist so
 * that this call requires no knowledge of bonded addresses.  The design
 * permits this as a behaviorally-identical fallback (§12.4).
 * -------------------------------------------------------------------------*/
/* Public alias — see ble_core.h for the call-site contract. */
void ble_core_resume_background_scan(void)
{
    start_scan_if_needed();
}

void start_scan_if_needed(void)
{
    if (s_connecting || s_discovering) {
        ESP_LOGI(TAG, "camera is connecting, skipping background scan");
        return;
    }
    if (g_cbs.is_shutdown_active && g_cbs.is_shutdown_active()) {
        ESP_LOGI(TAG, "shutdown active, skipping background scan");
        return;
    }
    if (g_cbs.has_disconnected_cameras && !g_cbs.has_disconnected_cameras()) {
        ESP_LOGI(TAG, "no disconnected cameras, skipping background scan");
        return;
    }

    ble_gap_disc_cancel(); /* BLE_HS_EALREADY is normal — scan already stopped */

    struct ble_gap_disc_params params = {
        .itvl   = 0x0140,   // 320 * 0.625ms = 200 ms scan interval
        .window = 0x0030,   //  48 * 0.625ms =  30 ms scan window  → 15% duty
        .filter_policy     = 0,   /* no HW whitelist; is_known_addr filters in SW */
        .limited           = 0,
        .passive           = 1,
        .filter_duplicates = 1,   /* Filter duplicates.  Since this scan is forever, it will eventually timeout and re-find cameras */
    };

    ESP_LOGI(TAG, "starting background scan..");

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params,
                          scan_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "background scan start failed: rc=%d", rc);
    }
}

/* ---------------------------------------------------------------------------
 * Discovery scan — start (NPL event handler)
 * -------------------------------------------------------------------------*/
typedef struct {
    int timeout_ms;
} start_disc_args_t;

static void do_start_discovery(struct ble_npl_event *ev)
{
    start_disc_args_t *args = ble_npl_event_get_arg(ev);
    int timeout_ms = args->timeout_ms;
    ble_npl_event_deinit(ev);
    free(ev);
    free(args);

    /*
     * Set s_discovering before cancelling: if ble_gap_disc_cancel() fires
     * BLE_GAP_EVENT_DISC_COMPLETE synchronously (reason=EPREEMPTED), the
     * handler will see s_discovering=true and skip start_scan_if_needed.
     */
    s_discovering = true;
    ble_gap_disc_cancel();

    struct ble_gap_disc_params params = {
        .itvl   = 0x0140,   // 320 * 0.625ms = 200 ms scan interval
        .window = 0x0030,   //  48 * 0.625ms =  30 ms scan window  → 15% duty
        .filter_policy     = 0,   /* must see unpaired cameras */
        .limited           = 0,
        .passive           = 1,
        .filter_duplicates = 1,
    };

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, timeout_ms, &params,
                          scan_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "discovery scan start failed: rc=%d", rc);
        s_discovering = false;
        start_scan_if_needed();
    } else {
        ESP_LOGI(TAG, "discovery scan started (%d ms)", timeout_ms);
    }
}

void ble_core_start_discovery(int timeout_ms)
{
    struct ble_npl_event *ev   = calloc(1, sizeof(*ev));
    start_disc_args_t    *args = malloc(sizeof(*args));
    if (!ev || !args) {
        free(ev);
        free(args);
        return;
    }
    args->timeout_ms = timeout_ms;
    ble_npl_event_init(ev, do_start_discovery, args);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), ev);
}

/* ---------------------------------------------------------------------------
 * Discovery scan — stop (NPL event handler)
 * -------------------------------------------------------------------------*/
static void do_stop_discovery(struct ble_npl_event *ev)
{
    ble_npl_event_deinit(ev);
    free(ev);
    if (!s_discovering) {
        return;
    }
    s_discovering = false;
    ble_gap_disc_cancel(); /* fires DISC_COMPLETE(EPREEMPTED) — no auto-restart */
    ESP_LOGI(TAG, "discovery scan stopped by caller");
    start_scan_if_needed();
}

void ble_core_stop_discovery(void)
{
    struct ble_npl_event *ev = calloc(1, sizeof(*ev));
    if (!ev) {
        return;
    }
    ble_npl_event_init(ev, do_stop_discovery, NULL);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), ev);
}

/* ---------------------------------------------------------------------------
 * Connect by address (NPL event handler)
 * -------------------------------------------------------------------------*/
typedef struct {
    ble_addr_t addr;
} connect_args_t;

static void do_connect(struct ble_npl_event *ev)
{
    connect_args_t *args = ble_npl_event_get_arg(ev);
    ble_addr_t addr = args->addr;
    ble_npl_event_deinit(ev);
    free(ev);
    free(args);

    if (s_connecting) {
        ESP_LOGW(TAG, "connect_by_addr: already connecting, ignoring");
        return;
    }

    s_connecting  = true;
    s_discovering = false;
    ble_gap_disc_cancel();

    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, BLE_HS_FOREVER,
                             NULL, connection_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "connect_by_addr failed: rc=%d", rc);
        s_connecting = false;
        if (rc == BLE_HS_EDONE) {
            defer_rescan();
        } else {
            start_scan_if_needed();
        }
    }
}

esp_err_t ble_core_connect_by_addr(const ble_addr_t *addr)
{
    struct ble_npl_event *ev   = calloc(1, sizeof(*ev));
    connect_args_t       *args = malloc(sizeof(*args));
    if (!ev || !args) {
        free(ev);
        free(args);
        return ESP_ERR_NO_MEM;
    }
    args->addr = *addr;
    ble_npl_event_init(ev, do_connect, args);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), ev);
    return ESP_OK;
}
