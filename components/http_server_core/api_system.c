/*
 * api_system.c — System-level API handlers (§20.4).
 *
 * Endpoints:
 *   GET  /api/version
 *   GET  /api/logging-state
 *   GET  /api/utc
 *   GET  /api/auto-control
 *   POST /api/auto-control
 *   POST /api/reboot
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "can_manager.h"
#include "cam_core.h"
#include "ota_io.h"
#include "http_server_helpers.h"

static const char *TAG = "http_api_system";

/* ---- GET /api/version ---------------------------------------------------- *
 * Returns running firmware identity per docs/design/ota.md §6.
 *
 *   app:               main app version (esp_app_get_description)
 *   ui:                always equal to app per the §1 invariant — they ship
 *                      as a strictly-paired bundle, so we just echo the app
 *                      version rather than reading it from a file
 *   recovery:          factory partition's app desc; "unknown" if missing
 *   channel:           shared NVS ota/channel via ota_io
 *   running_partition: label (typically ota_0 or ota_1 in wireless mode)
 *   mode:              "wireless"
 *   product:           compile-time CONFIG_PRODUCT_VARIANT — slug for the
 *                      product variant (e.g. "wireless"). The browser uses
 *                      it to compose the variant-aware OTA route
 *                      (latest-<channel>-<product>/manifest.json).
 *   ota_base_url:      compile-time CONFIG_OTA_BASE_URL — the browser uses
 *                      this to compose the manifest URL (no /manifest.json
 *                      file needed on LittleFS)
 *   ota_repo_path:     compile-time CONFIG_OTA_REPO_PATH
 */

static void read_recovery_version(char *out, size_t out_len)
{
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    esp_app_desc_t desc;
    if (factory && esp_ota_get_partition_description(factory, &desc) == ESP_OK) {
        strncpy(out, desc.version, out_len - 1);
        out[out_len - 1] = '\0';
    } else {
        strncpy(out, "unknown", out_len - 1);
        out[out_len - 1] = '\0';
    }
}

static esp_err_t handler_version(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    char recovery[48];
    char channel[OTA_IO_CHANNEL_MAX];
    read_recovery_version(recovery, sizeof(recovery));
    ota_io_get_channel(channel, sizeof(channel));

    char buf[704];
    snprintf(buf, sizeof(buf),
             "{\"app\":\"%s\",\"ui\":\"%s\",\"recovery\":\"%s\","
             "\"channel\":\"%s\",\"running_partition\":\"%s\","
             "\"mode\":\"wireless\",\"product\":\"%s\","
             "\"build_date\":\"%s\",\"build_time\":\"%s\","
             "\"ota_base_url\":\"%s\",\"ota_repo_path\":\"%s\"}",
             app->version, app->version, recovery, channel,
             running ? running->label : "unknown",
             CONFIG_PRODUCT_VARIANT,
             app->date, app->time,
             CONFIG_OTA_BASE_URL, CONFIG_OTA_REPO_PATH);
    send_json(req, buf);
    return ESP_OK;
}

/* ---- GET /api/logging-state ---------------------------------------------- */

static esp_err_t handler_logging_state(httpd_req_t *req)
{
    static const char * const state_str[] = {
        [LOGGING_STATE_UNKNOWN]     = "unknown",
        [LOGGING_STATE_NOT_LOGGING] = "not_logging",
        [LOGGING_STATE_LOGGING]     = "logging",
    };
    can_logging_state_t state = can_manager_get_logging_state();
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\"}", state_str[state]);
    send_json(req, buf);
    return ESP_OK;
}

/* ---- GET /api/utc -------------------------------------------------------- */

static esp_err_t handler_utc(httpd_req_t *req)
{
    uint64_t utc_ms = 0;
    bool valid          = can_manager_get_utc_ms(&utc_ms);
    bool session_synced = can_manager_utc_is_session_synced();
    char buf[96];
    if (valid) {
        /* Apply timezone offset (§14.3). */
        int8_t tz = can_manager_get_tz_offset();
        utc_ms += (int64_t)tz * 3600LL * 1000LL;
        snprintf(buf, sizeof(buf),
                 "{\"valid\":true,\"session_synced\":%s,\"epoch_ms\":%llu}",
                 session_synced ? "true" : "false",
                 (unsigned long long)utc_ms);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"valid\":false,\"session_synced\":false,\"epoch_ms\":0}");
    }
    send_json(req, buf);
    return ESP_OK;
}

/* ---- GET /api/auto-control ----------------------------------------------- */

static esp_err_t handler_get_auto_control(httpd_req_t *req)
{
    bool enabled = cam_core_get_auto_control();
    send_json(req, enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
    return ESP_OK;
}

/* ---- POST /api/auto-control ---------------------------------------------- */

static esp_err_t handler_post_auto_control(httpd_req_t *req)
{
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;

    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *item = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'enabled'");
        return ESP_FAIL;
    }
    bool enabled = cJSON_IsTrue(item);
    cJSON_Delete(root);

    cam_core_set_auto_control(enabled);
    ESP_LOGI(TAG, "auto-control set to %s", enabled ? "on" : "off");

    send_json(req, enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
    return ESP_OK;
}

/* ---- POST /api/reboot ---------------------------------------------------- */

static esp_err_t handler_reboot(httpd_req_t *req)
{
    send_json(req, "{}");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_ota_mark_app_valid_cancel_rollback();   /* ignore ESP_ERR_NOT_SUPPORTED on factory */
    esp_restart();
    return ESP_OK;
}

/* ---- Registration -------------------------------------------------------- */

void api_system_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/version",       .method = HTTP_GET,  .handler = handler_version          },
        { .uri = "/api/logging-state", .method = HTTP_GET,  .handler = handler_logging_state    },
        { .uri = "/api/utc",           .method = HTTP_GET,  .handler = handler_utc              },
        { .uri = "/api/auto-control",  .method = HTTP_GET,  .handler = handler_get_auto_control },
        { .uri = "/api/auto-control",  .method = HTTP_POST, .handler = handler_post_auto_control},
        { .uri = "/api/reboot",        .method = HTTP_POST, .handler = handler_reboot           },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}
