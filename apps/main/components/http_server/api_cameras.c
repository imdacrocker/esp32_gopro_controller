/*
 * api_cameras.c — Camera management API handlers (§20.4).
 *
 * Endpoints:
 *   GET  /api/paired-cameras
 *   POST /api/shutter
 *   POST /api/remove-camera
 *   POST /api/reorder-cameras
 *   GET  /api/cameras
 *   POST /api/scan
 *   POST /api/scan-cancel
 *   POST /api/pair
 *   GET  /api/pair/status
 *   POST /api/pair/cancel
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "camera_manager.h"
#include "camera_types.h"
#include "open_gopro_ble.h"
#include "gopro_model.h"
#include "http_server_internal.h"

static const char *TAG = "http_api_cameras";

/* ---- Helpers ------------------------------------------------------------- */

static const char *model_name_str(camera_model_t model)
{
    switch (model) {
    case CAMERA_MODEL_GOPRO_HERO2:            return "GoPro Hero2";
    case CAMERA_MODEL_GOPRO_HERO3_WHITE:      return "GoPro Hero3 White";
    case CAMERA_MODEL_GOPRO_HERO3_SILVER:     return "GoPro Hero3 Silver";
    case CAMERA_MODEL_GOPRO_HERO3_BLACK:      return "GoPro Hero3 Black";
    case CAMERA_MODEL_GOPRO_HERO3PLUS_SILVER: return "GoPro Hero3+ Silver";
    case CAMERA_MODEL_GOPRO_HERO3PLUS_BLACK:  return "GoPro Hero3+ Black";
    case CAMERA_MODEL_GOPRO_HEROPLUS_LCD:     return "GoPro Hero+ LCD";
    case CAMERA_MODEL_GOPRO_HEROPLUS:         return "GoPro Hero+";
    case CAMERA_MODEL_GOPRO_HERO4_SILVER:     return "GoPro Hero4 Silver";
    case CAMERA_MODEL_GOPRO_HERO4_BLACK:      return "GoPro Hero4 Black";
    case CAMERA_MODEL_GOPRO_HERO4_SESSION:    return "GoPro Hero4 Session";
    case CAMERA_MODEL_GOPRO_HERO5_BLACK:      return "GoPro Hero5 Black";
    case CAMERA_MODEL_GOPRO_HERO5_SESSION:    return "GoPro Hero5 Session";
    case CAMERA_MODEL_GOPRO_HERO6_BLACK:      return "GoPro Hero6 Black";
    case CAMERA_MODEL_GOPRO_HERO_2018:        return "GoPro HERO (2018)";
    case CAMERA_MODEL_GOPRO_HERO_LEGACY_RC:   return "GoPro Hero (legacy)";
    case CAMERA_MODEL_GOPRO_HERO7_BLACK:      return "GoPro Hero7 Black";
    case CAMERA_MODEL_GOPRO_HERO8_BLACK:      return "GoPro Hero8 Black";
    case CAMERA_MODEL_GOPRO_HERO9_BLACK:      return "GoPro Hero9 Black";
    case CAMERA_MODEL_GOPRO_HERO10_BLACK:     return "GoPro Hero10 Black";
    case CAMERA_MODEL_GOPRO_HERO11_BLACK:     return "GoPro Hero11 Black";
    case CAMERA_MODEL_GOPRO_HERO11_MINI:      return "GoPro Hero11 Mini";
    case CAMERA_MODEL_GOPRO_HERO12_BLACK:     return "GoPro Hero12 Black";
    case CAMERA_MODEL_GOPRO_MAX2:             return "GoPro MAX2";
    case CAMERA_MODEL_GOPRO_HERO13_BLACK:     return "GoPro Hero13 Black";
    case CAMERA_MODEL_GOPRO_LIT_HERO:         return "GoPro Lit Hero";
    default:                                  return "Unknown";
    }
}

