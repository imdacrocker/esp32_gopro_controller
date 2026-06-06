/*
 * Internal helpers shared by ota_writer.c and storage_writer.c.
 * Not exposed via the public ota_io.h.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define OTA_IO_NVS_NAMESPACE "ota"

/* Lowercase hex encode; out_hex must hold at least 2*len + 1 bytes. */
void ota_io_hex_encode(const uint8_t *bin, size_t len, char *out_hex);

/*
 * Look up the stored SHA for the given partition label and compare to
 * `expected_hex`. Returns true on exact match (skip the write).
 *
 * Key format: "sha_<label>" — partition labels are bounded at 16 chars
 * (NVS key max is 15), so the longest label "ota_0"/"storage" stays under
 * the limit. Caller's responsibility to use a label that fits.
 */
bool ota_io_nvs_sha_matches(const char *partition_label, const char *expected_hex);

/* Persist the SHA for the given partition label. Best-effort; never fails
 * the calling write — a missing NVS record just means SHA-skip won't fire
 * next time. */
void ota_io_nvs_sha_store(const char *partition_label, const char *sha_hex);

/*
 * Delete the stored SHA for the given partition label. Best-effort. Called at
 * the start of a write (before the partition is erased) so that an aborted or
 * failed write cannot leave a stale SHA record — which would otherwise let a
 * later re-upload of the previous image SHA-skip against an erased partition.
 * The record is re-stored by *_writer_finish() on success.
 */
void ota_io_nvs_sha_delete(const char *partition_label);
