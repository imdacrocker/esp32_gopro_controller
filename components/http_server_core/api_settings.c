/*
 * api_settings.c — Device settings API handlers (§20.4).
 *
 * Endpoints:
 *   GET  /api/settings/timezone
 *   POST /api/settings/timezone
 *   POST /api/settings/datetime
 *   GET  /api/settings/can            (bitrate + per-channel IDs)
 *   POST /api/settings/can
 *   GET  /api/settings/logging-enabled
 *   POST /api/settings/logging-enabled
 *
 * /api/settings/can replaces the older /api/settings/can-bitrate (removed).
 * See docs/design/can-id-configuration.md.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "can_manager.h"
#include "log_ring.h"
#include "http_server_helpers.h"

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
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;

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
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;

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

/* ---- /api/settings/can — combined bitrate + per-channel IDs -------------- *
 *
 * GET returns the full current configuration.  POST accepts any subset of the
 * same shape; missing fields fall back to current values.  The merged set is
 * fully validated (range + IDE + cross-channel collision) before any value is
 * persisted — all-or-nothing.  See docs/design/can-id-configuration.md §5/§6.
 */

/* Channel keys on the JSON wire — order matches can_channel_id_t. */
static const char *CHANNEL_JSON_KEYS[CAN_CH_COUNT] = {
    [CAN_CH_LOGGING_CMD]   = "logging_cmd",
    [CAN_CH_CAM_STATUS]    = "cam_status",
    [CAN_CH_GPS_UTC]       = "gps_utc",
    [CAN_CH_SHUTDOWN_REQ]  = "shutdown_req",
};

static cJSON *build_can_response(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "bitrate_bps", (double)can_manager_get_bitrate());

    cJSON *channels = cJSON_AddObjectToObject(root, "channels");
    for (int i = 0; i < CAN_CH_COUNT; i++) {
        can_channel_t v;
        can_manager_get_channel((can_channel_id_t)i, &v);
        cJSON *ch = cJSON_AddObjectToObject(channels, CHANNEL_JSON_KEYS[i]);
        cJSON_AddStringToObject(ch, "ide", v.extended ? "ext" : "std");
        cJSON_AddNumberToObject(ch, "id", (double)v.id);
    }
    return root;
}

static esp_err_t handler_get_can(httpd_req_t *req)
{
    cJSON *root = build_can_response();
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc failed");
        return ESP_FAIL;
    }
    send_json(req, str);
    cJSON_free(str);
    return ESP_OK;
}

/* Validate a candidate (ide, id) per §5 step 4. */
static bool channel_value_in_range(bool extended, uint32_t id)
{
    if (id < 0x008u) return false;
    return extended ? (id <= 0x1FFFFFFFu) : (id <= 0x7FFu);
}

static bool bitrate_in_allowlist(uint32_t bps)
{
    static const uint32_t allowed[] = {
        50000u, 100000u, 125000u, 250000u, 500000u, 1000000u,
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (allowed[i] == bps) return true;
    }
    return false;
}

/* Pull a channel object out of POST body, validating shape/range as it goes.
 * On success: *out is the validated value, *touched = true.
 * On absence: *touched = false, *out untouched.
 * On error:   sends 400 with err_buf populated, returns false. */
static bool parse_channel_field(cJSON *channels_obj,
                                 const char *key,
                                 httpd_req_t *req,
                                 can_channel_t *out,
                                 bool *touched)
{
    *touched = false;
    cJSON *ch = cJSON_GetObjectItem(channels_obj, key);
    if (!ch) return true;                                /* absent — OK */

    if (!cJSON_IsObject(ch)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s must be object", key);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return false;
    }

    cJSON *ide_item = cJSON_GetObjectItem(ch, "ide");
    cJSON *id_item  = cJSON_GetObjectItem(ch, "id");
    if (!cJSON_IsString(ide_item) || !cJSON_IsNumber(id_item)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s requires ide+id", key);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return false;
    }

    const char *ide_str = cJSON_GetStringValue(ide_item);
    bool extended;
    if (strcmp(ide_str, "std") == 0) {
        extended = false;
    } else if (strcmp(ide_str, "ext") == 0) {
        extended = true;
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "invalid ide for %s", key);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return false;
    }

    double id_num = cJSON_GetNumberValue(id_item);
    if (id_num < 0 || id_num > (double)0x1FFFFFFFu || id_num != (double)(uint32_t)id_num) {
        char msg[64];
        snprintf(msg, sizeof(msg), "invalid id for %s", key);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return false;
    }
    uint32_t id = (uint32_t)id_num;

    if (!channel_value_in_range(extended, id)) {
        char msg[80];
        snprintf(msg, sizeof(msg), "%s id out of range for ide", key);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return false;
    }

    out->extended = extended;
    out->id       = id;
    *touched      = true;
    return true;
}

