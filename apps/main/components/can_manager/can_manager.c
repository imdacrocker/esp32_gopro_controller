/*
 * can_manager.c — CAN bus manager (§14).
 *
 * Hardware: 1 Mbps, TX=GPIO7, RX=GPIO6, standard 11-bit IDs.
 * Receives:  0x600 (RaceCapture logging command), 0x602 (GPS UTC timestamp).
 * Transmits: 0x601 (camera status) at 5 Hz via esp_timer.
 *
 * RX model: on_rx_done ISR callback reads the frame and enqueues it to a
 * FreeRTOS queue; the RX task (priority 5, core 1) dequeues and processes.
 */

#include "can_manager.h"
#include "camera_manager.h"
#include "shutdown_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "can_manager";

/* ---- Hardware constants (§14.1) ------------------------------------------ */

#define CAN_TX_GPIO          7
#define CAN_RX_GPIO          6
#define CAN_TX_QUEUE_DEPTH   8

/* ---- Protocol constants (§14.2) ------------------------------------------ */

#define CAN_ID_LOGGING_CMD   0x600u
#define CAN_ID_CAM_STATUS    0x601u
#define CAN_ID_GPS_UTC       0x602u
#define CAN_ID_SHUTDOWN_REQ  0x603u

#define STATUS_TX_PERIOD_US  200000   /* 5 Hz */
#define WATCHDOG_TIMEOUT_US  5000000  /* 5 s  */

/* First valid 0x602 timestamp: 2020-01-01 00:00:00 UTC in ms */
#define UTC_MIN_VALID_MS     1577836800000ULL

/* ---- NVS ------------------------------------------------------------------ */

#define NVS_NAMESPACE        "can_mgr"
#define NVS_KEY_TZ_OFFSET    "tz_off"
#define NVS_KEY_LAST_UTC_MS  "last_utc"
#define NVS_KEY_BITRATE      "bitrate"

/* ---- Bitrate -------------------------------------------------------------- */

#define CAN_BITRATE_DEFAULT  1000000u

static const uint32_t CAN_BITRATES_ALLOWED[] = {
    50000u, 100000u, 125000u, 250000u, 500000u, 1000000u,
};

static bool bitrate_is_allowed(uint32_t bps)
{
    for (size_t i = 0; i < sizeof(CAN_BITRATES_ALLOWED) / sizeof(CAN_BITRATES_ALLOWED[0]); i++) {
        if (CAN_BITRATES_ALLOWED[i] == bps) return true;
    }
    return false;
}

/* Periodic UTC-to-NVS save once UTC is valid.  Coarse interval keeps flash
 * wear reasonable: at 5 min we never lose more than ~5 minutes of clock
 * advancement on power loss, while writing only ~288 records/day. */
#define UTC_SAVE_PERIOD_US   (5ULL * 60ULL * 1000000ULL)

/* ---- RX queue item -------------------------------------------------------- */

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[TWAI_FRAME_MAX_LEN];
} can_rx_item_t;

/* Depth matches the legacy RX queue depth from §14.1. */
#define RX_QUEUE_DEPTH       32

/* ---- State ---------------------------------------------------------------- */

static can_manager_callbacks_t s_cbs;
static twai_node_handle_t      s_node;
static QueueHandle_t           s_rx_queue;

/* Logging state: written only from RX task, read from any task.
 * Aligned 4-byte enum — single-word writes are atomic on Xtensa LX7. */
static volatile can_logging_state_t s_logging_state = LOGGING_STATE_UNKNOWN;

/* UTC timestamp state — multi-field, protected by mutex.
 *
 *   valid           — any anchor is available (NVS-restored, GPS, or manual)
 *   session_synced  — anchor came from a live source (GPS frame or manual web
 *                     set) THIS boot session.  An NVS-restored anchor does
 *                     not set this flag.  Used to gate camera SetDateTime.
 */
static SemaphoreHandle_t s_utc_mutex;
static struct {
    bool     valid;
    bool     session_synced;
    uint64_t last_utc_ms;
    int64_t  last_esp_us;   /* esp_timer_get_time() snapshot at anchor time */
} s_utc;