/*
 * Status mapping mirrors camera_manager state (docs/design/web-ui.md §...).
 *
 * WiFi (RC-emulation) — four states:
 *   disconnected — wifi_status != READY, !wifi_associated
 *   connecting   — wifi_status != READY,  wifi_associated
 *                  (slot is on the SoftAP but either still waiting on its
 *                   first UDP response post-associate, or has been demoted
 *                   from READY by the keepalive silence watchdog and is in
 *                   the WoL-retry loop)
 *   idle         — wifi_status == READY, !is_recording
 *   recording    — wifi_status == READY,  is_recording
 *
 * BLE — four states:
 *   disconnected — ble_status == NONE
 *   pairing      — ble_status != NONE, wifi_status != READY, !first_pair_complete
 *   connecting   — ble_status != NONE, wifi_status != READY,  first_pair_complete
 *   idle         — wifi_status == READY, !is_recording
 *   recording    — wifi_status == READY,  is_recording
 *
 * BLE drivers also flip wifi_status to READY at the end of their readiness
 * sequence, so the recording / idle branch is shared between transports.
 */
static const char *camera_status_str(const camera_slot_info_t *info)
{
    bool is_rc = gopro_model_uses_rc_emulation(info->model);

    if (info->wifi_status == WIFI_CAM_READY) {
        return info->is_recording ? "recording" : "idle";
    }

    if (is_rc) {
        return info->wifi_associated ? "connecting" : "disconnected";
    }

    /* BLE camera: choose label based on whether this is initial pairing. */
    if (info->ble_status == CAM_BLE_NONE) {
        return "disconnected";
    }
    return info->first_pair_complete ? "connecting" : "pairing";
}

static const char *pair_state_str(pair_attempt_state_t s)
{
    switch (s) {
    case PAIR_ATTEMPT_IDLE:         return "idle";
    case PAIR_ATTEMPT_CONNECTING:   return "connecting";
    case PAIR_ATTEMPT_BONDING:      return "bonding";
    case PAIR_ATTEMPT_PROVISIONING: return "provisioning";
    case PAIR_ATTEMPT_SUCCESS:      return "success";
    case PAIR_ATTEMPT_FAILED:       return "failed";
    default:                        return "unknown";
    }
}

static const char *pair_error_str(pair_attempt_error_t e)
{
    switch (e) {
    case PAIR_ERROR_NONE:               return "none";
    case PAIR_ERROR_SLOTS_FULL:         return "slots_full";
    case PAIR_ERROR_BLE_CONNECT_FAILED: return "ble_connect_failed";
    case PAIR_ERROR_BOND_FAILED:        return "bond_failed";
    case PAIR_ERROR_HWINFO_TIMEOUT:     return "hwinfo_timeout";
    case PAIR_ERROR_MODEL_UNSUPPORTED:  return "model_unsupported";
    case PAIR_ERROR_HANDSHAKE_TIMEOUT:  return "handshake_timeout";
    case PAIR_ERROR_DISCONNECTED:       return "disconnected";
    case PAIR_ERROR_CANCELLED:          return "cancelled";
    case PAIR_ERROR_PAIR_COMPLETE_FAIL: return "pair_complete_failed";
    case PAIR_ERROR_INTERNAL:           return "internal";
    default:                            return "unknown";
    }
}

/* ---- GET /api/paired-cameras ---------------------------------------------
 *
 * The `slot` and `index` fields in the JSON are 1-based — the first paired
 * camera is "Cam 1".  Internally the camera_manager uses 0-based array
 * indices; conversion happens at this API boundary.  POST handlers that
 * accept a `slot` field expect the same 1-based value and decrement on the
 * way in.
 */

