/*
 * cam_core.c — Variant-agnostic camera control core.
 *
 * Owns the recording-intent engine, mismatch-poll timer, broadcast-driver
 * dispatch dedup, CAN-state translation, sleep/teardown helpers, and the
 * global auto-control flag.  All ESP-IDF transport (BLE, WiFi-RC, future
 * USB-host etc.) sits below this seam as a driver implementing
 * camera_driver_t (cam_core.h).  All product variants sit above this seam.
 *
 * Locking: a single recursive mutex (s_mutex) guards the slot registry
 * and every cam_core_slot_t field.  Driver methods are invoked with the
 * lock held — they must be non-blocking (post work to their own task
 * queues).  The mutex is recursive so a driver method that incidentally
 * calls back into cam_core (e.g. start_recording → eventually toggles
 * ready) does not self-deadlock.
 *
 * Timer lifecycle: cam_core_slot_set_ready(false) deletes the timer
 * AFTER releasing the mutex.  esp_timer_delete blocks until any in-flight
 * callback returns; the callback acquires the mutex; holding it during
 * delete would deadlock.
 *
 * See docs/multi-variant-restructure-plan.md §4 and
 * docs/design/camera-manager.md §13 for the wider design.
 */

#include "cam_core.h"

#include <assert.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cam_core";

/* Mismatch poll interval (§13.3 default; per-model tuning deferred). */
#define STATUS_POLL_INTERVAL_MS   2000

/* Grace window after a shutter command during which the mismatch poll
 * suppresses dispatch comparison.  Both Hero4 (UDP `st` polling) and
 * Hero 9+ (BLE status notifications) take several seconds to report a
 * state change after a SetShutter; 10 s comfortably covers both plus the
 * worst-case status-poll scheduling. */
#define RECORDING_STATUS_GRACE_MS 10000

static cam_core_slot_t  *s_slots[CAMERA_MAX_SLOTS];
static int               s_slot_count;
static bool              s_auto_control = true;
static SemaphoreHandle_t s_mutex;

static inline bool valid_idx(int i) { return i >= 0 && i < CAMERA_MAX_SLOTS; }
/* Recursive so driver callbacks invoked under the lock can re-enter
 * cam_core safely (mirrors the historical camera_manager pattern). */
