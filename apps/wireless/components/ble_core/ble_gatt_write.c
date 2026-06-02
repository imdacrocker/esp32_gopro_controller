#include "ble_core_internal.h"

#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ble_core";

/* ---------------------------------------------------------------------------
 * ble_core_gatt_write
 *
 * ATT Write Request (write-with-response).  The OpenGoPro spec lists the
 * command/settings/query characteristics as "Write" — Hero13 and later silently
 * drop ATT Write Commands on those handles.  GoPro responses themselves still
 * arrive via notifications; the ATT write response only confirms receipt.
 *
 * Routed through the NimBLE event queue so it is safe to call from HTTP
 * handler tasks or camera_manager timers running on any core.
 * -------------------------------------------------------------------------*/

typedef struct {
    uint16_t conn_handle;
    uint16_t attr_handle;
    uint16_t len;
    uint8_t  data[];   /* flexible array — allocated with the struct */
} gatt_write_args_t;

static int on_write_done(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGW(TAG, "gatt write status=0x%04x conn=%u attr=%u",
                 error->status, conn_handle,
                 attr ? attr->handle : 0);
    }
    return 0;
}

static void do_gatt_write(struct ble_npl_event *ev)
{
    gatt_write_args_t *args = ble_npl_event_get_arg(ev);

    int rc = ble_gattc_write_flat(args->conn_handle, args->attr_handle,
                                   args->data, args->len,
                                   on_write_done, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt write failed: conn=%u attr=%u rc=%d",
                 args->conn_handle, args->attr_handle, rc);
    }

    ble_npl_event_deinit(ev);
    free(ev);
    free(args);
}

esp_err_t ble_core_gatt_write(uint16_t conn_handle, uint16_t attr_handle,
                               const uint8_t *data, uint16_t len)
{
    struct ble_npl_event *ev   = calloc(1, sizeof(*ev));
    gatt_write_args_t    *args = malloc(sizeof(*args) + len);
    if (!ev || !args) {
        free(ev);
        free(args);
        return ESP_ERR_NO_MEM;
    }

    args->conn_handle = conn_handle;
    args->attr_handle = attr_handle;
    args->len         = len;
    memcpy(args->data, data, len);

    ble_npl_event_init(ev, do_gatt_write, args);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), ev);
    return ESP_OK;
}