static int8_t s_tz_offset = 0; /* UTC by default; user-configurable via /api/settings/timezone, persisted in NVS. */

static uint32_t s_bitrate_bps = CAN_BITRATE_DEFAULT; /* loaded from NVS at init; applies on next boot. */

static esp_timer_handle_t s_tx_timer;
static esp_timer_handle_t s_watchdog_timer;
static esp_timer_handle_t s_utc_save_timer;
static TaskHandle_t       s_rx_task_handle;
static TaskHandle_t       s_recovery_task_handle;

/* Forwards bus-state transitions from the on_state_change ISR to the
 * recovery task in task context.  Items are twai_state_change_event_data_t. */
static QueueHandle_t      s_state_queue;

/* TX gate: cleared while the node is in BUS_OFF (or recovering), set
 * otherwise.  Single-word writes are atomic on Xtensa LX7, so this is safe
 * without a lock.  Without this gate, the 5 Hz tx_timer_cb would log
 * ESP_ERR_INVALID_STATE on every tick once bus-off is entered. */
static volatile bool      s_can_tx_enabled = true;

/* ---- Watchdog ------------------------------------------------------------- */

static void watchdog_cb(void *arg)
{
    /* 5 s elapsed without a 0x600 frame — suppress mismatch correction. */
    s_logging_state = LOGGING_STATE_UNKNOWN;
    /* When auto-control is off, leave each slot's desired_recording alone so
     * manual /api/shutter intent (and the mismatch-correction safety net that
     * goes with it) survive a CAN bus dropout. */
    if (camera_manager_get_auto_control()) {
        camera_manager_set_desired_recording_all(DESIRED_RECORDING_UNKNOWN);
    }
    /* Callback is NOT fired with UNKNOWN (§14.2). */
}

/* ---- 0x600 handler -------------------------------------------------------- */

static void handle_logging_cmd(const can_rx_item_t *item)
{
    /* During shutdown, drop logging-state frames silently — accepting them
     * would re-issue recording intent and undo the per-slot stop_recording
     * step.  See docs/design/shutdown.md §6.2. */
    if (shutdown_manager_is_active()) {
        return;
    }

    can_logging_state_t state = item->data[0]
        ? LOGGING_STATE_LOGGING
        : LOGGING_STATE_NOT_LOGGING;

    s_logging_state = state;

    /* Reset 5 s watchdog. */
    esp_timer_stop(s_watchdog_timer);
    esp_timer_start_once(s_watchdog_timer, WATCHDOG_TIMEOUT_US);

    /* When auto-control is off, the bus reports its state but must not drive
     * camera recording intent — otherwise a 0x600 frame arriving right after a
     * manual /api/shutter would flip desired_recording straight back. */
    if (camera_manager_get_auto_control()) {
        camera_manager_set_desired_recording_all(
            state == LOGGING_STATE_LOGGING
                ? DESIRED_RECORDING_START
                : DESIRED_RECORDING_STOP);
    }

    if (s_cbs.on_logging_state) {
        s_cbs.on_logging_state(state, s_cbs.on_logging_state_arg);
    }
}

/* ---- System clock + NVS persistence helpers ------------------------------ */

/* Push a UTC anchor into the libc system clock so gettimeofday()/time() in
 * other components return a useful value.  Caller must NOT hold s_utc_mutex. */
static void apply_utc_to_system_clock(uint64_t utc_ms)
{
    struct timeval tv = {
        .tv_sec  = (time_t)(utc_ms / 1000ULL),
        .tv_usec = (suseconds_t)((utc_ms % 1000ULL) * 1000ULL),
    };
    settimeofday(&tv, NULL);
}

/* Persist the current best estimate of UTC (extrapolated to "now") to NVS so
 * the next boot can come up with an approximately correct clock without a
 * GPS fix.  We can only get "close" — diff during power-off is unrecoverable. */
