#include "ble_core_internal.h"

#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ble_core";

/* ATT MTU ceiling that matches CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517.
   Sized to hold the largest known GoPro response (~88 bytes for GetHardwareInfoRsp)
   with headroom to spare. */
#define NOTIFY_BUF_SIZE 517

/* ---------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------*/

/* Encryption-failure statuses that definitively mean the local LTK is bad.
   Anything else preserves the bond — deleting an LTK requires the user to
   put the camera physically back into pair-mode to recover, so we err on
   "keep" and let the reconnect scan retry on the subsequent disconnect. */
static bool is_bond_invalid_error(int status)
{
    return status == BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL)   ||
           status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING);
}

/* ---------------------------------------------------------------------------
 * connection_event_cb — handles all GAP lifecycle events for connected peers.
 * Registered as the event_fn argument to ble_gap_connect().
 * Runs on the NimBLE host task.
 * -------------------------------------------------------------------------*/
int connection_event_cb(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {

    /* ---------------------------------------------------------------------- */
    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status == 0) {
            ble_gap_conn_find(event->connect.conn_handle, &desc);
            ESP_LOGI(TAG, "connected: handle=%u", event->connect.conn_handle);
            if (g_cbs.on_connected) {
                g_cbs.on_connected(event->connect.conn_handle, desc.peer_id_addr);
            }
            /* Immediately attempt to resume encryption using stored LTK,
               or begin first-time SMP pairing if no bond exists. */
            ble_gap_security_initiate(event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed: status=%d", event->connect.status);
            start_scan_if_needed();
        }
        break;

    /* ---------------------------------------------------------------------- */
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected: handle=%u reason=%d",
                 event->disconnect.conn.conn_handle,
                 event->disconnect.reason);
        /* Fire callback while conn_handle is still valid so the higher layer
           can look up the slot by handle before clearing it. */
        if (g_cbs.on_disconnected) {
            g_cbs.on_disconnected(event->disconnect.conn.conn_handle,
                                  event->disconnect.conn.peer_id_addr,
                                  (uint8_t)event->disconnect.reason);
        }
        s_connecting = false;
        start_scan_if_needed();
        break;

    /* ---------------------------------------------------------------------- */
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ble_gap_conn_find(event->enc_change.conn_handle, &desc);
            ESP_LOGI(TAG, "encrypted: handle=%u", event->enc_change.conn_handle);
            if (g_cbs.on_encrypted) {
                g_cbs.on_encrypted(event->enc_change.conn_handle, desc.peer_id_addr);
            }
        } else {
            ESP_LOGW(TAG, "encryption failed: handle=%u status=%d",
                     event->enc_change.conn_handle, event->enc_change.status);
            if (is_bond_invalid_error(event->enc_change.status)) {
                /* Key mismatch — purge bond so the next connection performs
                   fresh SMP pairing rather than re-failing with stale keys. */
                if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                    ble_gap_unpair(&desc.peer_id_addr);
                    ESP_LOGW(TAG, "stale bond deleted (key mismatch)");
                }
            }
            /* Otherwise: bond preserved; the subsequent
               BLE_GAP_EVENT_DISCONNECT will call start_scan_if_needed. */
        }
        break;

    /* ---------------------------------------------------------------------- */
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Camera has a bond but the ESP32 does not (e.g. after NVS erase).
           Delete the camera's stale entry so NimBLE retries fresh pairing. */
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_gap_unpair(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    /* ---------------------------------------------------------------------- */
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > NOTIFY_BUF_SIZE) {
            ESP_LOGW(TAG, "notify truncated: %u → %u bytes", len, NOTIFY_BUF_SIZE);
            len = NOTIFY_BUF_SIZE;
        }
        uint8_t buf[NOTIFY_BUF_SIZE];
        os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
        if (g_cbs.on_notify_rx) {
            g_cbs.on_notify_rx(event->notify_rx.conn_handle,
                               event->notify_rx.attr_handle, buf, len);
        }
        break;
    }

    default:
        break;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * ble_core_purge_unknown_bonds
 *
 * Must be called before ble_core_init().  Walks the NimBLE peer-security
 * store and deletes any bond not in keep[].
 * -------------------------------------------------------------------------*/
void ble_core_purge_unknown_bonds(const ble_addr_t keep[], int keep_count)
{
    ble_addr_t bonded[CONFIG_BT_NIMBLE_MAX_CONNECTIONS + 1];
    int        bond_count = 0;

    int rc = ble_store_util_bonded_peers(bonded, &bond_count,
                                         (int)ARRAY_SIZE(bonded));
    if (rc != 0) {
        ESP_LOGE(TAG, "purge: failed to enumerate bonds rc=%d", rc);
        return;
    }

    for (int i = 0; i < bond_count; i++) {
        bool known = false;
        for (int j = 0; j < keep_count; j++) {
            if (bonded[i].type == keep[j].type &&
                memcmp(bonded[i].val, keep[j].val, sizeof(bonded[i].val)) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            ESP_LOGI(TAG, "purging unknown bond");
            ble_gap_unpair(&bonded[i]);
        }
    }
}

/* ---------------------------------------------------------------------------
 * ble_core_remove_bond — posted to the NimBLE event queue (thread-safe)
 * -------------------------------------------------------------------------*/
static void do_remove_bond(struct ble_npl_event *ev)
{
    ble_addr_t *addr = ble_npl_event_get_arg(ev);

    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find_by_addr(addr, &desc) == 0) {
        ble_gap_terminate(desc.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_unpair(addr);
    ESP_LOGI(TAG, "bond removed");

    free(addr);
    ble_npl_event_deinit(ev);
    free(ev);
}

void ble_core_remove_bond(const ble_addr_t *addr)
{
    struct ble_npl_event *ev        = calloc(1, sizeof(*ev));
    ble_addr_t           *addr_copy = malloc(sizeof(*addr_copy));
    if (!ev || !addr_copy) {
        free(ev);
        free(addr_copy);
        return;
    }
    *addr_copy = *addr;
    ble_npl_event_init(ev, do_remove_bond, addr_copy);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), ev);
}
