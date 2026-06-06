/*
 * storage_writer.c — streams a LittleFS image into the `storage` data
 * partition with rolling SHA-256 verification (docs/design/ota.md §6
 * "POST /api/ota/upload-ui").
 */

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "ota_io.h"
#include "ota_io_priv.h"

static const char *TAG = "storage_writer";

#define ERASE_SECTOR  4096u

struct storage_writer {
    const esp_partition_t *partition;
    psa_hash_operation_t sha;
    char expected_sha[65];
    size_t bytes_written;
};

esp_err_t storage_writer_begin(const char *expected_sha_hex,
                                size_t expected_size,
                                storage_writer_t **out,
                                bool *out_skipped)
{
    *out = NULL;
    *out_skipped = false;

    if (!expected_sha_hex || strlen(expected_sha_hex) != 64) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    if (!part) {
        ESP_LOGE(TAG, "storage partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "target: %s @ 0x%08" PRIx32 " size=%" PRIu32,
             part->label, part->address, part->size);

    if (expected_size == 0 || expected_size > part->size) {
        ESP_LOGW(TAG, "size %zu invalid for partition (%" PRIu32 ")",
                 expected_size, part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (ota_io_nvs_sha_matches(part->label, expected_sha_hex)) {
        ESP_LOGI(TAG, "%s SHA matches NVS — skipping write", part->label);
        *out_skipped = true;
        return ESP_OK;
    }

    /* Invalidate the stored SHA BEFORE the erase below destroys the partition
     * contents. Otherwise an aborted write would leave a stale SHA that lets a
     * re-upload of the previous image SHA-skip against an erased partition (and
     * a corrupt LittleFS has no bootloader-rollback net). finish() re-stores it
     * on success. */
    ota_io_nvs_sha_delete(part->label);

    /* Erase exactly the range we'll touch (rounded up to sector size). */
    size_t erase_size = ((expected_size + ERASE_SECTOR - 1) / ERASE_SECTOR) * ERASE_SECTOR;
    if (erase_size > part->size) erase_size = part->size;
    esp_err_t err = esp_partition_erase_range(part, 0, erase_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase 0..%zu failed: %s", erase_size, esp_err_to_name(err));
        return err;
    }

    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %ld", (long)ps);
        return ESP_FAIL;
    }

    storage_writer_t *w = calloc(1, sizeof(*w));
    if (!w) return ESP_ERR_NO_MEM;

    w->sha = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    ps = psa_hash_setup(&w->sha, PSA_ALG_SHA_256);
    if (ps != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_setup failed: %ld", (long)ps);
        free(w);
        return ESP_FAIL;
    }

    w->partition = part;
    strncpy(w->expected_sha, expected_sha_hex, 64);
    w->expected_sha[64] = '\0';

    *out = w;
    return ESP_OK;
}

esp_err_t storage_writer_write(storage_writer_t *w, const void *data, size_t len)
{
    if (!w) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_partition_write(w->partition, w->bytes_written, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_write at %zu failed: %s",
                 w->bytes_written, esp_err_to_name(err));
        return err;
    }
    psa_hash_update(&w->sha, data, len);
    w->bytes_written += len;
    return ESP_OK;
}

esp_err_t storage_writer_finish(storage_writer_t *w, char out_actual_sha_hex[65])
{
    if (!w) return ESP_ERR_INVALID_STATE;

    uint8_t digest[32];
    size_t digest_len = 0;
    psa_status_t ps = psa_hash_finish(&w->sha, digest, sizeof(digest), &digest_len);
    if (ps != PSA_SUCCESS || digest_len != 32) {
        ESP_LOGE(TAG, "psa_hash_finish failed: %ld len=%zu", (long)ps, digest_len);
        psa_hash_abort(&w->sha);
        free(w);
        return ESP_FAIL;
    }
    ota_io_hex_encode(digest, 32, out_actual_sha_hex);

    esp_err_t result = ESP_OK;
    if (strcmp(out_actual_sha_hex, w->expected_sha) != 0) {
        ESP_LOGW(TAG, "%s SHA mismatch: expected %s got %s",
                 w->partition->label, w->expected_sha, out_actual_sha_hex);
        result = ESP_ERR_INVALID_CRC;
    } else {
        ota_io_nvs_sha_store(w->partition->label, out_actual_sha_hex);
        ESP_LOGI(TAG, "%s wrote %zu bytes, sha %s",
                 w->partition->label, w->bytes_written, out_actual_sha_hex);
    }

    free(w);
    return result;
}

void storage_writer_abort(storage_writer_t *w)
{
    if (!w) return;
    psa_hash_abort(&w->sha);
    free(w);
}