static void save_utc_to_nvs_now(void)
{
    uint64_t utc_ms;
    xSemaphoreTake(s_utc_mutex, portMAX_DELAY);
    if (!s_utc.valid) {
        xSemaphoreGive(s_utc_mutex);
        return;
    }
    int64_t elapsed_us = esp_timer_get_time() - s_utc.last_esp_us;
    utc_ms = s_utc.last_utc_ms + (uint64_t)(elapsed_us / 1000);
    xSemaphoreGive(s_utc_mutex);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(rw) failed; UTC not saved");
        return;
    }
    nvs_set_u64(h, NVS_KEY_LAST_UTC_MS, utc_ms);
    nvs_commit(h);
    nvs_close(h);
}

static void utc_save_timer_cb(void *arg)
{
    save_utc_to_nvs_now();
}

static void start_utc_save_timer_if_needed(void)
{
    if (!s_utc_save_timer) {
        return;   /* not yet created — init still running */
    }
    if (esp_timer_is_active(s_utc_save_timer)) {
        return;
    }
    esp_timer_start_periodic(s_utc_save_timer, UTC_SAVE_PERIOD_US);
}

/* Load the last saved UTC into the in-memory anchor.  Marks the anchor valid
 * but NOT session-synced — callers that distinguish the two (e.g. camera
 * SetDateTime gating) will still see "no live sync yet". */
static void load_utc_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint64_t saved_ms = 0;
    esp_err_t err = nvs_get_u64(h, NVS_KEY_LAST_UTC_MS, &saved_ms);
    nvs_close(h);

    if (err != ESP_OK || saved_ms < UTC_MIN_VALID_MS) {
        return;
    }

    xSemaphoreTake(s_utc_mutex, portMAX_DELAY);
    s_utc.valid          = true;
    s_utc.session_synced = false;
    s_utc.last_utc_ms    = saved_ms;
    s_utc.last_esp_us    = esp_timer_get_time();
    xSemaphoreGive(s_utc_mutex);

    apply_utc_to_system_clock(saved_ms);
    ESP_LOGI(TAG, "UTC restored from NVS: %llu ms (not session-synced)",
             (unsigned long long)saved_ms);
}

/* ---- 0x603 handler -------------------------------------------------------- *
 *
 * Byte 0 non-zero requests system shutdown.  shutdown_manager_request() is
 * idempotent so the callback fires unconditionally — repeated frames during
 * SHUTTING_DOWN / COMPLETE collapse into no-ops at the shutdown_manager
 * layer.  See docs/design/shutdown.md §6.1.
 */
static void handle_shutdown_request(const can_rx_item_t *item)
{
    if (item->dlc < 1 || item->data[0] == 0) {
        return;
    }
    if (s_cbs.on_shutdown_request) {
        s_cbs.on_shutdown_request(s_cbs.on_shutdown_request_arg);
    }
}

/* ---- 0x602 handler -------------------------------------------------------- */

static void handle_gps_utc(const can_rx_item_t *item)
{
    if (item->dlc < 8) {
        return;
    }

    uint64_t utc_ms;
    memcpy(&utc_ms, item->data, sizeof(utc_ms));   /* little-endian on-wire */

    if (utc_ms < UTC_MIN_VALID_MS) {
        return;   /* GPS not yet locked or stale */
    }

    xSemaphoreTake(s_utc_mutex, portMAX_DELAY);
    bool first           = !s_utc.session_synced;
    s_utc.valid          = true;
    s_utc.session_synced = true;
    s_utc.last_utc_ms    = utc_ms;
    s_utc.last_esp_us    = esp_timer_get_time();
    xSemaphoreGive(s_utc_mutex);

    apply_utc_to_system_clock(utc_ms);

    if (first) {
        save_utc_to_nvs_now();
        start_utc_save_timer_if_needed();
        if (s_cbs.on_utc_acquired) {
            s_cbs.on_utc_acquired(utc_ms, s_cbs.on_utc_acquired_arg);
        }
    }
}

/* ---- ISR: on_rx_done — reads frame and enqueues to RX task --------------- */

