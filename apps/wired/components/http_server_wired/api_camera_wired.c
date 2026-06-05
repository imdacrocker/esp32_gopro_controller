/*
 * api_camera_wired.c — Wired-variant single-camera HTTP endpoints.
 *
 * Serves the SAME JSON contract as http_server_wireless's /api/paired-cameras
 * and /api/shutter so the shared web UI renders the wired USB camera with no
 * UI fork — it just sees a one-element camera list and hides the pairing /
 * add-camera affordances (gated on /api/version's product == "wired").
 *
 * The wired variant has exactly one camera (cam_core slot 0, owned by the
 * gopro_usb driver), so there is no pairing, scanning, reorder or removal.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "cam_core.h"
#include "gopro_usb.h"
#include "usb_host_net.h"
#include "http_server_helpers.h"

static const char *TAG = "http_api_cam_wired";

#define WIRED_SLOT 0

/* Map cam_core CAN-state + USB link into the UI status vocabulary
 * (disconnected / connecting / idle / recording) used by renderCameraCards. */
static const char *wired_status_str(void)
{
    switch (cam_core_get_can_state(WIRED_SLOT)) {
    case CAMERA_CAN_STATE_RECORDING: return "recording";
    case CAMERA_CAN_STATE_IDLE:      return "idle";
    default:
        /* Slot registered but not ready: "connecting" while the USB link is
         * up (control handshake in flight), else "disconnected". */
        return usb_host_net_is_up() ? "connecting" : "disconnected";
    }
}

/* ---- GET /api/paired-cameras --------------------------------------------- *
 * One-element array; fields mirror the wireless schema (slot/index 1-based). */
static esp_err_t handler_paired_cameras(httpd_req_t *req)
{
    char model_name[40];
    gopro_usb_get_model_name(model_name, sizeof(model_name));

    char ip_str[16] = "";
    uint32_t ip_be = usb_host_net_camera_ip();
    if (ip_be) {
        const uint8_t *o = (const uint8_t *)&ip_be;   /* network-order octets */
        snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "[{"
        "\"slot\":1,"
        "\"index\":1,"
        "\"name\":\"\","
        "\"model_name\":\"%s\","
        "\"type\":\"usb\","
        "\"addr\":\"\","
        "\"ip\":\"%s\","
        "\"status\":\"%s\""
        "}]",
        model_name[0] ? model_name : "Unknown",
        ip_str,
        wired_status_str());

    send_json(req, buf);
    return ESP_OK;
}

/* ---- POST /api/shutter --------------------------------------------------- *
 * Body: {"on":bool[,"slot":1]}.  slot is ignored (single camera); both the
 * per-camera button and the Record/Stop bar drive the one slot. */
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
    cJSON *on_item = cJSON_GetObjectItem(root, "on");
    if (!cJSON_IsBool(on_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'on'");
        return ESP_FAIL;
    }
    bool record = cJSON_IsTrue(on_item);
    cJSON_Delete(root);

    desired_recording_t intent = record ? DESIRED_RECORDING_START
                                        : DESIRED_RECORDING_STOP;
    cam_core_set_desired_slot(WIRED_SLOT, intent);

    int dispatched = cam_core_slot_active(WIRED_SLOT) ? 1 : 0;
    ESP_LOGI(TAG, "shutter %s -> wired camera", record ? "start" : "stop");

    char resp[40];
    snprintf(resp, sizeof(resp), "{\"dispatched\":%d}", dispatched);
    send_json(req, resp);
    return ESP_OK;
}

/* ---- Registration -------------------------------------------------------- */

void api_camera_wired_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/paired-cameras", .method = HTTP_GET,  .handler = handler_paired_cameras },
        { .uri = "/api/shutter",        .method = HTTP_POST, .handler = handler_shutter        },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}
