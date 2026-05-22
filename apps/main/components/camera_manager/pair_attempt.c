/*
 * pair_attempt.c — single in-flight pair-attempt state machine.
 *
 * Tracks the user-initiated BLE add-camera flow so the web UI can show
 * "Pairing…" and surface error reasons that prevent a camera from being
 * remembered.  Reconnects do NOT use this machine — it exists purely for
 * the initial-pair flow.
 *
 * Sticky terminal states: SUCCESS and FAILED remain set until the next
 * pair_attempt_begin() clears them.  This avoids polling races where a
 * client might miss the result if it auto-cleared on a timer.
 *
 * Forward-only transitions: advance() ignores any state at or behind the
 * current state.  fail() ignores calls when state is already terminal —
 * so the first cause set wins (e.g. HWINFO_TIMEOUT before the inevitable
 * BLE-disconnect that follows).
 */

#include "camera_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include <string.h>

static const char *TAG = "pair_attempt";

/* Overall watchdog: if a pair attempt does not reach SUCCESS or FAILED
 * within this window, force it to FAILED+HANDSHAKE_TIMEOUT.  Covers the
 * classic "camera advertises but never bonds" hang (e.g. Hero7's broken
 * SMP) where NimBLE's own SMP timeout takes ~30 s. */
#define PAIR_ATTEMPT_WATCHDOG_MS  20000

static SemaphoreHandle_t   s_mutex;
static pair_attempt_info_t s_info;
static uint16_t            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static esp_timer_handle_t  s_watchdog;

static inline void lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_mutex); }

static void ensure_init(void)
{
    if (s_mutex) return;
    /* xSemaphoreCreateMutex is safe at first-use; pair_attempt_begin can only
     * be called from the http_server task after FreeRTOS is up. */
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_info, 0, sizeof(s_info));
    s_info.state  = PAIR_ATTEMPT_IDLE;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

static bool is_terminal(pair_attempt_state_t s)
{
    return s == PAIR_ATTEMPT_SUCCESS || s == PAIR_ATTEMPT_FAILED;
}

static void watchdog_cb(void *arg)
{
    (void)arg;
    /* Runs on the esp_timer task — must not hold the pair_attempt mutex
     * across NimBLE calls.  Reuse pair_attempt_cancel() for cleanup, then
     * overwrite the error code so the UI sees TIMEOUT instead of CANCELLED. */
    if (!pair_attempt_in_flight()) return;

    ESP_LOGW(TAG, "watchdog fired — aborting stalled pair attempt");
    pair_attempt_cancel();

    lock();
    s_info.error_code = PAIR_ERROR_HANDSHAKE_TIMEOUT;
    strncpy(s_info.error_message,
            "Camera did not finish pairing in time",
            sizeof(s_info.error_message) - 1);
    s_info.error_message[sizeof(s_info.error_message) - 1] = '\0';
    unlock();
}

static void watchdog_arm(void)
{
    if (!s_watchdog) {
        esp_timer_create_args_t args = {
            .callback        = watchdog_cb,
            .arg             = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name            = "pair_wdt",
        };
        if (esp_timer_create(&args, &s_watchdog) != ESP_OK) {
            ESP_LOGW(TAG, "watchdog timer create failed");
            return;
        }
    }
    esp_timer_stop(s_watchdog);  /* harmless if not started */
    esp_timer_start_once(s_watchdog,
                         (uint64_t)PAIR_ATTEMPT_WATCHDOG_MS * 1000ULL);
}

static void watchdog_disarm(void)
{
    if (s_watchdog) esp_timer_stop(s_watchdog);
}

void pair_attempt_reset_watchdog(uint32_t timeout_ms)
{
    if (!s_mutex || !s_watchdog) return;

    lock();
    bool in_flight = (s_info.state != PAIR_ATTEMPT_IDLE) && !is_terminal(s_info.state);
    unlock();
    if (!in_flight) return;

    esp_timer_stop(s_watchdog);  /* harmless if not running */
    esp_timer_start_once(s_watchdog, (uint64_t)timeout_ms * 1000ULL);
    ESP_LOGI(TAG, "watchdog reset: %lu ms", (unsigned long)timeout_ms);
}