static bool IRAM_ATTR on_rx_done_isr(twai_node_handle_t handle,
                                      const twai_rx_done_event_data_t *edata,
                                      void *user_ctx)
{
    can_rx_item_t item;
    twai_frame_t frame = {
        .buffer     = item.data,
        .buffer_len = sizeof(item.data),
    };

    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK) {
        return false;
    }

    item.id  = frame.header.id;
    item.dlc = (uint8_t)frame.header.dlc;

    BaseType_t higher_prio_woken = pdFALSE;
    xQueueSendFromISR(s_rx_queue, &item, &higher_prio_woken);
    return higher_prio_woken == pdTRUE;
}

/* ---- ISR: on_state_change — bus error-state transitions ------------------ */

/* Fires on every error-FSM transition (ACTIVE/WARNING/PASSIVE/BUS_OFF).
 * We act on two transitions:
 *   - entering BUS_OFF: gate TX off and signal the recovery task.
 *   - leaving BUS_OFF:  re-open the TX gate (recovery completed in hardware).
 * Other transitions are forwarded to the recovery task for logging. */
static bool IRAM_ATTR on_state_change_isr(twai_node_handle_t handle,
                                           const twai_state_change_event_data_t *edata,
                                           void *user_ctx)
{
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        s_can_tx_enabled = false;
    } else if (edata->old_sta == TWAI_ERROR_BUS_OFF) {
        s_can_tx_enabled = true;
    }

    BaseType_t higher_prio_woken = pdFALSE;
    xQueueSendFromISR(s_state_queue, edata, &higher_prio_woken);
    return higher_prio_woken == pdTRUE;
}

/* ---- Recovery task — initiates twai_node_recover() in task context ------- */

/* twai_node_recover() is documented as a kick-off (non-blocking); the
 * controller waits for 128*11 consecutive recessive bits before firing
 * on_state_change again with new_sta != BUS_OFF.  We must call it from a
 * task because it isn't documented as ISR-safe. */
static void recovery_task(void *arg)
{
    twai_state_change_event_data_t ev;
    for (;;) {
        if (!xQueueReceive(s_state_queue, &ev, portMAX_DELAY)) {
            continue;
        }

        if (ev.new_sta == TWAI_ERROR_BUS_OFF) {
            ESP_LOGW(TAG, "TWAI bus-off detected — initiating recovery");
            esp_err_t err = twai_node_recover(s_node);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "twai_node_recover: %s", esp_err_to_name(err));
            }
        } else if (ev.old_sta == TWAI_ERROR_BUS_OFF) {
            ESP_LOGI(TAG, "TWAI recovered from bus-off");
        }
    }
}

/* ---- RX task (§14.4 — priority 5, core 1) -------------------------------- */

static void rx_task(void *arg)
{
    can_rx_item_t item;

    for (;;) {
        if (!xQueueReceive(s_rx_queue, &item, portMAX_DELAY)) {
            continue;
        }

        if (s_cbs.on_rx_frame) {
            s_cbs.on_rx_frame(item.id, item.data, item.dlc,
                               s_cbs.on_rx_frame_arg);
        }

        switch (item.id) {
        case CAN_ID_LOGGING_CMD:
            handle_logging_cmd(&item);
            break;
        case CAN_ID_GPS_UTC:
            handle_gps_utc(&item);
            break;
        case CAN_ID_SHUTDOWN_REQ:
            handle_shutdown_request(&item);
            break;
        default:
            break;
        }
    }
}

/* ---- 0x601 TX timer (5 Hz) ------------------------------------------------ */

