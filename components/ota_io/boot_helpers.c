/*
 * boot_helpers.c — partition lookup, NVS SHA persistence, boot-target
 * switching. Used by both writers and by the HTTP commit/boot endpoints.
 */

#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "nvs.h"

#include "ota_io.h"
#include "app_date_compare.h"
#include "ota_io_priv.h"

static const char *TAG = "ota_io";

/* ---- private helpers (declared in ota_io_priv.h) ----------------------- */

void ota_io_hex_encode(const uint8_t *bin, size_t len, char *out_hex)
{
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out_hex[i * 2]     = H[bin[i] >> 4];
        out_hex[i * 2 + 1] = H[bin[i] & 0xF];
    }
    out_hex[len * 2] = '\0';
}

/* NVS key max is 15 chars. "sha_" (4) + label (≤8) = ≤12. Safe. */
static void nvs_key_for(const char *label, char out_key[16])
{
    snprintf(out_key, 16, "sha_%s", label);
}

bool ota_io_nvs_sha_matches(const char *partition_label, const char *expected_hex)
{
    nvs_handle_t h;
    if (nvs_open(OTA_IO_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    char key[16];
    nvs_key_for(partition_label, key);

    char stored[65] = {0};
    size_t sz = sizeof(stored);
    esp_err_t err = nvs_get_str(h, key, stored, &sz);
    nvs_close(h);
    return err == ESP_OK && strcmp(stored, expected_hex) == 0;
}

void ota_io_nvs_sha_store(const char *partition_label, const char *sha_hex)
{
    nvs_handle_t h;
    if (nvs_open(OTA_IO_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    nvs_key_for(partition_label, key);
    nvs_set_str(h, key, sha_hex);
    nvs_commit(h);
    nvs_close(h);
}

void ota_io_nvs_sha_delete(const char *partition_label)
{
    nvs_handle_t h;
    if (nvs_open(OTA_IO_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    nvs_key_for(partition_label, key);
    /* ESP_ERR_NVS_NOT_FOUND (no prior record) is fine — nothing to clear. */
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ---- public boot helpers ----------------------------------------------- */

esp_err_t ota_io_commit_pending(char out_label[17])
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "no inactive OTA partition to commit");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_ota_set_boot_partition(part);
    if (err == ESP_OK) {
        strncpy(out_label, part->label, 16);
        out_label[16] = '\0';
        ESP_LOGI(TAG, "boot partition set to %s", out_label);
    } else {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s) failed: %s",
                 part->label, esp_err_to_name(err));
    }
    return err;
}

esp_err_t ota_io_set_boot_factory(void)
{
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) {
        ESP_LOGE(TAG, "factory partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(factory) failed: %s",
                 esp_err_to_name(err));
    }
    return err;
}

/* Return 1 if a's build is more recent than b's, else 0.
 *
 * Compares secure_version first (monotonic anti-rollback counter); on a
 * tie falls back to a properly parsed __DATE__ / __TIME__ comparison via
 * app_date_time_compare.  An earlier version used strncmp directly on
 * the "MMM DD YYYY" date string, which sorts months alphabetically rather
 * than calendrically — so e.g. "Nov 30 2024" > "Jan 15 2025" lex, even
 * though Jan 2025 is calendrically newer.  That caused recovery's
 * /api/ota/boot-main to select the OLDER of two valid OTA slots whenever
 * the two build months sat on opposite sides of an alphabetical-vs-
 * calendar regression — which on continuous-development builds is roughly
 * half of all month transitions.
 *
 * Note: this only affects the recovery boot-main code path; normal OTA
 * commit goes through esp_ota_set_boot_partition() and never calls here.
 *
 * The build system does not currently bump CONFIG_APP_SECURE_VERSION per
 * build, so every pair of images defaults to a secure_version tie and the
 * date/time comparator is the operative test.  If a future build process
 * starts bumping secure_version, this function still does the right thing
 * — the tiebreaker just becomes vestigial. */
static int app_desc_is_newer(const esp_app_desc_t *a, const esp_app_desc_t *b)
{
    if (a->secure_version != b->secure_version) {
        return a->secure_version > b->secure_version;
    }
    return app_date_time_compare(a->date, a->time,
                                  b->date, b->time) > 0;
}

esp_err_t ota_io_set_boot_main(char out_label[17])
{
    const esp_partition_t *winner = NULL;
    esp_app_desc_t winner_desc;

    const esp_partition_subtype_t subtypes[] = {
        ESP_PARTITION_SUBTYPE_APP_OTA_0,
        ESP_PARTITION_SUBTYPE_APP_OTA_1,
    };
    for (size_t i = 0; i < sizeof(subtypes) / sizeof(subtypes[0]); i++) {
        const esp_partition_t *cand = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, subtypes[i], NULL);
        if (!cand) continue;
        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(cand, &desc) != ESP_OK) continue;
        if (!winner || app_desc_is_newer(&desc, &winner_desc)) {
            winner = cand;
            winner_desc = desc;
        }
    }

    if (!winner) {
        ESP_LOGW(TAG, "no OTA slot holds a valid app");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "boot-main: %s holds '%s' v%s",
             winner->label, winner_desc.project_name, winner_desc.version);
    esp_err_t err = esp_ota_set_boot_partition(winner);
    if (err == ESP_OK) {
        strncpy(out_label, winner->label, 16);
        out_label[16] = '\0';
    } else {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s) failed: %s",
                 winner->label, esp_err_to_name(err));
    }
    return err;
}