static esp_err_t handler_paired_cameras(httpd_req_t *req)
{
    int count = camera_manager_get_slot_count();

    /* Each slot JSON entry is at most ~200 bytes; 4 slots + brackets = ~900. */
    const size_t BUF_SIZE = 1024;
    char *buf = malloc(BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t pos = 0;
    buf[pos++] = '[';

    bool first = true;
    for (int i = 0; i < count; i++) {
        camera_slot_info_t info;
        if (camera_manager_get_slot_info(i, &info) != ESP_OK) continue;
        if (!info.is_configured) continue;

        char mac_str[18];
        format_mac(mac_str, info.mac);

        char ip_str[16] = "";
        if (info.ip_addr != 0) {
            format_ip(ip_str, sizeof(ip_str), info.ip_addr);
        }

        const char *type   = gopro_model_uses_rc_emulation(info.model)
                             ? "rc_emulation" : "ble";
        const char *status = camera_status_str(&info);

        int external = i + 1;   /* 1-based for the API surface */
        int n = snprintf(buf + pos, BUF_SIZE - pos,
            "%s{"
            "\"slot\":%d,"
            "\"index\":%d,"
            "\"name\":\"%s\","
            "\"model_name\":\"%s\","
            "\"type\":\"%s\","
            "\"addr\":\"%s\","
            "\"ip\":\"%s\","
            "\"status\":\"%s\""
            "}",
            first ? "" : ",",
            external, external,
            info.name,
            model_name_str(info.model),
            type,
            mac_str,
            ip_str,
            status);

        if (n < 0 || (size_t)n >= BUF_SIZE - pos) {
            /* Truncation — stop here rather than emit corrupt JSON. */
            break;
        }
        pos += (size_t)n;
        first = false;
    }

    if (pos < BUF_SIZE - 1) buf[pos++] = ']';
    buf[pos] = '\0';

    send_json(req, buf);
    free(buf);
    return ESP_OK;
}

/* ---- POST /api/shutter --------------------------------------------------- */

static esp_err_t handler_shutter(httpd_req_t *req)
{
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;

    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *on_item   = cJSON_GetObjectItem(root, "on");
    cJSON *slot_item = cJSON_GetObjectItem(root, "slot");

    if (!cJSON_IsBool(on_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'on'");
        return ESP_FAIL;
    }

    bool record = cJSON_IsTrue(on_item);
    desired_recording_t intent = record ? DESIRED_RECORDING_START : DESIRED_RECORDING_STOP;
    int dispatched = 0;

    if (cJSON_IsNumber(slot_item)) {
        int external = (int)cJSON_GetNumberValue(slot_item);
        int slot = external - 1;   /* API is 1-based; internal is 0-based */
        camera_manager_set_desired_recording_slot(slot, intent);
        dispatched = 1;
        ESP_LOGI(TAG, "shutter %s → Cam %d", record ? "start" : "stop", external);
    } else {
        camera_manager_set_desired_recording_all(intent);
        dispatched = camera_manager_get_configured_count();
        ESP_LOGI(TAG, "shutter %s → all (%d slots)",
                 record ? "start" : "stop", dispatched);
    }

    cJSON_Delete(root);

    char resp[40];
    snprintf(resp, sizeof(resp), "{\"dispatched\":%d}", dispatched);
    send_json(req, resp);
    return ESP_OK;
}

/* ---- POST /api/repair-camera --------------------------------------------- *
 *
 * Clears `first_pair_complete` on a slot so the legacy wireless/pair/complete
 * orchestration re-runs on the next BLE reconnect.  For use after the user
 * runs Reset Connections on a Hero6/7/8 camera and the camera-side app entry
 * is wiped — without this, our firmware would assume the bond is registered
 * and the camera would silently refuse commands after power-cycle.
 */

static esp_err_t handler_repair_camera(httpd_req_t *req)
{
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;

    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *slot_item = cJSON_GetObjectItem(root, "slot");
    if (!cJSON_IsNumber(slot_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'slot'");
        return ESP_FAIL;
    }
    int external = (int)cJSON_GetNumberValue(slot_item);
    int slot = external - 1;
    cJSON_Delete(root);

    esp_err_t err = camera_manager_clear_first_pair_complete(slot);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Cam %d marked for re-pair on next reconnect", external);
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- POST /api/remove-camera --------------------------------------------- */

static esp_err_t handler_remove_camera(httpd_req_t *req)
{
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;

    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *slot_item = cJSON_GetObjectItem(root, "slot");
    if (!cJSON_IsNumber(slot_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'slot'");
        return ESP_FAIL;
    }
    int external = (int)cJSON_GetNumberValue(slot_item);
    int slot = external - 1;   /* API is 1-based; internal is 0-based */
    cJSON_Delete(root);

    esp_err_t err = camera_manager_remove_slot(slot);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "removed Cam %d", external);
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- POST /api/reorder-cameras ------------------------------------------- */

static esp_err_t handler_reorder_cameras(httpd_req_t *req)
{
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;

    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *order_arr = cJSON_GetObjectItem(root, "order");
    if (!cJSON_IsArray(order_arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'order' array");
        return ESP_FAIL;
    }

    int count = cJSON_GetArraySize(order_arr);
    int new_order[CAMERA_MAX_SLOTS];
    if (count <= 0 || count > CAMERA_MAX_SLOTS) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid order length");
        return ESP_FAIL;
    }

    for (int i = 0; i < count; i++) {
        cJSON *el = cJSON_GetArrayItem(order_arr, i);
        if (!cJSON_IsNumber(el)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "non-numeric in order");
            return ESP_FAIL;
        }
        /* API is 1-based; camera_manager expects 0-based */
        new_order[i] = (int)cJSON_GetNumberValue(el) - 1;
    }
    cJSON_Delete(root);

    esp_err_t err = camera_manager_reorder_slots(new_order, count);
    if (err == ESP_ERR_INVALID_STATE) {
        /* 409 Conflict — a camera in the reorder set is currently connected. */
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"error\":\"camera connected — disconnect before reordering\"}");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid order");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "slots reordered (%d entries)", count);
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- GET /api/cameras (BLE discovery) ------------------------------------ */

static esp_err_t handler_cameras(httpd_req_t *req)
{
    /* Heap-allocate the device list — 400 bytes on the httpd task stack is
     * too close to the stack limit when combined with httpd internals. */
    gopro_device_t *devices = malloc(GOPRO_DISC_MAX * sizeof(gopro_device_t));
    if (!devices) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int n = open_gopro_ble_get_discovered(devices, GOPRO_DISC_MAX);

    /* Max entry: ~100 bytes; 10 entries + brackets = ~1100. */
    const size_t BUF_SIZE = 1280;
    char *buf = malloc(BUF_SIZE);
    if (!buf) {
        free(devices);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t pos = 0;
    buf[pos++] = '[';

    for (int i = 0; i < n; i++) {
        char addr_str[18];
        format_mac(addr_str, devices[i].addr.val);

        int written = snprintf(buf + pos, BUF_SIZE - pos,
            "%s{\"name\":\"%s\",\"addr\":\"%s\",\"addr_type\":%d,\"rssi\":%d}",
            (i == 0) ? "" : ",",
            devices[i].name,
            addr_str,
            devices[i].addr.type,
            devices[i].rssi);

        if (written < 0 || (size_t)written >= BUF_SIZE - pos) break;
        pos += (size_t)written;
    }

    if (pos < BUF_SIZE - 1) buf[pos++] = ']';
    buf[pos] = '\0';

    send_json(req, buf);
    free(buf);
    free(devices);
    return ESP_OK;
}

/* ---- POST /api/scan ------------------------------------------------------ */

static esp_err_t handler_scan(httpd_req_t *req)
{
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;
    open_gopro_ble_start_discovery();
    ESP_LOGI(TAG, "BLE scan started");
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- POST /api/scan-cancel ----------------------------------------------- */

static esp_err_t handler_scan_cancel(httpd_req_t *req)
{
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;
    open_gopro_ble_stop_discovery();
    ESP_LOGI(TAG, "BLE scan cancelled");
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- POST /api/pair ------------------------------------------------------ */

static esp_err_t handler_pair(httpd_req_t *req)
{
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;

    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *addr_item      = cJSON_GetObjectItem(root, "addr");
    cJSON *addr_type_item = cJSON_GetObjectItem(root, "addr_type");

    if (!cJSON_IsString(addr_item) || !cJSON_IsNumber(addr_type_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing addr or addr_type");
        return ESP_FAIL;
    }

    ble_addr_t ble_addr;
    ble_addr.type = (uint8_t)cJSON_GetNumberValue(addr_type_item);

    if (!parse_mac(cJSON_GetStringValue(addr_item), ble_addr.val)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid addr");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    /* Reserve the pair-attempt state machine before kicking off BLE work.
     * If a previous attempt is still in flight, refuse with 409 — the UI is
     * expected to wait for the previous attempt to reach a terminal state. */
    esp_err_t err = pair_attempt_begin(ble_addr.val, ble_addr.type,
                                        PAIR_TRANSPORT_BLE);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"pair already in flight\"}");
        return ESP_FAIL;
    }

    open_gopro_ble_connect_by_addr(&ble_addr);
    ESP_LOGI(TAG, "pairing initiated");
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- GET /api/pair/status ----------------------------------------------- */

static const char *pair_transport_str(pair_attempt_transport_t t)
{
    switch (t) {
    case PAIR_TRANSPORT_WIFI_RC: return "wifi_rc";
    case PAIR_TRANSPORT_BLE:
    default:                     return "ble";
    }
}

static esp_err_t handler_pair_status(httpd_req_t *req)
{
    pair_attempt_info_t info;
    pair_attempt_get(&info);

    char addr_str[18];
    format_mac(addr_str, info.addr);

    char buf[360];
    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"state\":\"%s\","
        "\"transport\":\"%s\","
        "\"addr\":\"%s\","
        "\"addr_type\":%u,"
        "\"model\":%d,"
        "\"model_name\":\"%s\","
        "\"error_code\":\"%s\","
        "\"error_message\":\"%s\""
        "}",
        pair_state_str(info.state),
        pair_transport_str(info.transport),
        addr_str,
        info.addr_type,
        (int)info.model,
        model_name_str(info.model),
        pair_error_str(info.error_code),
        info.error_message);

    if (n < 0 || (size_t)n >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "buffer");
        return ESP_FAIL;
    }
    send_json(req, buf);
    return ESP_OK;
}

/* ---- POST /api/pair/cancel ---------------------------------------------- */

static esp_err_t handler_pair_cancel(httpd_req_t *req)
{
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;

    esp_err_t err = pair_attempt_cancel();
    if (err == ESP_ERR_INVALID_STATE) {
        /* No attempt in flight — not an error from the UI's POV; just report
         * the current (possibly already-terminal) state via the same JSON
         * shape as /api/pair/status would. */
        ESP_LOGI(TAG, "pair cancel: no attempt in flight");
    } else {
        ESP_LOGI(TAG, "pair cancel: issued");
    }
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- Registration -------------------------------------------------------- */

void api_cameras_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/paired-cameras",   .method = HTTP_GET,  .handler = handler_paired_cameras  },
        { .uri = "/api/shutter",          .method = HTTP_POST, .handler = handler_shutter         },
        { .uri = "/api/remove-camera",    .method = HTTP_POST, .handler = handler_remove_camera   },
        { .uri = "/api/repair-camera",    .method = HTTP_POST, .handler = handler_repair_camera   },
        { .uri = "/api/reorder-cameras",  .method = HTTP_POST, .handler = handler_reorder_cameras },
        { .uri = "/api/cameras",          .method = HTTP_GET,  .handler = handler_cameras         },
        { .uri = "/api/scan",             .method = HTTP_POST, .handler = handler_scan            },
        { .uri = "/api/scan-cancel",      .method = HTTP_POST, .handler = handler_scan_cancel     },
        { .uri = "/api/pair",             .method = HTTP_POST, .handler = handler_pair            },
        { .uri = "/api/pair/status",      .method = HTTP_GET,  .handler = handler_pair_status     },
        { .uri = "/api/pair/cancel",      .method = HTTP_POST, .handler = handler_pair_cancel     },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}