static void tx_timer_cb(void *arg)
{
    /* Skip transmit while the node is in bus-off / recovery — every TX
     * attempt would otherwise return ESP_ERR_INVALID_STATE and spam the log
     * at 5 Hz.  Gate is re-opened by on_state_change_isr() once the
     * controller leaves BUS_OFF. */
    if (!s_can_tx_enabled) {
        return;
    }

    /* Once shutdown is complete, stop announcing camera status on the bus.
     * RaceCapture-side observers treat CAN silence on 0x601 as the shutdown
     * indicator.  We keep transmitting during SHUTTING_DOWN so the bus sees
     * the per-slot state transition to DISCONNECTED in real time.
     * See docs/design/shutdown.md §6.3. */
    if (shutdown_manager_get_state() == SHUTDOWN_STATE_COMPLETE) {
        return;
    }

    uint8_t data[CAMERA_MAX_SLOTS];
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        data[i] = (uint8_t)camera_manager_get_slot_can_state(i);
    }

    twai_frame_t frame = {
        .header = {
            .id  = CAN_ID_CAM_STATUS,
            .dlc = CAMERA_MAX_SLOTS,
            .ide = 0,
            .rtr = 0,
            .fdf = 0,
        },
        .buffer     = data,
        .buffer_len = sizeof(data),
    };

    /* Non-blocking — drop the frame if the TX queue is full. */
    esp_err_t err = twai_node_transmit(s_node, &frame, 0);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "twai_node_transmit: %s", esp_err_to_name(err));
    }
}

/* ---- NVS ------------------------------------------------------------------ */

static void load_tz_offset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int8_t val;
    if (nvs_get_i8(h, NVS_KEY_TZ_OFFSET, &val) == ESP_OK) {
        s_tz_offset = val;
    }
    nvs_close(h);
}

static void load_bitrate(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint32_t val;
    if (nvs_get_u32(h, NVS_KEY_BITRATE, &val) == ESP_OK && bitrate_is_allowed(val)) {
        s_bitrate_bps = val;
    }
    nvs_close(h);
}

static void save_tz_offset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    nvs_set_i8(h, NVS_KEY_TZ_OFFSET, s_tz_offset);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- Public API ----------------------------------------------------------- */

void can_manager_register_callbacks(const can_manager_callbacks_t *cbs)
{
    s_cbs = *cbs;
}

void can_manager_init(void)
{
    load_tz_offset();
    load_bitrate();

    s_utc_mutex = xSemaphoreCreateMutex();
    configASSERT(s_utc_mutex);

    /* Restore the last known UTC from NVS (sets system clock too).  Anchor
     * is marked valid but NOT session-synced — live sync flag flips only on
     * a real 0x602 frame or a manual web-UI set this session. */
    load_utc_from_nvs();

    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(can_rx_item_t));
    configASSERT(s_rx_queue);

    /* TWAI node (§14.1). */
    twai_onchip_node_config_t node_cfg = {
        .io_cfg = {
            .tx               = CAN_TX_GPIO,
            .rx               = CAN_RX_GPIO,
            .quanta_clk_out   = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing = {
            .bitrate = s_bitrate_bps,
        },
        .tx_queue_depth = CAN_TX_QUEUE_DEPTH,
        .fail_retry_cnt = 0,      /* single attempt — drop on ACK error (no receiver connected) */
    };
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_cfg, &s_node));

    /* Bus-state transition queue — depth 4 is plenty: the controller can only
     * traverse 4 error states, and the recovery task drains immediately. */
    s_state_queue = xQueueCreate(4, sizeof(twai_state_change_event_data_t));
    configASSERT(s_state_queue);

    twai_event_callbacks_t twai_cbs = {
        .on_rx_done      = on_rx_done_isr,
        .on_state_change = on_state_change_isr,
    };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(s_node, &twai_cbs, NULL));

    ESP_ERROR_CHECK(twai_node_enable(s_node));
    ESP_LOGI(TAG, "TWAI started at %u bps (TX=%d RX=%d)",
             (unsigned)s_bitrate_bps, CAN_TX_GPIO, CAN_RX_GPIO);

    /* 5 s watchdog — fires if no 0x600 frame arrives. */
    esp_timer_create_args_t wd_args = {
        .callback = watchdog_cb,
        .name     = "can_watchdog",
    };
    ESP_ERROR_CHECK(esp_timer_create(&wd_args, &s_watchdog_timer));

    /* 0x601 TX timer at 5 Hz. */
    esp_timer_create_args_t tx_args = {
        .callback = tx_timer_cb,
        .name     = "can_tx",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tx_args, &s_tx_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tx_timer, STATUS_TX_PERIOD_US));

    /* Periodic UTC-to-NVS save.  Created here but only armed once UTC becomes
     * valid (either by NVS restore above or by the first GPS / manual set). */
    esp_timer_create_args_t save_args = {
        .callback = utc_save_timer_cb,
        .name     = "can_utc_save",
    };
    ESP_ERROR_CHECK(esp_timer_create(&save_args, &s_utc_save_timer));
    if (s_utc.valid) {
        start_utc_save_timer_if_needed();
    }

    /* RX task on core 1, priority 5 (§14.4). */
    xTaskCreatePinnedToCore(rx_task, "can_rx", 4096, NULL, 5,
                             &s_rx_task_handle, 1);
    configASSERT(s_rx_task_handle);

    /* Recovery task on core 1, priority 4 — sits below RX so it can never
     * delay frame dispatch.  Wakes only on bus error-state transitions. */
    xTaskCreatePinnedToCore(recovery_task, "can_recov", 3072, NULL, 4,
                             &s_recovery_task_handle, 1);
    configASSERT(s_recovery_task_handle);
}

