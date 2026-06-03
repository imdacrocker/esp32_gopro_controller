/*
 * log_ring.h — In-RAM diagnostic log ring buffer.
 *
 * Captures ESP_LOG output to a fixed-size circular buffer in internal SRAM
 * for user-facing diagnostic reports. See docs/design/log-capture.md.
 *
 * Boot sequence (from app_main):
 *   log_ring_init();                       // first call, before NVS
 *   nvs_flash_init();
 *   log_ring_load_persisted_enabled();     // apply the saved toggle
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Install the esp_log vprintf hook and raise the runtime level to DEBUG so
 * the hook receives DEBUG (and VERBOSE, if compiled in) lines. The ring
 * starts ENABLED so any logs emitted before NVS is up are captured. Safe to
 * call as the very first line of app_main(). Idempotent.
 */
esp_err_t log_ring_init(void);

/*
 * Enable or disable ring capture at runtime. ON -> OFF additionally clears
 * the ring (atomically with the flag flip). UART echo is unaffected by this
 * toggle. Does NOT touch NVS; use log_ring_save_enabled_to_nvs() to persist.
 */
void log_ring_set_enabled(bool enabled);

bool log_ring_is_enabled(void);

/* Persist the current enable state to NVS (namespace "logring", key "enabled"). */
esp_err_t log_ring_save_enabled_to_nvs(void);

/*
 * Read the persisted enable state from NVS and apply it via
 * log_ring_set_enabled(). A missing key defaults to OFF. Call once at boot
 * immediately after nvs_flash_init().
 */
void log_ring_load_persisted_enabled(void);

/*
 * Stream the current ring contents to the caller via a callback. Holds the
 * internal mutex only during each per-chunk copy (a 2 KB stack buffer is
 * used), so log writes can interleave with the stream — any bytes evicted
 * mid-stream are skipped silently. The byte range is fixed at the call
 * start: new writes that arrive during the stream are NOT included.
 *
 * The callback receives chunks of up to ~2 KB. Return ESP_OK to continue;
 * return any other value to abort, which log_ring_stream propagates back.
 *
 * No heap allocation. Cannot fail with NO_MEM. Safe to call against a full
 * 64 KB ring even when the heap is fragmented.
 */
typedef esp_err_t (*log_ring_write_cb_t)(void *arg, const char *data, size_t n);

esp_err_t log_ring_stream(log_ring_write_cb_t cb, void *arg);

/* Clear the ring. Lifetime counters (bytes_written_total, lines_dropped_total)
 * are preserved. */
void log_ring_clear(void);

typedef struct {
    size_t   capacity;
    size_t   used;
    uint64_t bytes_written_total;
    uint64_t lines_dropped_total;
} log_ring_stats_t;

void log_ring_get_stats(log_ring_stats_t *out);

#ifdef __cplusplus
}
#endif
