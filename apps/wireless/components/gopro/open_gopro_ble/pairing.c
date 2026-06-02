/*
 * pairing.c — BLE connection lifecycle: connect, encrypt, disconnect.
 *
 * All callbacks run on the NimBLE host task (core 1).
 */

#include <string.h>
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "open_gopro_ble_internal.h"
#include "shutdown_manager.h"

static const char *TAG = "gopro_ble/pair";

/*
 * Hero11+ cameras typically initiate the ATT MTU exchange themselves shortly
 * after encryption, but Hero13 (firmware H24.x) does not — leaving the link
 * at the 23-byte default, which is too small for some protobuf commands.
 * We initiate the exchange ourselves; cameras that already exchanged simply
 * ack the existing MTU.
 */
static int on_mtu_exchanged(uint16_t conn_handle, const struct ble_gatt_error *error,
                             uint16_t mtu, void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;
    if (error->status == 0) {
        ctx->negotiated_mtu = mtu;
        ESP_LOGI(TAG, "slot %d: MTU exchanged, negotiated mtu=%u",
                 ctx->slot, mtu);
    } else {
        ESP_LOGW(TAG, "slot %d: MTU exchange status=0x%04x, using existing mtu=%u",
                 ctx->slot, error->status, ctx->negotiated_mtu);
    }
    /* Refresh from NimBLE in case the value moved underneath us. */
    uint16_t cur = ble_att_mtu(conn_handle);
    if (cur != 0) {
        ctx->negotiated_mtu = cur;
    }
    gopro_gatt_start_discovery(ctx);
    return 0;
}

/* ---- on_connected -------------------------------------------------------- */

void gopro_on_connected(uint16_t conn_handle, ble_addr_t addr)
{
    /* If shutdown is in progress, defense-in-depth: terminate any new BLE
     * connection that gets through (e.g. a camera-initiated reconnect that
     * raced our teardown).  See docs/design/shutdown.md §7. */
    if (shutdown_manager_is_active()) {
        ESP_LOGI(TAG, "shutdown active — terminating new connection conn=%u",
                 conn_handle);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    /* L2 is up; ble_core has already kicked off ble_gap_security_initiate().
     * If this is the camera the user is currently pairing, record the
     * conn_handle so cancel() can terminate the link and advance the state
     * machine.  Reconnects (known camera, no in-flight attempt) are
     * unaffected. */
    if (pair_attempt_addr_matches(addr.val)) {
        pair_attempt_set_conn_handle(conn_handle);
        pair_attempt_advance(PAIR_ATTEMPT_BONDING);
    }

    int slot = camera_manager_find_by_mac(addr.val);
    if (slot < 0) {
        /* Unknown camera — will be registered in on_encrypted once bonded. */
        ESP_LOGI(TAG, "connected unknown addr %02x:%02x:%02x:%02x:%02x:%02x, conn=%d",
                 addr.val[5], addr.val[4], addr.val[3],
                 addr.val[2], addr.val[1], addr.val[0], conn_handle);
        return;
    }

    gopro_ble_ctx_t *ctx = gopro_ctx_by_slot(slot);
    if (!ctx) {
        return;
    }
    ctx->conn_handle = conn_handle;
    camera_manager_on_ble_connected(slot, conn_handle);
    ESP_LOGI(TAG, "connected slot %d conn=%d", slot, conn_handle);
}

/* ---- on_encrypted -------------------------------------------------------- */

void gopro_on_encrypted(uint16_t conn_handle, ble_addr_t addr)
{
    bool is_active_pair = pair_attempt_addr_matches(addr.val);
    int slot = camera_manager_find_by_mac(addr.val);

    if (slot < 0) {
        /* First-time bond: register a new slot. */
        slot = camera_manager_register_new(addr.val);
        if (slot < 0) {
            ESP_LOGW(TAG, "no free slots for new camera, disconnecting conn=%d",
                     conn_handle);
            if (is_active_pair) {
                pair_attempt_fail(PAIR_ERROR_SLOTS_FULL,
                                  "All camera slots in use");
            }
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return;
        }
        ESP_LOGI(TAG, "registered new camera in slot %d conn=%d", slot, conn_handle);

        /* Carry the advertised name (e.g. "GP50553313") forward as the slot's
         * display name.  Discovery cache holds the value until the next scan. */
        char adv_name[32];
        if (open_gopro_ble_lookup_disc_name(addr.val, adv_name, sizeof(adv_name))) {
            camera_manager_set_name(slot, adv_name);
        }
    }

    if (is_active_pair) {
        pair_attempt_advance(PAIR_ATTEMPT_PROVISIONING);
    }

    gopro_ble_ctx_t *ctx = gopro_ctx_by_slot(slot);
    if (!ctx) {
        return;
    }

    ctx->conn_handle    = conn_handle;
    ctx->negotiated_mtu = ble_att_mtu(conn_handle);
    if (ctx->negotiated_mtu == 0) {
        ctx->negotiated_mtu = BLE_ATT_MTU_DFLT;
    }

    camera_manager_on_ble_connected(slot, conn_handle);

    ESP_LOGI(TAG, "encrypted slot %d conn=%d mtu=%d (initial)",
             slot, conn_handle, ctx->negotiated_mtu);

    /*
     * Initiate ATT MTU exchange before discovery — Hero13 does not initiate
     * it, and the default 23-byte MTU prevents larger protobuf commands
     * (e.g. RequestConnectNew) from fitting in a single ATT Write Request.
     */
    int rc = ble_gattc_exchange_mtu(conn_handle, on_mtu_exchanged, ctx);
    if (rc != 0) {
        ESP_LOGW(TAG, "slot %d: ble_gattc_exchange_mtu rc=%d, proceeding with mtu=%d",
                 slot, rc, ctx->negotiated_mtu);
        gopro_gatt_start_discovery(ctx);
    }
}

/* ---- on_disconnected ----------------------------------------------------- */

void gopro_on_disconnected(uint16_t conn_handle, ble_addr_t addr,
                            uint8_t reason)
{
    /* If a pair attempt is in flight for this address and hasn't already
     * been failed with a more specific cause (handshake/hwinfo timeout etc.),
     * record the disconnect.  pair_attempt_fail is a no-op when state is
     * already terminal, so the first cause wins. */
    if (pair_attempt_addr_matches(addr.val)) {
        pair_attempt_fail(PAIR_ERROR_DISCONNECTED,
                          "Camera disconnected during pairing");
    }

    gopro_ble_ctx_t *ctx = gopro_ctx_by_conn(conn_handle);
    if (!ctx) {
        return;
    }

    int slot = ctx->slot;
    ESP_LOGI(TAG, "disconnected slot %d conn=%d reason=0x%02x",
             slot, conn_handle, reason);

    /* Cancel timers before touching ctx fields. */
    gopro_readiness_cancel(ctx);
    gopro_keepalive_stop(ctx);
    gopro_status_poll_stop(ctx);
    gopro_query_free(ctx);

    /* Clear BLE context state. */
    memset(&ctx->gatt, 0, sizeof(ctx->gatt));
    ctx->conn_handle           = GOPRO_CONN_NONE;
    ctx->negotiated_mtu        = 0;
    atomic_store(&ctx->readiness_polling, false);
    ctx->readiness_retry_count = 0;
    ctx->cached_status         = CAMERA_RECORDING_UNKNOWN;
    /* Do NOT clear datetime_pending_utc — user may reconnect before UTC arrives. */

    camera_manager_on_ble_disconnected_by_handle(conn_handle);
}