esp_err_t pair_attempt_begin(const uint8_t addr[6], uint8_t addr_type,
                              pair_attempt_transport_t transport)
{
    if (!addr) return ESP_ERR_INVALID_ARG;
    ensure_init();

    lock();
    if (s_info.state != PAIR_ATTEMPT_IDLE && !is_terminal(s_info.state)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_info, 0, sizeof(s_info));
    s_info.state      = PAIR_ATTEMPT_CONNECTING;
    s_info.transport  = transport;
    s_info.addr_type  = addr_type;
    s_info.model      = CAMERA_MODEL_UNKNOWN;
    s_info.error_code = PAIR_ERROR_NONE;
    memcpy(s_info.addr, addr, 6);
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    unlock();

    watchdog_arm();

    ESP_LOGI(TAG, "begin (%s): %02x:%02x:%02x:%02x:%02x:%02x",
             transport == PAIR_TRANSPORT_WIFI_RC ? "wifi-rc" : "ble",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return ESP_OK;
}

bool pair_attempt_addr_matches(const uint8_t addr[6])
{
    if (!s_mutex || !addr) return false;
    bool match = false;
    lock();
    if (s_info.state != PAIR_ATTEMPT_IDLE && !is_terminal(s_info.state)) {
        match = (memcmp(s_info.addr, addr, 6) == 0);
    }
    unlock();
    return match;
}

void pair_attempt_advance(pair_attempt_state_t new_state)
{
    if (!s_mutex) return;
    lock();
    if (is_terminal(s_info.state)) {
        unlock();
        return;
    }
    if ((int)new_state <= (int)s_info.state) {
        unlock();
        return;
    }
    s_info.state = new_state;
    unlock();

    if (new_state == PAIR_ATTEMPT_SUCCESS) {
        watchdog_disarm();
    }

    ESP_LOGI(TAG, "advance → %d", (int)new_state);
}

void pair_attempt_fail(pair_attempt_error_t err, const char *message)
{
    if (!s_mutex) return;
    lock();
    if (is_terminal(s_info.state) || s_info.state == PAIR_ATTEMPT_IDLE) {
        unlock();
        return;
    }
    s_info.state      = PAIR_ATTEMPT_FAILED;
    s_info.error_code = err;
    if (message) {
        strncpy(s_info.error_message, message, sizeof(s_info.error_message) - 1);
        s_info.error_message[sizeof(s_info.error_message) - 1] = '\0';
    } else {
        s_info.error_message[0] = '\0';
    }
    unlock();

    watchdog_disarm();

    ESP_LOGW(TAG, "fail: code=%d msg='%s'", (int)err,
             message ? message : "");
}

void pair_attempt_set_model(camera_model_t model)
{
    if (!s_mutex) return;
    lock();
    if (s_info.state != PAIR_ATTEMPT_IDLE && !is_terminal(s_info.state)) {
        s_info.model = model;
    }
    unlock();
}

void pair_attempt_get(pair_attempt_info_t *out)
{
    if (!out) return;
    ensure_init();
    lock();
    *out = s_info;
    unlock();
}

bool pair_attempt_in_flight(void)
{
    if (!s_mutex) return false;
    lock();
    bool in_flight = (s_info.state != PAIR_ATTEMPT_IDLE) && !is_terminal(s_info.state);
    unlock();
    return in_flight;
}

void pair_attempt_set_conn_handle(uint16_t conn_handle)
{
    if (!s_mutex) return;
    lock();
    if (s_info.state != PAIR_ATTEMPT_IDLE && !is_terminal(s_info.state)) {
        s_conn_handle = conn_handle;
    }
    unlock();
}

esp_err_t pair_attempt_cancel(void)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    /* Snapshot the state we need to act on, then mark FAILED so any racing
     * advance(SUCCESS) from the BLE host task becomes a no-op.  Don't hold
     * the mutex while calling NimBLE — keep the critical section short. */
    pair_attempt_state_t      prev_state;
    pair_attempt_transport_t  transport;
    uint16_t                  conn_handle;
    uint8_t                   addr[6];

    lock();
    if (s_info.state == PAIR_ATTEMPT_IDLE || is_terminal(s_info.state)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    prev_state  = s_info.state;
    transport   = s_info.transport;
    conn_handle = s_conn_handle;
    memcpy(addr, s_info.addr, 6);

    s_info.state      = PAIR_ATTEMPT_FAILED;
    s_info.error_code = PAIR_ERROR_CANCELLED;
    strncpy(s_info.error_message, "Pairing cancelled by user",
            sizeof(s_info.error_message) - 1);
    s_info.error_message[sizeof(s_info.error_message) - 1] = '\0';
    unlock();

    watchdog_disarm();

    ESP_LOGI(TAG, "cancel (%s): was state=%d conn=%u",
             transport == PAIR_TRANSPORT_WIFI_RC ? "wifi-rc" : "ble",
             (int)prev_state, (unsigned)conn_handle);

    if (transport == PAIR_TRANSPORT_BLE) {
        /* Always try to abort an in-flight connect — no-op (BLE_HS_ENOTCONN)
         * if we're past the connect phase. */
        int rc = ble_gap_conn_cancel();
        if (rc != 0 && rc != BLE_HS_ENOTCONN) {
            ESP_LOGW(TAG, "ble_gap_conn_cancel rc=%d", rc);
        }

        /* If L2 came up, drop the link.  The disconnect callback will run on
         * the NimBLE host task and clean up driver state. */
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            if (rc != 0) {
                ESP_LOGW(TAG, "ble_gap_terminate(%u) rc=%d",
                         (unsigned)conn_handle, rc);
            }
        }

        /* If a slot was registered during PROVISIONING and never reached
         * first_pair_complete, remove it.  Mirrors the frozen-model cleanup
         * path in readiness.c — without this, a slot with model=UNKNOWN sits
         * in RAM (not NVS-saved) until reboot. */
        if (prev_state == PAIR_ATTEMPT_PROVISIONING) {
            int slot = camera_manager_find_by_mac(addr);
            if (slot >= 0) {
                camera_slot_info_t info;
                if (camera_manager_get_slot_info(slot, &info) == ESP_OK &&
                    !info.first_pair_complete) {
                    ESP_LOGI(TAG, "cancel: removing unfinished slot %d", slot);
                    camera_manager_remove_slot(slot);
                }
            }
        }
    } else if (transport == PAIR_TRANSPORT_WIFI_RC) {
        /* RC slots are registered (and first_pair_complete=true) immediately
         * by gopro_wifi_rc_add_camera() — there's no multi-step handshake to
         * roll back to.  On cancel/timeout, remove the slot unconditionally
         * so a never-responding device doesn't leave a phantom camera in NVS.
         * The driver's teardown vtable disarms keepalive/WoL timers. */
        int slot = camera_manager_find_by_mac(addr);
        if (slot >= 0) {
            ESP_LOGI(TAG, "cancel: removing RC slot %d", slot);
            camera_manager_remove_slot(slot);
        }
    }

    return ESP_OK;
}
