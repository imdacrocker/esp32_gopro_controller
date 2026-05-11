/*
 * ota_settings.c — shared NVS-backed persistence for OTA-related user
 * settings (currently just the release channel).
 *
 * Both apps link this; whichever app is running sees its own sdkconfig
 * (so CONFIG_OTA_ALLOW_DEV_CHANNEL applies per-app) but the underlying
 * NVS namespace and key are shared.
 */

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "ota_io.h"
#include "ota_io_priv.h"

static const char *TAG = "ota_settings";

#define NVS_KEY_CHANNEL    "channel"
#define DEFAULT_CHANNEL    "stable"

bool ota_io_channel_allowed(const char *channel)
{
    if (!channel) return false;
    if (strcmp(channel, "stable") == 0) return true;
    if (strcmp(channel, "beta")   == 0) return true;
#if CONFIG_OTA_ALLOW_DEV_CHANNEL
    if (strcmp(channel, "dev")    == 0) return true;
#endif
    return false;
}

void ota_io_get_channel(char *out_buf, size_t out_len)
{
    if (!out_buf || out_len == 0) return;

    nvs_handle_t h;
    if (nvs_open(OTA_IO_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        strncpy(out_buf, DEFAULT_CHANNEL, out_len - 1);
        out_buf[out_len - 1] = '\0';
        return;
    }

    size_t sz = out_len;
    esp_err_t err = nvs_get_str(h, NVS_KEY_CHANNEL, out_buf, &sz);
    nvs_close(h);

    if (err != ESP_OK || !ota_io_channel_allowed(out_buf)) {
        /* Missing key, read failure, or stored value disallowed in this
         * build (e.g. "dev" persisted by a dev build then loaded into a
         * release build) — fall back to default. */
        strncpy(out_buf, DEFAULT_CHANNEL, out_len - 1);
        out_buf[out_len - 1] = '\0';
    }
}

esp_err_t ota_io_set_channel(const char *channel)
{
    if (!ota_io_channel_allowed(channel)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(OTA_IO_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, NVS_KEY_CHANNEL, channel);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "channel persist failed: %s", esp_err_to_name(err));
    }
    return err;
}
