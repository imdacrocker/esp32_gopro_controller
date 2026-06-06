/*
 * ota_writer.c — streams a new app image into the inactive OTA slot.
 *
 * Target partition is selected by esp_ota_get_next_update_partition(NULL):
 * recovery (running from factory) → ota_0; main (running from ota_0) →
 * ota_1; main (running from ota_1) → ota_0.
 */

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "ota_io.h"
#include "ota_io_priv.h"

static const char *TAG = "ota_writer";

struct ota_writer {
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    psa_hash_operation_t sha;
    char expected_sha[65];
    size_t bytes_written;
};

esp_err_t ota_writer_begin(const char *expected_sha_hex,
                            size_t expected_size,
                            ota_writer_t **out,
                            bool *out_skipped)
{
    *out = NULL;
    *out_skipped = false;

    if (!expected_sha_hex || strlen(expected_sha_hex) != 64) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "no inactive OTA partition (running=%s)",
                 esp_ota_get_running_partition() ?
                     esp_ota_get_running_partition()->label : "?");
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

    /* Invalidate the stored SHA BEFORE esp_ota_begin() erases the partition.
     * If this write is later aborted/fails, the erased slot no longer holds the
     * previous image, so leaving its SHA in NVS would let a re-upload of that
     * image SHA-skip against erased flash and commit a corrupt boot target.
     * finish() re-stores the SHA on success. */
    ota_io_nvs_sha_delete(part->label);

    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %ld", (long)ps);
        return ESP_FAIL;
    }

    ota_writer_t *w = calloc(1, sizeof(*w));
    if (!w) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_ota_begin(part, expected_size, &w->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        free(w);
        return err;
    }

    w->sha = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    ps = psa_hash_setup(&w->sha, PSA_ALG_SHA_256);
    if (ps != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_setup failed: %ld", (long)ps);
        esp_ota_abort(w->handle);
        free(w);
        return ESP_FAIL;
    }

    w->partition = part;
    strncpy(w->expected_sha, expected_sha_hex, 64);
    w->expected_sha[64] = '\0';

    *out = w;
    return ESP_OK;
}

esp_err_t ota_writer_write(ota_writer_t *w, const void *data, size_t len)
{
    if (!w) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_write(w->handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write at %zu failed: %s",
                 w->bytes_written, esp_err_to_name(err));
        return err;
    }
    /* REVIEW[ota_io:M1] (minor): psa_hash_update return value ignored (same in
     * storage_writer_write). Fails safe — a hashing error yields a wrong digest
     * that finish() catches as ESP_ERR_INVALID_CRC, rejecting the image — but
     * checking it would surface the real cause instead of a misleading SHA
     * mismatch. */
    psa_hash_update(&w->sha, data, len);
    w->bytes_written += len;
    return ESP_OK;
}

esp_err_t ota_writer_finish(ota_writer_t *w, char out_actual_sha_hex[65])
{
    if (!w) return ESP_ERR_INVALID_STATE;

    uint8_t digest[32];
    size_t digest_len = 0;
    psa_status_t ps = psa_hash_finish(&w->sha, digest, sizeof(digest), &digest_len);
    if (ps != PSA_SUCCESS || digest_len != 32) {
        ESP_LOGE(TAG, "psa_hash_finish failed: %ld len=%zu", (long)ps, digest_len);
        psa_hash_abort(&w->sha);
        esp_ota_abort(w->handle);
        free(w);
        return ESP_FAIL;
    }
    ota_io_hex_encode(digest, 32, out_actual_sha_hex);

    esp_err_t result = ESP_OK;
    if (strcmp(out_actual_sha_hex, w->expected_sha) != 0) {
        ESP_LOGW(TAG, "%s SHA mismatch: expected %s got %s",
                 w->partition->label, w->expected_sha, out_actual_sha_hex);
        esp_ota_abort(w->handle);
        result = ESP_ERR_INVALID_CRC;
    } else {
        esp_err_t err = esp_ota_end(w->handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            result = err;
        } else {
            ota_io_nvs_sha_store(w->partition->label, out_actual_sha_hex);
            ESP_LOGI(TAG, "%s wrote %zu bytes, sha %s",
                     w->partition->label, w->bytes_written, out_actual_sha_hex);
        }
    }

    free(w);
    return result;
}

void ota_writer_abort(ota_writer_t *w)
{
    if (!w) return;
    psa_hash_abort(&w->sha);
    esp_ota_abort(w->handle);
    free(w);
}