static inline void lock(void)   { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

static inline int64_t grace_deadline(void)
{
    return esp_timer_get_time() + (int64_t)RECORDING_STATUS_GRACE_MS * 1000;
}

/* ================================================================
 * Init / registry
 * ================================================================ */

void cam_core_init(void)
{
    if (s_mutex) return;   /* idempotent */
    s_mutex = xSemaphoreCreateRecursiveMutex();
    assert(s_mutex);
    memset(s_slots, 0, sizeof(s_slots));
    s_slot_count   = 0;
    s_auto_control = true;
    ESP_LOGI(TAG, "init complete (%d slot capacity)", CAMERA_MAX_SLOTS);
}

esp_err_t cam_core_register_slot(int idx, cam_core_slot_t *slot)
{
    if (!valid_idx(idx) || !slot) return ESP_ERR_INVALID_ARG;
    lock();
    s_slots[idx] = slot;
    if (s_slot_count <= idx) s_slot_count = idx + 1;
    unlock();
    return ESP_OK;
}

void cam_core_unregister_slot(int idx)
{
    if (!valid_idx(idx)) return;

    /* Stop the poll timer first (see file-header note on timer lifecycle). */
    cam_core_teardown_slot(idx);

    lock();
    s_slots[idx] = NULL;
    /* Recompute the exclusive bound — shrinking-on-removal mirrors the
     * pre-split camera_manager_remove_slot semantics. */
    int new_count = 0;
    for (int i = CAMERA_MAX_SLOTS - 1; i >= 0; i--) {
        if (s_slots[i]) { new_count = i + 1; break; }
    }
    s_slot_count = new_count;
    unlock();
}

int cam_core_slot_count(void)
{
    /* Atomic int read; locking would add nothing useful. */
    return s_slot_count;
}

bool cam_core_slot_active(int idx)
{
    if (!valid_idx(idx)) return false;
    lock();
    bool active = (s_slots[idx] != NULL);
    unlock();
    return active;
}

/* ================================================================
 * Driver attach
 * ================================================================ */

esp_err_t cam_core_slot_attach_driver(int                       idx,
                                       const camera_driver_t    *driver,
                                       void                     *ctx,
                                       bool                      requires_ble)
{
    if (!valid_idx(idx) || !driver) return ESP_ERR_INVALID_ARG;
    /* Enforce the "always non-NULL" vtable contract documented in
     * cam_core.h — get_recording_status is called from the poll timer
     * with only a `driver != NULL` guard, and start/stop are called from
     * the dispatch paths the same way.  Catch contract violations at
     * attach time rather than crashing on first poll. */
    assert(driver->start_recording);
    assert(driver->stop_recording);
    assert(driver->get_recording_status);

    lock();
    cam_core_slot_t *sl = s_slots[idx];
    if (!sl) { unlock(); return ESP_ERR_INVALID_STATE; }
    sl->driver       = driver;
    sl->driver_ctx   = ctx;
    sl->requires_ble = requires_ble;
    unlock();
    return ESP_OK;
}

bool cam_core_slot_has_driver(int idx)
{
    if (!valid_idx(idx)) return false;
    lock();
    cam_core_slot_t *sl = s_slots[idx];
    bool has = (sl && sl->driver);
    unlock();
    return has;
}

bool cam_core_slot_requires_ble(int idx)
{
    if (!valid_idx(idx)) return false;
    lock();
    cam_core_slot_t *sl = s_slots[idx];
    bool req = (sl && sl->driver && sl->requires_ble);
    unlock();
    return req;
}

/* ================================================================
 * Mismatch poll timer (§13.3 / §13.4)
 * ================================================================ */

static void poll_timer_cb(void *arg)
{
    int idx = (int)(intptr_t)arg;

    lock();
    cam_core_slot_t *sl = s_slots[idx];
    if (!sl || !sl->driver || !sl->ready) {
        unlock();
        return;
    }

    camera_recording_status_t status =
        sl->driver->get_recording_status(sl->driver_ctx);

    /* Grace expires naturally when the deadline passes — no per-tick reset. */
    bool grace_active = (esp_timer_get_time() < sl->grace_until_us);

    mismatch_action_t action =
        mismatch_step(sl->desired, status, grace_active);

    /* Dispatch the driver call WHILE the lock is still held.  An earlier
     * version snapshotted the driver pointer + ctx, unlocked, and then
     * called through — that opened a TOCTOU window in which a detach /
     * teardown could null sl->driver / free driver_ctx between the
     * snapshot and the call, producing a NULL deref or use-after-free.
     * The driver methods queue work to their own task queues and return
     * quickly, so the global lock is held only briefly.  The mutex is
     * recursive so any incidental re-entry from the driver is safe. */
    if (action == MISMATCH_ACTION_START) {
        ESP_LOGI(TAG, "slot %d: mismatch — issuing start_recording", idx);
        sl->driver->start_recording(sl->driver_ctx);
        sl->grace_until_us = grace_deadline();
    } else if (action == MISMATCH_ACTION_STOP) {
        ESP_LOGI(TAG, "slot %d: mismatch — issuing stop_recording", idx);
        sl->driver->stop_recording(sl->driver_ctx);
        sl->grace_until_us = grace_deadline();
    }
    unlock();
}

/* Create + start the periodic poll timer.  Must be called with the cam_core
 * mutex held (so sl->poll_timer can be written safely). */
static void start_poll_timer_locked(cam_core_slot_t *sl, int idx)
{
    if (sl->poll_timer) return;
    esp_timer_create_args_t args = {
        .callback        = poll_timer_cb,
        .arg             = (void *)(intptr_t)idx,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "cam_poll",
    };
    esp_timer_handle_t h;
    if (esp_timer_create(&args, &h) == ESP_OK) {
        sl->poll_timer = h;
        esp_timer_start_periodic(h, (uint64_t)STATUS_POLL_INTERVAL_MS * 1000ULL);
    } else {
        ESP_LOGE(TAG, "slot %d: failed to create poll timer", idx);
    }
}

void cam_core_slot_set_ready(int idx, bool ready)
{
    if (!valid_idx(idx)) return;

    esp_timer_handle_t to_stop = NULL;

    lock();
    cam_core_slot_t *sl = s_slots[idx];
    if (!sl) { unlock(); return; }

    bool was_ready = sl->ready;
    if (was_ready == ready) { unlock(); return; }
    sl->ready = ready;

    if (!ready) {
        sl->grace_until_us = 0;
        to_stop            = sl->poll_timer;
        sl->poll_timer     = NULL;
    } else {
        start_poll_timer_locked(sl, idx);
    }
    unlock();

    if (to_stop) {
        /* esp_timer_delete blocks until any in-progress callback returns;
         * the callback acquires the mutex, so this MUST run unlocked. */
        esp_timer_stop(to_stop);
        esp_timer_delete(to_stop);
    }
}

/* ================================================================
 * Recording intent (§13)
 * ================================================================ */

void cam_core_set_desired_slot(int idx, desired_recording_t intent)
{
    if (!valid_idx(idx)) return;
    lock();
    cam_core_slot_t *sl = s_slots[idx];
    if (!sl) { unlock(); return; }

    bool transitioned = (sl->desired != intent);
    sl->desired = intent;
    if (transitioned && intent != DESIRED_RECORDING_UNKNOWN
        && sl->driver && sl->ready) {
        sl->grace_until_us = grace_deadline();
        if (intent == DESIRED_RECORDING_START) {
            ESP_LOGI(TAG, "slot %d: immediate dispatch — start_recording", idx);
            sl->driver->start_recording(sl->driver_ctx);
        } else { /* DESIRED_RECORDING_STOP */
            ESP_LOGI(TAG, "slot %d: immediate dispatch — stop_recording", idx);
            sl->driver->stop_recording(sl->driver_ctx);
        }
    }
    unlock();
}

void cam_core_set_desired_all(desired_recording_t intent)
{
    /* Tracks broadcast-style drivers already enrolled in this dispatch wave.
     * For these drivers, only the FIRST slot encountered triggers a call;
     * subsequent slots have intent + grace updated but no per-slot dispatch
     * (the broadcast already covered them). */
    const camera_driver_t *seen_broadcast[CAMERA_MAX_SLOTS];
    int                    seen_count = 0;

    lock();
    for (int i = 0; i < s_slot_count; i++) {
        cam_core_slot_t *sl = s_slots[i];
        if (!sl) continue;
        if (sl->desired == intent) continue;
        sl->desired = intent;
        if (intent == DESIRED_RECORDING_UNKNOWN) continue;
        if (!sl->driver || !sl->ready) continue;

        if (sl->driver->broadcasts_to_all) {
            bool already_seen = false;
            for (int k = 0; k < seen_count; k++) {
                if (seen_broadcast[k] == sl->driver) { already_seen = true; break; }
            }
            sl->grace_until_us = grace_deadline();
            if (already_seen) continue;
            seen_broadcast[seen_count++] = sl->driver;
            if (intent == DESIRED_RECORDING_START) {
                ESP_LOGI(TAG, "slot %d: immediate dispatch — start_recording_all (broadcast)", i);
                sl->driver->start_recording_all();
            } else { /* DESIRED_RECORDING_STOP */
                ESP_LOGI(TAG, "slot %d: immediate dispatch — stop_recording_all (broadcast)", i);
                sl->driver->stop_recording_all();
            }
        } else {
            sl->grace_until_us = grace_deadline();
            if (intent == DESIRED_RECORDING_START) {
                ESP_LOGI(TAG, "slot %d: immediate dispatch — start_recording", i);
                sl->driver->start_recording(sl->driver_ctx);
            } else { /* DESIRED_RECORDING_STOP */
                ESP_LOGI(TAG, "slot %d: immediate dispatch — stop_recording", i);
                sl->driver->stop_recording(sl->driver_ctx);
            }
        }
    }
    unlock();
}

bool cam_core_get_auto_control(void)        { return s_auto_control; }
void cam_core_set_auto_control(bool enabled) { s_auto_control = enabled; }

/* ================================================================
 * Queries
 * ================================================================ */

camera_recording_status_t cam_core_get_recording_status(int idx)
{
    if (!valid_idx(idx)) return CAMERA_RECORDING_UNKNOWN;
    camera_recording_status_t status = CAMERA_RECORDING_UNKNOWN;
    lock();
    cam_core_slot_t *sl = s_slots[idx];
    if (sl && sl->driver) {
        status = sl->driver->get_recording_status(sl->driver_ctx);
    }
    unlock();
    return status;
}

desired_recording_t cam_core_get_desired(int idx)
{
    if (!valid_idx(idx)) return DESIRED_RECORDING_UNKNOWN;
    desired_recording_t d = DESIRED_RECORDING_UNKNOWN;
    lock();
    cam_core_slot_t *sl = s_slots[idx];
    if (sl) d = sl->desired;
    unlock();
    return d;
}

bool cam_core_is_recording(int idx)
{
    if (!valid_idx(idx)) return false;
    bool recording = false;
    lock();
    cam_core_slot_t *sl = s_slots[idx];
    if (sl && sl->ready && sl->driver) {
        recording = (sl->driver->get_recording_status(sl->driver_ctx)
                     == CAMERA_RECORDING_ACTIVE);
    }
    unlock();
    return recording;
}

camera_can_state_t cam_core_get_can_state(int idx)
{
    if (!valid_idx(idx)) return CAMERA_CAN_STATE_UNDEFINED;

    lock();
    cam_core_slot_t *sl = s_slots[idx];
    if (!sl) { unlock(); return CAMERA_CAN_STATE_UNDEFINED; }
    if (!sl->ready) { unlock(); return CAMERA_CAN_STATE_DISCONNECTED; }
    camera_recording_status_t rs = CAMERA_RECORDING_UNKNOWN;
    if (sl->driver) {
        rs = sl->driver->get_recording_status(sl->driver_ctx);
    }
    unlock();

    return (rs == CAMERA_RECORDING_ACTIVE) ? CAMERA_CAN_STATE_RECORDING
                                            : CAMERA_CAN_STATE_IDLE;
}

/* ================================================================
 * Shutdown helpers
 * ================================================================ */

esp_err_t cam_core_invoke_sleep(int idx)
{
    if (!valid_idx(idx)) return ESP_ERR_INVALID_ARG;
    const camera_driver_t *drv = NULL;
    void                  *ctx = NULL;

    lock();
    cam_core_slot_t *sl = s_slots[idx];
    if (sl && sl->driver) {
        drv = sl->driver;
        ctx = sl->driver_ctx;
    }
    unlock();

    if (!drv || !drv->sleep) return ESP_ERR_NOT_SUPPORTED;
    return drv->sleep(ctx);
}

void cam_core_teardown_slot(int idx)
{
    if (!valid_idx(idx)) return;

    esp_timer_handle_t     to_stop = NULL;
    const camera_driver_t *drv     = NULL;
    void                  *ctx     = NULL;

    lock();
    cam_core_slot_t *sl = s_slots[idx];
    if (!sl) { unlock(); return; }

    to_stop           = sl->poll_timer;
    sl->poll_timer    = NULL;
    sl->ready         = false;
    sl->grace_until_us = 0;
    drv               = sl->driver;
    ctx               = sl->driver_ctx;
    unlock();

    if (to_stop) {
        /* See file-header note on timer lifecycle — delete outside lock. */
        esp_timer_stop(to_stop);
        esp_timer_delete(to_stop);
    }
    if (drv && drv->teardown) {
        drv->teardown(ctx);
    }
}

void cam_core_notify_slot_index_changed(int idx)
{
    if (!valid_idx(idx)) return;
    lock();
    cam_core_slot_t *sl = s_slots[idx];
    if (sl && sl->driver && sl->driver->update_slot_index) {
        sl->driver->update_slot_index(sl->driver_ctx, idx);
    }
    unlock();
}

void cam_core_notify_wifi_disconnected(int idx)
{
    if (!valid_idx(idx)) return;
    lock();
    cam_core_slot_t *sl = s_slots[idx];
    if (sl && sl->driver && sl->driver->on_wifi_disconnected) {
        sl->driver->on_wifi_disconnected(sl->driver_ctx);
    }
    unlock();
}