static esp_err_t handler_post_can(httpd_req_t *req)
{
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;

    /* Generous buffer for the full-shape body (~200 bytes when pretty-printed). */
    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* Snapshot current values so we can merge partial updates against them
     * and run the collision check on the merged set. */
    can_channel_t merged[CAN_CH_COUNT];
    for (int i = 0; i < CAN_CH_COUNT; i++) {
        can_manager_get_channel((can_channel_id_t)i, &merged[i]);
    }
    bool ch_touched[CAN_CH_COUNT] = { 0 };

    /* Bitrate (optional). */
    bool bitrate_touched = false;
    uint32_t bitrate_new = can_manager_get_bitrate();
    cJSON *bps_item = cJSON_GetObjectItem(root, "bitrate_bps");
    if (bps_item) {
        if (!cJSON_IsNumber(bps_item)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid bitrate_bps");
            return ESP_FAIL;
        }
        bitrate_new = (uint32_t)cJSON_GetNumberValue(bps_item);
        if (!bitrate_in_allowlist(bitrate_new)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid bitrate_bps");
            return ESP_FAIL;
        }
        bitrate_touched = true;
    }

    /* Per-channel (all optional). */
    cJSON *channels_obj = cJSON_GetObjectItem(root, "channels");
    if (channels_obj) {
        if (!cJSON_IsObject(channels_obj)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "channels must be object");
            return ESP_FAIL;
        }
        for (int i = 0; i < CAN_CH_COUNT; i++) {
            if (!parse_channel_field(channels_obj, CHANNEL_JSON_KEYS[i], req,
                                      &merged[i], &ch_touched[i])) {
                /* parse_channel_field already sent the 400 response. */
                cJSON_Delete(root);
                return ESP_FAIL;
            }
        }
    }
    cJSON_Delete(root);

    /* §5.6 — collision check on the merged set.  Cross-IDE same-ID collision
     * is intentional (see design §8.6); compare (extended, id) as a tuple. */
    for (int i = 0; i < CAN_CH_COUNT; i++) {
        for (int j = i + 1; j < CAN_CH_COUNT; j++) {
            if (merged[i].id == merged[j].id &&
                merged[i].extended == merged[j].extended) {
                char msg[80];
                snprintf(msg, sizeof(msg), "channel id collision: %s and %s",
                         CHANNEL_JSON_KEYS[i], CHANNEL_JSON_KEYS[j]);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
                return ESP_FAIL;
            }
        }
    }

    /* All-or-nothing persist.  Each setter rewrites its own NVS key; partial
     * failure here is treated as 500 (no rollback — extremely unlikely with
     * NVS, and the in-memory snapshot stays consistent because writes touch
     * NVS only). */
    if (bitrate_touched) {
        esp_err_t err = can_manager_set_bitrate(bitrate_new);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set bitrate failed");
            return ESP_FAIL;
        }
    }
    for (int i = 0; i < CAN_CH_COUNT; i++) {
        if (!ch_touched[i]) continue;
        esp_err_t err = can_manager_set_channel((can_channel_id_t)i, merged[i]);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set channel failed");
            return ESP_FAIL;
        }
    }

    /* Reply with the full GET-shaped body so the UI can re-render without
     * a follow-up GET. */
    cJSON *resp = build_can_response();
    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!str) {
        send_json(req, "{}");
        return ESP_OK;
    }
    send_json(req, str);
    cJSON_free(str);
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
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;

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
        { .uri = "/api/settings/can",             .method = HTTP_GET,  .handler = handler_get_can             },
        { .uri = "/api/settings/can",             .method = HTTP_POST, .handler = handler_post_can            },
        { .uri = "/api/settings/logging-enabled", .method = HTTP_GET,  .handler = handler_get_logging_enabled },
        { .uri = "/api/settings/logging-enabled", .method = HTTP_POST, .handler = handler_post_logging_enabled},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}
