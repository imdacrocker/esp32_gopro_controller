/*
 * ota_io — shared OTA image-writing primitives used by both the main app
 * (in-place A/B updates) and the recovery app (factory-mediated updates).
 *
 * Implements the §6 contract from ota_design.md: streaming uploads with
 * SHA-256 verification, NVS-backed SHA-skip, and boot-partition switching.
 *
 * The writers are stateful; one instance per upload. Begin → many writes →
 * Finish (or Abort). All allocations are freed by Finish/Abort.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* ---- App writer --------------------------------------------------------- *
 * Streams a new app image into the inactive OTA slot returned by
 * esp_ota_get_next_update_partition(NULL):
 *   - From factory (recovery)        → ota_0
 *   - From ota_0 (main, slot A)      → ota_1
 *   - From ota_1 (main, slot B)      → ota_0
 */

typedef struct ota_writer ota_writer_t;

/*
 * Validate headers, look up target partition, check NVS for SHA-skip.
 * On skip, *out_skipped=true, *out=NULL — caller drains body and replies OK.
 * On non-skip, *out is a valid handle; caller must finish() or abort().
 */
esp_err_t ota_writer_begin(const char *expected_sha_hex,
                            size_t expected_size,
                            ota_writer_t **out,
                            bool *out_skipped);

esp_err_t ota_writer_write(ota_writer_t *w, const void *data, size_t len);

/*
 * Verifies SHA, finalizes esp_ota handle, persists SHA into NVS for skip.
 * Frees the writer regardless of outcome. Returns ESP_ERR_INVALID_CRC on
 * SHA mismatch.
 */
esp_err_t ota_writer_finish(ota_writer_t *w, char out_actual_sha_hex[65]);

void ota_writer_abort(ota_writer_t *w);

/* ---- Storage (LittleFS) writer ----------------------------------------- *
 * Streams a complete LittleFS image into the `storage` data partition.
 * Same SHA-skip semantics as ota_writer_begin().
 */

typedef struct storage_writer storage_writer_t;

esp_err_t storage_writer_begin(const char *expected_sha_hex,
                                size_t expected_size,
                                storage_writer_t **out,
                                bool *out_skipped);

esp_err_t storage_writer_write(storage_writer_t *w, const void *data, size_t len);

esp_err_t storage_writer_finish(storage_writer_t *w, char out_actual_sha_hex[65]);

void storage_writer_abort(storage_writer_t *w);

/* ---- Boot-partition helpers -------------------------------------------- */

/*
 * Set the partition that ota_writer would target as the boot partition.
 * Used by /api/ota/commit on both apps. Caller is responsible for
 * esp_restart().
 *
 * Writes `out_label` (must be ≥17 chars) with the chosen partition's label
 * for inclusion in the JSON response.
 *
 * Returns ESP_ERR_NOT_FOUND if no inactive OTA slot is available.
 */
esp_err_t ota_io_commit_pending(char out_label[17]);

/*
 * Set boot to the `factory` partition (recovery). Used by main's
 * /api/ota/reboot-recovery. Returns ESP_ERR_NOT_FOUND if factory is missing.
 */
esp_err_t ota_io_set_boot_factory(void);

/*
 * Set boot to a valid OTA-app partition. Used by recovery's /api/ota/boot-main.
 * Iterates ota_0/ota_1, picks the first one with a valid app header. If both
 * have valid apps, prefers the one with the more recent secure_version /
 * build timestamp.
 *
 * Writes `out_label` with the chosen partition's label.
 * Returns ESP_ERR_NOT_FOUND if no OTA slot holds a valid app.
 */
esp_err_t ota_io_set_boot_main(char out_label[17]);

/* ---- Channel settings (shared NVS, no HTTP) ---------------------------- *
 * Both apps read/write the same NVS key (`ota/channel`). A value set in
 * either UI is immediately visible to the other after restart-into-the-other.
 *
 * Allowlist always includes "stable" and "beta"; "dev" is appended if any
 * call site's CONFIG_OTA_ALLOW_DEV_CHANNEL is set (see ota_io_channel_allowed).
 */

#define OTA_IO_CHANNEL_MAX 8  /* "stable" / "beta" / "dev" + NUL */

/* Copy current channel into out_buf (NUL-terminated). Falls back to "stable"
 * if NVS is unreadable or the key is absent. Always succeeds. */
void ota_io_get_channel(char *out_buf, size_t out_len);

/* Persist a channel string to NVS. Returns ESP_ERR_INVALID_ARG if the
 * channel isn't in the allowlist for this build. Does NOT trigger any
 * downstream behavior — takes effect at next manifest fetch. */
esp_err_t ota_io_set_channel(const char *channel);

/* Returns true if `channel` is one of the strings this build allows. */
bool ota_io_channel_allowed(const char *channel);