can_logging_state_t can_manager_get_logging_state(void)
{
    return s_logging_state;
}

bool can_manager_get_utc_ms(uint64_t *out_ms)
{
    /* Callable from other components (BLE readiness, etc.) that may run
     * before can_manager_init() has finished creating the mutex. */
    if (!s_utc_mutex) return false;

    xSemaphoreTake(s_utc_mutex, portMAX_DELAY);
    if (!s_utc.valid) {
        xSemaphoreGive(s_utc_mutex);
        return false;
    }
    int64_t elapsed_us = esp_timer_get_time() - s_utc.last_esp_us;
    *out_ms = s_utc.last_utc_ms + (uint64_t)(elapsed_us / 1000);
    xSemaphoreGive(s_utc_mutex);
    return true;
}

void can_manager_set_tz_offset(int8_t hours)
{
    if (hours < -12) hours = -12;
    if (hours >  14) hours =  14;
    s_tz_offset = hours;
    save_tz_offset();
}

int8_t can_manager_get_tz_offset(void)
{
    return s_tz_offset;
}

esp_err_t can_manager_set_manual_utc_ms(uint64_t utc_ms)
{
    if (utc_ms < UTC_MIN_VALID_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_utc_mutex, portMAX_DELAY);
    /* Reject only if a live source already won this session — an NVS-restored
     * value (valid && !session_synced) does not block the user from entering
     * a fresh time. */
    if (s_utc.session_synced) {
        xSemaphoreGive(s_utc_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_utc.valid          = true;
    s_utc.session_synced = true;
    s_utc.last_utc_ms    = utc_ms;
    s_utc.last_esp_us    = esp_timer_get_time();
    xSemaphoreGive(s_utc_mutex);

    apply_utc_to_system_clock(utc_ms);
    save_utc_to_nvs_now();
    start_utc_save_timer_if_needed();

    /* Fire the one-time callback — same path as a real GPS acquisition. */
    if (s_cbs.on_utc_acquired) {
        s_cbs.on_utc_acquired(utc_ms, s_cbs.on_utc_acquired_arg);
    }

    ESP_LOGI(TAG, "manual UTC set: %llu ms", (unsigned long long)utc_ms);
    return ESP_OK;
}

bool can_manager_utc_is_session_synced(void)
{
    if (!s_utc_mutex) return false;

    xSemaphoreTake(s_utc_mutex, portMAX_DELAY);
    bool synced = s_utc.session_synced;
    xSemaphoreGive(s_utc_mutex);
    return synced;
}

uint32_t can_manager_get_bitrate(void)
{
    return s_bitrate_bps;
}

esp_err_t can_manager_set_bitrate(uint32_t bitrate_bps)
{
    if (!bitrate_is_allowed(bitrate_bps)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_bitrate_bps = bitrate_bps;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return ESP_FAIL;
    }
    nvs_set_u32(h, NVS_KEY_BITRATE, bitrate_bps);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "CAN bitrate set to %u bps (applies on reboot)",
             (unsigned)bitrate_bps);
    return ESP_OK;
}
