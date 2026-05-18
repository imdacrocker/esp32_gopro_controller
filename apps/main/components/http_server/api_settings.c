/*
 * api_settings.c — Device settings API handlers (§20.4).
 *
 * Endpoints:
 *   GET  /api/settings/timezone
 *   POST /api/settings/timezone
 *   POST /api/settings/datetime
 *   GET  /api/settings/can-bitrate
 *   POST /api/settings/can-bitrate
 *   GET  /api/settings/logging-enabled
 *   POST /api/settings/logging-enabled
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "can_manager.h"
#include "log_ring.h"
#include "http_server_internal.h"

static const char *TAG = "http_api_settings";

/* ---- GET /api/settings/timezone ------------------------------------------ */

static esp_err_t handler_get_timezone(httpd_req_t *req)
{
    int8_t tz = can_manager_get_tz_offset();
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"tz_offset_hours\":%d}", (int)tz);
    send_json(req, buf);
    return ESP_OK;
}

/* ---- POST /api/settings/timezone ----------------------------------------- */

static esp_err_t handler_post_timezone(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *tz_item = cJSON_GetObjectItem(root, "tz_offset_hours");
    if (!cJSON_IsNumber(tz_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing tz_offset_hours");
        return ESP_FAIL;
    }
    int tz = (int)cJSON_GetNumberValue(tz_item);
    cJSON_Delete(root);

    if (tz < -12 || tz > 14) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tz_offset_hours out of range");
        return ESP_FAIL;
    }

    can_manager_set_tz_offset((int8_t)tz);
    ESP_LOGI(TAG, "timezone set to UTC%+d", tz);
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- POST /api/settings/datetime ----------------------------------------- */
/*
 * Sets the system clock from the browser and fires the UTC-acquired path
 * (time sync to all cameras).  Rejected only when a live source — GPS frame
 * or a previous manual set this session — has already won; an NVS-restored
 * value at boot does not block manual entry.
 */
static esp_err_t handler_post_datetime(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *ms_item = cJSON_GetObjectItem(root, "epoch_ms");
    if (!cJSON_IsNumber(ms_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing epoch_ms");
        return ESP_FAIL;
    }
    uint64_t utc_ms = (uint64_t)cJSON_GetNumberValue(ms_item);
    cJSON_Delete(root);

    esp_err_t err = can_manager_set_manual_utc_ms(utc_ms);
    if (err == ESP_ERR_INVALID_STATE) {
        /* A live source already won this session — manual override not permitted. */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "UTC already synced this session");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid timestamp");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "manual datetime set: %llu ms", (unsigned long long)utc_ms);
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- GET /api/settings/can-bitrate --------------------------------------- */

static esp_err_t handler_get_can_bitrate(httpd_req_t *req)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"bitrate_bps\":%u}",
             (unsigned)can_manager_get_bitrate());
    send_json(req, buf);
    return ESP_OK;
}

/* ---- POST /api/settings/can-bitrate -------------------------------------- */
/*
 * Body: {"bitrate_bps": <50000|100000|125000|250000|500000|1000000>}.
 * Persists to NVS; takes effect on next reboot.
 */
static esp_err_t handler_post_can_bitrate(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *item = cJSON_GetObjectItem(root, "bitrate_bps");
    if (!cJSON_IsNumber(item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing bitrate_bps");
        return ESP_FAIL;
    }
    uint32_t bps = (uint32_t)cJSON_GetNumberValue(item);
    cJSON_Delete(root);

    esp_err_t err = can_manager_set_bitrate(bps);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid bitrate_bps");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "CAN bitrate set to %u bps", (unsigned)bps);
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- GET /api/settings/logging-enabled ----------------------------------- */

static esp_err_t handler_get_logging_enabled(httpd_req_t *req)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"enabled\":%s}",
             log_ring_is_enabled() ? "true" : "false");
    send_json(req, buf);
    return ESP_OK;
}

/* ---- POST /api/settings/logging-enabled ---------------------------------- */
/*
 * Body: {"enabled": true|false}.
 * When transitioning ON -> OFF the ring is cleared inside
 * log_ring_set_enabled() before this returns, so by the time the response
 * goes out the captured data is already gone.
 */
static esp_err_t handler_post_logging_enabled(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *item = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing enabled");
        return ESP_FAIL;
    }
    bool desired = cJSON_IsTrue(item);
    cJSON_Delete(root);

    log_ring_set_enabled(desired);
    esp_err_t err = log_ring_save_enabled_to_nvs();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "logging-enabled NVS save failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "logging %s", desired ? "ENABLED" : "DISABLED");

    char reply[32];
    snprintf(reply, sizeof(reply), "{\"enabled\":%s}", desired ? "true" : "false");
    send_json(req, reply);
    return ESP_OK;
}

/* ---- Registration -------------------------------------------------------- */

void api_settings_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/settings/timezone",        .method = HTTP_GET,  .handler = handler_get_timezone        },
        { .uri = "/api/settings/timezone",        .method = HTTP_POST, .handler = handler_post_timezone       },
        { .uri = "/api/settings/datetime",        .method = HTTP_POST, .handler = handler_post_datetime       },
        { .uri = "/api/settings/can-bitrate",     .method = HTTP_GET,  .handler = handler_get_can_bitrate     },
        { .uri = "/api/settings/can-bitrate",     .method = HTTP_POST, .handler = handler_post_can_bitrate    },
        { .uri = "/api/settings/logging-enabled", .method = HTTP_GET,  .handler = handler_get_logging_enabled },
        { .uri = "/api/settings/logging-enabled", .method = HTTP_POST, .handler = handler_post_logging_enabled},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}
