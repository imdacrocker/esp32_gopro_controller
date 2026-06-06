/*
 * log_ring.c — Implementation of the diagnostic log ring buffer.
 *
 * See docs/design/log-capture.md for the full design.
 */

#include "log_ring.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#ifndef CONFIG_LOG_RING_SIZE_KB
#define CONFIG_LOG_RING_SIZE_KB 64
#endif

#define RING_CAP        ((size_t)CONFIG_LOG_RING_SIZE_KB * 1024U)
#define LINE_BUF_BYTES  256

#define NVS_NAMESPACE   "logring"
#define NVS_KEY_ENABLED "enabled"

static char              s_ring[RING_CAP];   /* .bss */
static size_t            s_head;             /* next write index, [0, RING_CAP) */
static size_t            s_tail;             /* oldest byte index, [0, RING_CAP) */
static size_t            s_used;
static uint64_t          s_total_written;
static uint64_t          s_total_dropped;
static uint64_t          s_tail_byte_abs;    /* monotonic absolute position of s_tail; ticks per evicted byte */

/* REVIEW[log_ring:M1] (minor/contract): s_mtx is a non-recursive mutex taken
 * inside the esp_log vprintf hook (log_tee) with portMAX_DELAY. Two standing
 * invariants this creates are worth documenting/guarding:
 *   (a) No code may call ESP_LOG* while holding s_mtx — that would re-enter
 *       log_tee and self-deadlock. Currently respected (no logging happens
 *       under the lock), but it's an easy trap for future edits.
 *   (b) If ESP_LOG* is ever invoked from ISR context, xSemaphoreTake() here is
 *       illegal and will crash. ESP_LOG-from-ISR is already unsupported, but
 *       the hook turns it into a hard fault rather than the usual benign path.
 *       Consider a xPortInIsrContext() bail-out at the top of log_tee. */
static SemaphoreHandle_t s_mtx;
static atomic_bool       s_enabled    = true;   /* start ON to catch pre-NVS boot logs */
static atomic_bool       s_initialized = false;

/* ---- internal helpers (caller holds s_mtx) ------------------------------- */

static void evict_one_line_locked(void)
{
    /* Advance s_tail to the byte AFTER the next '\n' (inclusive of newline).
     * If no '\n' is found, drop the whole ring — should not happen in
     * practice since every line written ends in '\n'. */
    bool found_newline = false;
    while (s_used > 0) {
        char c = s_ring[s_tail];
        s_tail = (s_tail + 1U) % RING_CAP;
        s_used--;
        s_tail_byte_abs++;
        if (c == '\n') {
            found_newline = true;
            break;
        }
    }
    if (!found_newline) {
        /* defensive: ring contained no newlines, just drained */
        s_head = s_tail = 0;
        s_used = 0;
    }
    s_total_dropped++;
}

static void ring_write_locked(const char *buf, size_t n)
{
    if (n == 0 || n > RING_CAP) {
        /* Pathological line length: nothing useful we can do. Skip. */
        return;
    }

    while (s_used + n > RING_CAP) {
        evict_one_line_locked();
    }

    size_t first = RING_CAP - s_head;
    if (n <= first) {
        memcpy(&s_ring[s_head], buf, n);
    } else {
        memcpy(&s_ring[s_head], buf, first);
        memcpy(&s_ring[0], buf + first, n - first);
    }
    s_head = (s_head + n) % RING_CAP;
    s_used += n;
    s_total_written += n;
}

/* ---- vprintf hook -------------------------------------------------------- */

/* Strip ANSI CSI escape sequences (e.g. "\x1b[0;32m", "\x1b[0m") from buf
 * in-place. ESP-IDF wraps each log line in color codes when
 * CONFIG_LOG_COLORS=y; those bytes are unreadable noise in a downloaded
 * report. Returns the new length. */
static size_t strip_ansi_inplace(char *buf, size_t n)
{
    size_t w = 0;
    size_t r = 0;
    while (r < n) {
        if (buf[r] == '\x1b' && r + 1 < n && buf[r + 1] == '[') {
            r += 2;
            while (r < n && buf[r] != 'm') r++;
            if (r < n) r++;   /* consume the terminating 'm' */
        } else {
            buf[w++] = buf[r++];
        }
    }
    return w;
}

static int log_tee(const char *fmt, va_list args)
{
    /* Decide UART echo from the FIRST char of the assembled format string,
     * before doing any formatting work. That char is always a literal: the
     * level letter, or ESC (0x1b) for colored INFO/WARN/ERROR. DEBUG/VERBOSE
     * never carry color in ESP-IDF, so they always start with 'D'/'V' — which
     * is exactly the non-echoed set. (fmt[0] == buf[0] for the same reason,
     * so this matches the previous post-vsnprintf test.) */
    bool echo_to_uart = (fmt[0] != 'D' && fmt[0] != 'V');

    /* Capture disabled (the default): skip all formatting. Echo INFO/WARN/ERROR
     * straight through with the original fmt+args; drop DEBUG/VERBOSE entirely
     * rather than vsnprintf them just to discard the result. */
    if (!atomic_load(&s_enabled)) {
        return echo_to_uart ? vprintf(fmt, args) : 0;
    }

    char buf[LINE_BUF_BYTES];

    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);

    if (n <= 0) {
        return n;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }

    size_t ring_n = strip_ansi_inplace(buf, (size_t)n);
    if (s_mtx && xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        ring_write_locked(buf, ring_n);
        xSemaphoreGive(s_mtx);
    }

    /* UART echo uses the original fmt+args, so colors are preserved on the
     * developer console regardless of the ring stripping above. */
    if (echo_to_uart) {
        return vprintf(fmt, args);
    }
    return n;
}

