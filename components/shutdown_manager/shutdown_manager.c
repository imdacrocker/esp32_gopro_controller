/*
 * shutdown_manager.c — see docs/design/shutdown.md.
 *
 * One per-slot FreeRTOS task per configured camera; each runs:
 *   stop_recording (if recording, with 1.5 s confirm wait)
 *   sleep command  (via driver vtable, with 500 ms ACK settle)
 *   link terminate (for slots whose driver holds a persistent link, via the
 *                   cam_core terminate_link vtable entry)
 *   teardown       (cam_core_teardown_slot — stops mismatch timer +
 *                   invokes driver teardown)
 *
 * Per-slot deadline: 5 s end-to-end.  On expiry the slot is marked failed
 * but the sequence continues; the global state transitions to COMPLETE
 * once every slot has finished one way or the other.
 *
 * Layering note: the link-terminate step is routed through cam_core's
 * `terminate_link` vtable entry (the BLE driver implements it; RC/USB leave
 * it NULL), so this component depends only on cam_core — no wireless-only
 * components.  Done as part of the wired-variant integration
 * (docs/design/wired-variant.md §2).
 */

#include "shutdown_manager.h"

#include <stdatomic.h>
#include <string.h>

#include "cam_core.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "shutdown";

#define PER_SLOT_DEADLINE_MS        5000
#define STOP_RECORDING_TIMEOUT_MS   1500
#define SLEEP_SETTLE_MS              500
#define IS_RECORDING_POLL_MS         100

#define SHUTDOWN_TASK_STACK_BYTES   4096
#define SHUTDOWN_TASK_PRIORITY      4

/* ---- Module state -------------------------------------------------------- */

static atomic_int        s_state          = ATOMIC_VAR_INIT(SHUTDOWN_STATE_IDLE);
static SemaphoreHandle_t s_mutex;
static uint8_t           s_failed_mask;
static int               s_done_count;
static int               s_slot_count_at_start;

/* ---- Per-slot worker ----------------------------------------------------- */

static void shutdown_slot_task(void *arg)
{
    int slot = (int)(intptr_t)arg;
    int64_t deadline_us = esp_timer_get_time() + (int64_t)PER_SLOT_DEADLINE_MS * 1000;
    bool failed = false;

    if (!cam_core_slot_active(slot)) {
        ESP_LOGW(TAG, "slot %d: not configured at task start — skipping", slot);
        goto done;
    }

    ESP_LOGI(TAG, "slot %d: shutdown sequence starting", slot);

    /* ---- Step 1: stop recording if active. ------------------------------- */
    if (cam_core_is_recording(slot)) {
        ESP_LOGI(TAG, "slot %d: stopping recording", slot);
        cam_core_set_desired_slot(slot, DESIRED_RECORDING_STOP);

        int64_t step_deadline_us = esp_timer_get_time() +
                                   (int64_t)STOP_RECORDING_TIMEOUT_MS * 1000;
        while (esp_timer_get_time() < step_deadline_us &&
               esp_timer_get_time() < deadline_us) {
            if (!cam_core_is_recording(slot)) break;
            vTaskDelay(pdMS_TO_TICKS(IS_RECORDING_POLL_MS));
        }
        if (cam_core_is_recording(slot)) {
            ESP_LOGW(TAG, "slot %d: stop_recording did not confirm — marking failed",
                     slot);
            failed = true;
            /* Continue to sleep anyway — the sleep command itself stops
             * recording on most GoPros. */
        } else {
            ESP_LOGI(TAG, "slot %d: recording stopped", slot);
        }
    }

    /* ---- Step 2: send sleep command (best-effort). ----------------------- */
    if (esp_timer_get_time() < deadline_us) {
        esp_err_t err = cam_core_invoke_sleep(slot);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "slot %d: sleep command sent", slot);
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(TAG, "slot %d: no sleep command for model — relying on auto-sleep",
                     slot);
        } else {
            ESP_LOGW(TAG, "slot %d: sleep send failed (%s) — continuing",
                     slot, esp_err_to_name(err));
            failed = true;
        }

        /* Brief settle so the BLE/UDP write isn't torn down mid-flight.
         * Capped at the remaining per-slot budget. */
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        int settle_ms = (remaining_us > (int64_t)SLEEP_SETTLE_MS * 1000)
                          ? SLEEP_SETTLE_MS
                          : (int)(remaining_us / 1000);
        if (settle_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(settle_ms));
        }
    }

    /* ---- Step 3: terminate the transport link (if any). ----------------- *
     * Routed through cam_core's terminate_link vtable entry so this component
     * depends only on cam_core, not on any wireless-only driver.  Drivers
     * with no persistent link (RC-emulation, USB) leave the entry NULL and
     * this is a no-op.  cam_core_slot_requires_ble gates the call to slots
     * whose driver actually holds a link worth tearing down. */
    if (cam_core_slot_requires_ble(slot)) {
        cam_core_invoke_terminate_link(slot);
    }

    /* ---- Step 4: stop driver timers, mark slot disconnected. ------------- */
    cam_core_teardown_slot(slot);

done:
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (failed) s_failed_mask |= (uint8_t)(1u << slot);
    s_done_count++;
    bool all_done = (s_done_count >= s_slot_count_at_start);
    if (all_done) {
        atomic_store(&s_state, SHUTDOWN_STATE_COMPLETE);
    }
    xSemaphoreGive(s_mutex);

    if (all_done) {
        ESP_LOGI(TAG, "shutdown complete (failed_mask=0x%02x)", s_failed_mask);
    }

    vTaskDelete(NULL);
}

/* ---- Public API ---------------------------------------------------------- */

void shutdown_manager_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);
    s_failed_mask         = 0;
    s_done_count          = 0;
    s_slot_count_at_start = 0;
    atomic_store(&s_state, SHUTDOWN_STATE_IDLE);
    ESP_LOGI(TAG, "init OK");
}

esp_err_t shutdown_manager_request(void)
{
    /* CAS from IDLE; any other current state is an idempotent no-op. */
    int expected = SHUTDOWN_STATE_IDLE;
    if (!atomic_compare_exchange_strong(&s_state, &expected,
                                         SHUTDOWN_STATE_SHUTTING_DOWN)) {
        ESP_LOGI(TAG, "request ignored — already in state %d", expected);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "shutdown requested");

    int slot_count = cam_core_slot_count();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_failed_mask         = 0;
    s_done_count          = 0;
    s_slot_count_at_start = slot_count;
    xSemaphoreGive(s_mutex);

    if (slot_count == 0) {
        /* No cameras to shut down — go straight to COMPLETE. */
        ESP_LOGI(TAG, "no paired cameras — shutdown complete immediately");
        atomic_store(&s_state, SHUTDOWN_STATE_COMPLETE);
        return ESP_OK;
    }

    /* Spawn one task per configured slot. */
    for (int i = 0; i < slot_count; i++) {
        /* Buffer sized so gcc -Werror=format-truncation can statically
         * prove no truncation: "shutdown_" (9) + INT_MIN-width %d (11)
         * + NUL = 21 bytes worst case.  FreeRTOS truncates internally to
         * configMAX_TASK_NAME_LEN regardless. */
        char name[24];
        snprintf(name, sizeof(name), "shutdown_%d", i);
        BaseType_t ok = xTaskCreate(shutdown_slot_task,
                                     name,
                                     SHUTDOWN_TASK_STACK_BYTES / sizeof(StackType_t),
                                     (void *)(intptr_t)i,
                                     SHUTDOWN_TASK_PRIORITY,
                                     NULL);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "slot %d: failed to spawn task — counting as failed",
                     i);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_failed_mask |= (uint8_t)(1u << i);
            s_done_count++;
            bool all_done = (s_done_count >= s_slot_count_at_start);
            if (all_done) atomic_store(&s_state, SHUTDOWN_STATE_COMPLETE);
            xSemaphoreGive(s_mutex);
        }
    }
    return ESP_OK;
}

void shutdown_manager_on_can_request(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "CAN 0x603 shutdown request received");
    shutdown_manager_request();
}

shutdown_state_t shutdown_manager_get_state(void)
{
    return (shutdown_state_t)atomic_load(&s_state);
}

bool shutdown_manager_is_active(void)
{
    return atomic_load(&s_state) != SHUTDOWN_STATE_IDLE;
}

uint8_t shutdown_manager_get_failed_slots_mask(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t m = s_failed_mask;
    xSemaphoreGive(s_mutex);
    return m;
}