/* ---- public API ---------------------------------------------------------- */

/* ESP-IDF internal tags that produce high-volume DEBUG output of no value to
 * end-user diagnostic reports. Held at INFO so they still report errors and
 * warnings but stop flooding the ring. Empirically, silencing this list cut
 * ring volume by ~85% in a 222-second sample (httpd_parse alone was 23 KB). */
static const char *const SILENCED_DEBUG_TAGS[] = {
    "httpd", "httpd_parse", "httpd_uri", "httpd_txrx", "httpd_sess",
    "vfs_calls",
    "efuse", "intr_alloc", "temperature_sensor_hal",
    "nvs",
    "event",   /* esp_event spams "no handlers registered" for every ESP_HTTP_SERVER_EVENT, ~3 lines per HTTP request */
};

esp_err_t log_ring_init(void)
{
    if (atomic_load(&s_initialized)) {
        return ESP_OK;
    }
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) {
        return ESP_ERR_NO_MEM;
    }
    esp_log_set_vprintf(log_tee);
    esp_log_level_set("*", ESP_LOG_DEBUG);
    for (size_t i = 0; i < sizeof(SILENCED_DEBUG_TAGS) / sizeof(SILENCED_DEBUG_TAGS[0]); i++) {
        esp_log_level_set(SILENCED_DEBUG_TAGS[i], ESP_LOG_INFO);
    }
    atomic_store(&s_initialized, true);
    return ESP_OK;
}

void log_ring_set_enabled(bool enabled)
{
    bool prev = atomic_exchange(&s_enabled, enabled);
    if (prev && !enabled) {
        log_ring_clear();
    }
}

bool log_ring_is_enabled(void)
{
    return atomic_load(&s_enabled);
}

esp_err_t log_ring_save_enabled_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_ENABLED, atomic_load(&s_enabled) ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void log_ring_load_persisted_enabled(void)
{
    nvs_handle_t h;
    uint8_t v = 0;   /* default OFF */
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_ENABLED, &v);
        nvs_close(h);
    }
    log_ring_set_enabled(v != 0);
}

esp_err_t log_ring_stream(log_ring_write_cb_t cb, void *arg)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    if (!s_mtx) return ESP_ERR_INVALID_STATE;

    /* Snapshot the absolute byte range once so the response is a point-in-time
     * view. New writes that arrive during the stream are not included. */
    uint64_t cursor;
    uint64_t end_abs;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    cursor  = s_tail_byte_abs;
    end_abs = s_tail_byte_abs + s_used;
    xSemaphoreGive(s_mtx);

    char tmp[2048];   /* stack-allocated chunk buffer; no malloc */

    while (cursor < end_abs) {
        size_t n_copied = 0;

        xSemaphoreTake(s_mtx, portMAX_DELAY);

        /* If concurrent writes evicted bytes past our cursor, skip ahead. */
        if (cursor < s_tail_byte_abs) {
            cursor = s_tail_byte_abs;
            if (cursor >= end_abs) {
                xSemaphoreGive(s_mtx);
                break;
            }
        }

        size_t ring_offset = (size_t)(cursor - s_tail_byte_abs);
        if (ring_offset >= s_used) {
            /* Defensive: cursor sits past current contents. Stop. */
            xSemaphoreGive(s_mtx);
            break;
        }

        size_t available_in_ring = s_used - ring_offset;
        size_t available_to_end  = (size_t)(end_abs - cursor);
        size_t want = available_in_ring < available_to_end
                          ? available_in_ring : available_to_end;
        if (want > sizeof(tmp)) want = sizeof(tmp);

        size_t start_idx = (s_tail + ring_offset) % RING_CAP;
        size_t first     = RING_CAP - start_idx;
        if (want <= first) {
            memcpy(tmp, &s_ring[start_idx], want);
        } else {
            memcpy(tmp, &s_ring[start_idx], first);
            memcpy(tmp + first, &s_ring[0], want - first);
        }
        n_copied = want;

        xSemaphoreGive(s_mtx);

        esp_err_t e = cb(arg, tmp, n_copied);
        if (e != ESP_OK) return e;
        cursor += n_copied;
    }

    return ESP_OK;
}

void log_ring_clear(void)
{
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_head = 0;
    s_tail = 0;
    s_used = 0;
    xSemaphoreGive(s_mtx);
}

void log_ring_get_stats(log_ring_stats_t *out)
{
    if (!out) return;
    out->capacity = RING_CAP;
    if (!s_mtx) {
        out->used = 0;
        out->bytes_written_total = 0;
        out->lines_dropped_total = 0;
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    out->used                = s_used;
    out->bytes_written_total = s_total_written;
    out->lines_dropped_total = s_total_dropped;
    xSemaphoreGive(s_mtx);
}
