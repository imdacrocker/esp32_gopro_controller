#include "camera_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_ip_addr.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <assert.h>

static const char *TAG = "camera_manager";

/* ---- NVS layout (§6.1) ----
 *
 * Schema version 2 added the `first_pair_complete` field at the end of the
 * record.  Bumping the version invalidates v1 records on first boot after
 * upgrade — flash must be erased.
 *
 * Schema version 3 renumbered camera_model_t entries to match GoPro's
 * official model_number IDs (HERO4_BLACK 40→13, HERO4_SILVER 41→12,
 * HERO_LEGACY_RC 39→999) and added Hero2/3/3+/4 Session/5/6/HERO 2018 IDs.
 * v2 records that store the old HERO4 values would silently decode as
 * different cameras after the renumber, so the version is bumped to discard
 * them; users must re-pair.
 */
#define CAMERA_NV_SCHEMA_VERSION  3
#define NVS_KEY_CAMERA            "camera"

/* Mismatch poll interval (§13.3 default; per-model tuning deferred) */
#define STATUS_POLL_INTERVAL_MS   2000

/* Grace window after a shutter command during which the mismatch poll will
 * NOT compare desired vs. cached recording status.  Both Hero4 (UDP `st`
 * polling) and Hero 9+ (BLE status notifications) take several seconds to
 * report a state change after a SetShutter; 10 s comfortably covers both
 * and the worst-case status-poll scheduling, so we never re-issue a Start
 * to a camera that's already recording but hasn't yet told us. */
#define RECORDING_STATUS_GRACE_MS 10000

#define MAX_DRIVER_REGS  4

/*
 * NVS record — extended in a backwards-compatible way: new fields are
 * appended to the END of the struct.  load_slot_from_nvs() memsets the
 * record to zero before reading, so legacy records (smaller blobs) leave
 * the new fields zero.  The schema version is only bumped if existing
 * fields are renamed/removed/repurposed; appending is implicitly safe.
 */
typedef struct {
    uint8_t        version;
    char           name[32];
    camera_model_t model;
    uint8_t        mac[6];
    uint32_t       last_ip;
    bool           is_configured;
    bool           first_pair_complete;
} camera_nv_record_t;

/* Internal per-slot state */
typedef struct {
    /* Persisted (mirrored from NVS on load) */
    char              name[32];
    camera_model_t    model;
    uint8_t           mac[6];          /* BLE peer MAC for BLE-control cameras;
                                          WiFi MAC for RC-emulation cameras */
    uint32_t          last_ip;
    bool              is_configured;
    bool              first_pair_complete;

    /* BLE (§7.2) */
    cam_ble_status_t  ble_status;
    uint16_t          ble_handle;

    /* WiFi (§7.3) */
    wifi_cam_status_t wifi_status;
    uint32_t          ip_addr;
    bool              wifi_associated;  /* true between SoftAP station join
                                           and disassociation events; RC
                                           cameras only */

    /* Control */
    desired_recording_t    desired_recording;
    const camera_driver_t *driver;
    void                  *driver_ctx;
    bool                   requires_ble;    /* mirrors the driver registration flag */

    /* Mismatch correction (§13.4) */
    esp_timer_handle_t poll_timer;
    /* Monotonic deadline (esp_timer_get_time() µs) until which the mismatch
     * poll suppresses dispatch for this slot — set to now + RECORDING_STATUS_
     * GRACE_MS after every issued Start/Stop so the camera has time to reflect
     * the new state in its status report.  0 means "no grace active". */
    int64_t            grace_until_us;
} camera_slot_t;

typedef struct {
    const camera_driver_t *driver;
    camera_model_match_fn  matches;
    camera_ctx_create_fn   create_ctx;
    bool                   requires_ble;
} driver_reg_t;

/* ---- Module state ---- */
static camera_slot_t     s_slots[CAMERA_MAX_SLOTS];
static int               s_slot_count;
static bool              s_auto_control = true;
static SemaphoreHandle_t s_mutex;
static driver_reg_t      s_drivers[MAX_DRIVER_REGS];
static int               s_driver_count;

/* ---- Forward declarations ---- */
static void     poll_timer_cb(void *arg);
static void     start_poll_timer(int slot);
static void     stop_poll_timer(int slot);
static esp_err_t load_slot_from_nvs(int slot);
static esp_err_t save_slot_to_nvs(int slot);

/* ---- Inline helpers ---- */
static inline bool slot_valid(int s) { return s >= 0 && s < s_slot_count; }
/* Recursive so driver ctx_create() callbacks (invoked under lock by
 * set_model / register_driver) can call back into camera_manager APIs without
 * self-deadlocking. */
static inline void lock(void)   { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

static void nvs_namespace(int slot, char *buf, size_t len)
{
    snprintf(buf, len, "cam_%d", slot);
}

/* Returns the absolute esp_timer deadline at which a freshly-armed
 * post-shutter grace window expires. */
static inline int64_t grace_deadline(void)
{
    return esp_timer_get_time() + (int64_t)RECORDING_STATUS_GRACE_MS * 1000;
}

/* ================================================================
 * Init
 * ================================================================ */

void camera_manager_init(void)
{
    ESP_LOGI(TAG, "initiating camera manager..");
    s_mutex = xSemaphoreCreateRecursiveMutex();
    assert(s_mutex != NULL);

    memset(s_slots, 0, sizeof(s_slots));
    s_slot_count   = 0;
    s_driver_count = 0;
    s_auto_control = true;

    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        s_slots[i].ble_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    /* Load persisted slots; gaps are left as unconfigured */
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        esp_err_t err = load_slot_from_nvs(i);
        if (err == ESP_OK) {
            if (s_slot_count <= i) s_slot_count = i + 1;
            ESP_LOGI(TAG, "slot %d: loaded '%s' model=%d", i,
                     s_slots[i].name, (int)s_slots[i].model);
        } else if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_INVALID_VERSION) {
            ESP_LOGW(TAG, "slot %d: NVS read error %s — left unconfigured",
                     i, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "init complete: %d slot(s) loaded", s_slot_count);
}

/* ================================================================
 * Driver registration (§21.4)
 * ================================================================ */

esp_err_t camera_manager_register_driver(const camera_driver_t *driver,
                                          camera_model_match_fn   matches,
                                          camera_ctx_create_fn    create_ctx,
                                          bool                    requires_ble)
{
    if (!driver || !matches || !create_ctx) return ESP_ERR_INVALID_ARG;
    if (s_driver_count >= MAX_DRIVER_REGS) {
        ESP_LOGE(TAG, "register_driver: table full");
        return ESP_ERR_NO_MEM;
    }

    s_drivers[s_driver_count++] = (driver_reg_t){
        .driver      = driver,
        .matches     = matches,
        .create_ctx  = create_ctx,
        .requires_ble = requires_ble,
    };

    /* Assign to already-loaded matching slots */
    lock();
    for (int i = 0; i < s_slot_count; i++) {
        camera_slot_t *sl = &s_slots[i];
        if (sl->is_configured && !sl->driver && matches(sl->model)) {
            sl->driver      = driver;
            sl->driver_ctx  = create_ctx(i);
            sl->requires_ble = requires_ble;
            ESP_LOGI(TAG, "slot %d: driver assigned on register", i);
        }
    }

    /* Warn for any configured slots still without a driver */
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].is_configured && !s_slots[i].driver) {
            ESP_LOGW(TAG, "slot %d: no driver for model %d",
                     i, (int)s_slots[i].model);
        }
    }
    unlock();

    return ESP_OK;
}

/* ================================================================
 * Slot lookup
 * ================================================================ */

int camera_manager_find_by_mac(const uint8_t mac[6])
{
    lock();
    for (int i = 0; i < s_slot_count; i++) {
        if (!s_slots[i].is_configured) continue;
        if (memcmp(s_slots[i].mac, mac, 6) == 0) {
            unlock();
            return i;
        }
    }
    unlock();
    return -1;
}

int camera_manager_register_new(const uint8_t mac[6])
{
    int existing = camera_manager_find_by_mac(mac);
    if (existing >= 0) return existing;

    lock();
    if (s_slot_count >= CAMERA_MAX_SLOTS) {
        unlock();
        ESP_LOGW(TAG, "register_new: no slots available (max %d)", CAMERA_MAX_SLOTS);
        return -1;
    }

    int slot = s_slot_count++;
    camera_slot_t *sl = &s_slots[slot];
    memset(sl, 0, sizeof(*sl));
    sl->ble_handle        = BLE_HS_CONN_HANDLE_NONE;
    sl->is_configured     = true;
    sl->model             = CAMERA_MODEL_UNKNOWN;
    sl->desired_recording = DESIRED_RECORDING_UNKNOWN;
    memcpy(sl->mac, mac, 6);
    unlock();

    ESP_LOGI(TAG, "slot %d: registered new camera (model TBD)", slot);
    return slot;
}

/* ================================================================
 * BLE state transitions
 * ================================================================ */

void camera_manager_on_ble_connected(int slot, uint16_t conn_handle)
{
    if (!slot_valid(slot)) return;
    lock();
    s_slots[slot].ble_handle = conn_handle;
    s_slots[slot].ble_status = CAM_BLE_CONNECTED;
    unlock();
    ESP_LOGI(TAG, "slot %d: BLE connected (handle %u)", slot, conn_handle);
}

void camera_manager_on_ble_ready(int slot)
{
    if (!slot_valid(slot)) return;
    lock();
    s_slots[slot].ble_status = CAM_BLE_READY;
    unlock();
    ESP_LOGI(TAG, "slot %d: BLE ready", slot);
}

void camera_manager_on_ble_disconnected_by_handle(uint16_t conn_handle)
{
    int found = -1;
    bool was_ready = false;

    lock();
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].ble_handle == conn_handle) {
            s_slots[i].ble_handle = BLE_HS_CONN_HANDLE_NONE;
            s_slots[i].ble_status = CAM_BLE_NONE;

            /* For BLE-control cameras, wifi_status is overloaded as the
             * universal "fully ready" flag (set by the driver's readiness
             * sequence).  Clear it on disconnect so is_recording derivation
             * and the UI status mapping see the camera as no-longer-ready. */
            if (s_slots[i].requires_ble) {
                was_ready = (s_slots[i].wifi_status == WIFI_CAM_READY);
                s_slots[i].wifi_status     = WIFI_CAM_NONE;
                s_slots[i].grace_until_us  = 0;
            }
            found = i;
            break;
        }
    }
    unlock();

    if (found < 0) return;

    /* Stop the mismatch poll timer if it was armed (it's started inside
     * on_camera_ready, which is what set wifi_status=READY for BLE slots). */
    if (was_ready) {
        stop_poll_timer(found);
    }

    ESP_LOGI(TAG, "slot %d: BLE disconnected", found);
}

/* ================================================================
 * Slot field updates
 * ================================================================ */

void camera_manager_set_model(int slot, camera_model_t model)
{
    if (!slot_valid(slot)) return;
    lock();
    s_slots[slot].model = model;

    /* Assign driver now if not already assigned */
    if (!s_slots[slot].driver) {
        for (int d = 0; d < s_driver_count; d++) {
            if (s_drivers[d].matches(model)) {
                s_slots[slot].driver      = s_drivers[d].driver;
                s_slots[slot].driver_ctx  = s_drivers[d].create_ctx(slot);
                s_slots[slot].requires_ble = s_drivers[d].requires_ble;
                ESP_LOGI(TAG, "slot %d: driver assigned after model set", slot);
                break;
            }
        }
        if (!s_slots[slot].driver) {
            ESP_LOGW(TAG, "slot %d: no driver for model %d", slot, (int)model);
        }
    }
    unlock();
}

void camera_manager_set_name(int slot, const char *name)
{
    if (!slot_valid(slot) || !name) return;
    lock();
    strncpy(s_slots[slot].name, name, sizeof(s_slots[slot].name) - 1);
    s_slots[slot].name[sizeof(s_slots[slot].name) - 1] = '\0';
    unlock();
}

/* ================================================================
 * Camera-ready transition
 * ================================================================ */

void camera_manager_on_camera_ready(int slot)
{
    if (!slot_valid(slot)) return;
    lock();
    s_slots[slot].wifi_status = WIFI_CAM_READY;
    unlock();

    /* Start mismatch poll timer — must not hold mutex while creating timer */
    start_poll_timer(slot);

    ESP_LOGI(TAG, "slot %d: camera ready", slot);
}

void camera_manager_on_wifi_disconnected(int slot)
{
    if (!slot_valid(slot)) return;

    /* Stop timer before acquiring mutex — timer callback acquires mutex */
    stop_poll_timer(slot);

    lock();
    const camera_driver_t *drv     = s_slots[slot].driver;
    void                  *drv_ctx = s_slots[slot].driver_ctx;
    s_slots[slot].wifi_status      = WIFI_CAM_NONE;
    s_slots[slot].ip_addr          = 0;
    s_slots[slot].grace_until_us   = 0;
    unlock();

    if (drv && drv->on_wifi_disconnected) {
        drv->on_wifi_disconnected(drv_ctx);
    }

    ESP_LOGI(TAG, "slot %d: wifi disconnected", slot);
}

void camera_manager_on_camera_unresponsive(int slot)
{
    if (!slot_valid(slot)) return;

    bool was_ready;
    lock();
    was_ready = (s_slots[slot].wifi_status == WIFI_CAM_READY);
    if (was_ready) {
        s_slots[slot].wifi_status    = WIFI_CAM_PROBING;
        s_slots[slot].grace_until_us = 0;
    }
    unlock();

    if (!was_ready) return;

    /* Mismatch poll timer is only armed once on_camera_ready has fired, so a
     * paired stop is safe and required — leaving it running would dispatch
     * start/stop commands to a camera we already know is not answering. */
    stop_poll_timer(slot);

    ESP_LOGI(TAG, "slot %d: camera unresponsive — demoted to PROBING", slot);
}

void camera_manager_on_station_ip(const uint8_t mac[6], uint32_t ip)
{
    int slot = camera_manager_find_by_mac(mac);
    if (slot < 0) return;

    lock();
    s_slots[slot].last_ip         = ip;
    s_slots[slot].wifi_associated = true;  /* DHCP implies association */
    unlock();

    ESP_LOGD(TAG, "slot %d: last_ip updated", slot);
}

void camera_manager_on_station_associated(const uint8_t mac[6])
{
    int slot = camera_manager_find_by_mac(mac);
    if (slot < 0) return;
    lock();
    s_slots[slot].wifi_associated = true;
    unlock();
    ESP_LOGD(TAG, "slot %d: station associated", slot);
}

void camera_manager_on_station_disassociated(const uint8_t mac[6])
{
    int slot = camera_manager_find_by_mac(mac);
    if (slot < 0) return;
    lock();
    s_slots[slot].wifi_associated = false;
    unlock();
    ESP_LOGD(TAG, "slot %d: station disassociated", slot);
}

/* ================================================================
 * NVS persistence (§6.1)
 * ================================================================ */

static esp_err_t load_slot_from_nvs(int slot)
{
    char ns[16];
    nvs_namespace(slot, ns, sizeof(ns));

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NVS_NOT_FOUND;
    if (err != ESP_OK) return err;

    camera_nv_record_t rec;
    memset(&rec, 0, sizeof(rec));
    size_t sz = sizeof(rec);
    err = nvs_get_blob(h, NVS_KEY_CAMERA, &rec, &sz);
    nvs_close(h);
    if (err != ESP_OK) return err;

    if (rec.version != CAMERA_NV_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "slot %d: schema version mismatch (got %d, want %d) — discarding",
                 slot, rec.version, CAMERA_NV_SCHEMA_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    camera_slot_t *sl = &s_slots[slot];
    strncpy(sl->name, rec.name, sizeof(sl->name) - 1);
    sl->name[sizeof(sl->name) - 1] = '\0';
    sl->model               = rec.model;
    memcpy(sl->mac, rec.mac, 6);
    sl->last_ip             = rec.last_ip;
    sl->is_configured       = rec.is_configured;
    sl->first_pair_complete = rec.first_pair_complete;
    sl->ble_handle          = BLE_HS_CONN_HANDLE_NONE;
    sl->desired_recording   = DESIRED_RECORDING_UNKNOWN;

    return ESP_OK;
}

static esp_err_t save_slot_to_nvs(int slot)
{
    char ns[16];
    nvs_namespace(slot, ns, sizeof(ns));

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    camera_slot_t *sl = &s_slots[slot];
    camera_nv_record_t rec = {
        .version             = CAMERA_NV_SCHEMA_VERSION,
        .model               = sl->model,
        .last_ip             = sl->last_ip,
        .is_configured       = sl->is_configured,
        .first_pair_complete = sl->first_pair_complete,
    };
    strncpy(rec.name, sl->name, sizeof(rec.name) - 1);
    memcpy(rec.mac, sl->mac, 6);

    err = nvs_set_blob(h, NVS_KEY_CAMERA, &rec, sizeof(rec));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t camera_manager_save_slot(int slot)
{
    if (!slot_valid(slot)) return ESP_ERR_INVALID_ARG;
    lock();
    if (s_slots[slot].model == CAMERA_MODEL_UNKNOWN) {
        unlock();
        ESP_LOGE(TAG, "slot %d: save refused — model is UNKNOWN (§6.1)", slot);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = save_slot_to_nvs(slot);
    unlock();
    if (err != ESP_OK)
        ESP_LOGE(TAG, "slot %d: NVS save failed: %s", slot, esp_err_to_name(err));
    return err;
}

esp_err_t camera_manager_mark_first_pair_complete(int slot)
{
    if (!slot_valid(slot)) return ESP_ERR_INVALID_ARG;
    lock();
    if (s_slots[slot].model == CAMERA_MODEL_UNKNOWN) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_slots[slot].first_pair_complete) {
        unlock();
        return ESP_OK;  /* already set, no NVS write needed */
    }
    s_slots[slot].first_pair_complete = true;
    esp_err_t err = save_slot_to_nvs(slot);
    unlock();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "slot %d: first_pair_complete set", slot);
    } else {
        ESP_LOGW(TAG, "slot %d: first_pair_complete save failed: %s",
                 slot, esp_err_to_name(err));
    }
    return err;
}

esp_err_t camera_manager_clear_first_pair_complete(int slot)
{
    if (!slot_valid(slot)) return ESP_ERR_INVALID_ARG;
    lock();
    if (!s_slots[slot].is_configured) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_slots[slot].first_pair_complete) {
        unlock();
        return ESP_OK;
    }
    s_slots[slot].first_pair_complete = false;
    esp_err_t err = save_slot_to_nvs(slot);
    unlock();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "slot %d: first_pair_complete cleared", slot);
    } else {
        ESP_LOGW(TAG, "slot %d: first_pair_complete clear failed: %s",
                 slot, esp_err_to_name(err));
    }
    return err;
}

/* ================================================================
 * Queries
 * ================================================================ */

uint32_t camera_manager_get_last_ip(int slot)
{
    if (!slot_valid(slot)) return 0;
    lock();
    uint32_t ip = s_slots[slot].last_ip;
    unlock();
    return ip;
}

camera_model_t camera_manager_get_model(int slot)
{
    if (!slot_valid(slot)) return CAMERA_MODEL_UNKNOWN;
    lock();
    camera_model_t m = s_slots[slot].model;
    unlock();
    return m;
}

int camera_manager_get_slot_count(void)
{
    return s_slot_count;
}

int camera_manager_get_configured_count(void)
{
    int n = 0;
    lock();
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].is_configured) n++;
    }
    unlock();
    return n;
}

esp_err_t camera_manager_get_slot_info(int slot, camera_slot_info_t *out)
{
    if (!out || !slot_valid(slot)) return ESP_ERR_INVALID_ARG;

    lock();
    camera_slot_t *sl = &s_slots[slot];
    out->index               = slot;
    out->model               = sl->model;
    out->is_configured       = sl->is_configured;
    out->ble_status          = sl->ble_status;
    out->wifi_status         = sl->wifi_status;
    out->ip_addr             = sl->ip_addr;
    out->desired_recording   = sl->desired_recording;
    out->first_pair_complete = sl->first_pair_complete;
    out->wifi_associated     = sl->wifi_associated;
    memcpy(out->mac, sl->mac, 6);
    strncpy(out->name, sl->name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';

    /* Derive is_recording from driver's cached status (non-blocking §8) */
    out->is_recording = (sl->wifi_status == WIFI_CAM_READY &&
                         sl->driver != NULL &&
                         sl->driver->get_recording_status(sl->driver_ctx) ==
                             CAMERA_RECORDING_ACTIVE);
    unlock();
    return ESP_OK;
}

camera_can_state_t camera_manager_get_slot_can_state(int slot)
{
    if (!slot_valid(slot)) return CAMERA_CAN_STATE_UNDEFINED;

    lock();
    camera_slot_t *sl = &s_slots[slot];
    if (!sl->is_configured) {
        unlock();
        return CAMERA_CAN_STATE_UNDEFINED;
    }
    wifi_cam_status_t ws = sl->wifi_status;
    camera_recording_status_t rs = CAMERA_RECORDING_UNKNOWN;
    if (ws == WIFI_CAM_READY && sl->driver) {
        rs = sl->driver->get_recording_status(sl->driver_ctx);
    }
    unlock();

    if (ws != WIFI_CAM_READY)            return CAMERA_CAN_STATE_DISCONNECTED;
    if (rs == CAMERA_RECORDING_ACTIVE)   return CAMERA_CAN_STATE_RECORDING;
    return CAMERA_CAN_STATE_IDLE;
}

/* ================================================================
 * Recording intent (§13)
 * ================================================================ */

/*
 * Immediate-dispatch path: when the desired state actually transitions and the
 * slot is ready, send the start/stop command now instead of waiting up to one
 * STATUS_POLL_INTERVAL_MS for the mismatch poll to fire.  The poll remains the
 * safety net for slots that weren't ready and for state divergence.
 *
 * Idempotency: callers (notably can_manager on every 0x600 frame) may invoke
 * with the same intent repeatedly — we only dispatch on a real transition.
 *
 * grace_until_us is armed after dispatch so the mismatch poll defers status
 * comparison for RECORDING_STATUS_GRACE_MS, mirroring poll_timer_cb's own
 * behaviour after it issues a command.  10 s is long enough that both BLE
 * status notifications and the WiFi RC `st` poll have caught up before the
 * next mismatch comparison runs.
 */
void camera_manager_set_desired_recording_all(desired_recording_t intent)
{
    const camera_driver_t *drvs[CAMERA_MAX_SLOTS];
    void                  *ctxs[CAMERA_MAX_SLOTS];   /* NULL for broadcast entries */
    int                    slots[CAMERA_MAX_SLOTS];
    bool                   is_broadcast[CAMERA_MAX_SLOTS];
    int                    n = 0;

    /* Tracks broadcast-style drivers already enrolled in this dispatch wave.
     * For these drivers, only the FIRST slot encountered triggers a call;
     * subsequent slots have intent + grace updated but no per-slot dispatch
     * (the broadcast already covered them). */
    const camera_driver_t *seen_broadcast[CAMERA_MAX_SLOTS];
    int                    seen_count = 0;

    lock();
    for (int i = 0; i < s_slot_count; i++) {
        camera_slot_t *sl = &s_slots[i];
        if (sl->desired_recording == intent) continue;
        sl->desired_recording = intent;
        if (intent == DESIRED_RECORDING_UNKNOWN) continue;
        if (!sl->driver || sl->wifi_status != WIFI_CAM_READY) continue;

        if (sl->driver->broadcasts_to_all) {
            bool already_seen = false;
            for (int k = 0; k < seen_count; k++) {
                if (seen_broadcast[k] == sl->driver) { already_seen = true; break; }
            }
            if (already_seen) {
                /* Earlier slot's broadcast already covered this camera. */
                sl->grace_until_us = grace_deadline();
                continue;
            }
            seen_broadcast[seen_count++] = sl->driver;
            drvs[n]         = sl->driver;
            ctxs[n]         = NULL;
            slots[n]        = i;
            is_broadcast[n] = true;
        } else {
            drvs[n]         = sl->driver;
            ctxs[n]         = sl->driver_ctx;
            slots[n]        = i;
            is_broadcast[n] = false;
        }
        sl->grace_until_us = grace_deadline();
        n++;
    }
    unlock();

    for (int j = 0; j < n; j++) {
        if (is_broadcast[j]) {
            if (intent == DESIRED_RECORDING_START) {
                ESP_LOGI(TAG, "slot %d: immediate dispatch — start_recording_all (broadcast)",
                         slots[j]);
                drvs[j]->start_recording_all();
            } else { /* DESIRED_RECORDING_STOP */
                ESP_LOGI(TAG, "slot %d: immediate dispatch — stop_recording_all (broadcast)",
                         slots[j]);
                drvs[j]->stop_recording_all();
            }
        } else {
            if (intent == DESIRED_RECORDING_START) {
                ESP_LOGI(TAG, "slot %d: immediate dispatch — start_recording", slots[j]);
                drvs[j]->start_recording(ctxs[j]);
            } else { /* DESIRED_RECORDING_STOP */
                ESP_LOGI(TAG, "slot %d: immediate dispatch — stop_recording", slots[j]);
                drvs[j]->stop_recording(ctxs[j]);
            }
        }
    }
}

void camera_manager_set_desired_recording_slot(int slot, desired_recording_t intent)
{
    if (!slot_valid(slot)) return;

    const camera_driver_t *drv = NULL;
    void                  *ctx = NULL;

    lock();
    camera_slot_t *sl = &s_slots[slot];
    bool transitioned = (sl->desired_recording != intent);
    sl->desired_recording = intent;
    if (transitioned && intent != DESIRED_RECORDING_UNKNOWN
        && sl->driver && sl->wifi_status == WIFI_CAM_READY) {
        drv = sl->driver;
        ctx = sl->driver_ctx;
        sl->grace_until_us = grace_deadline();
    }
    unlock();

    if (drv) {
        if (intent == DESIRED_RECORDING_START) {
            ESP_LOGI(TAG, "slot %d: immediate dispatch — start_recording", slot);
            drv->start_recording(ctx);
        } else { /* DESIRED_RECORDING_STOP */
            ESP_LOGI(TAG, "slot %d: immediate dispatch — stop_recording", slot);
            drv->stop_recording(ctx);
        }
    }
}

bool camera_manager_get_auto_control(void) { return s_auto_control; }
void camera_manager_set_auto_control(bool enabled) { s_auto_control = enabled; }

/* ================================================================
 * Shutdown helpers (docs/design/shutdown.md)
 * ================================================================ */

esp_err_t camera_manager_invoke_sleep(int slot)
{
    if (!slot_valid(slot)) return ESP_ERR_INVALID_ARG;

    const camera_driver_t *drv = NULL;
    void                  *ctx = NULL;

    lock();
    if (s_slots[slot].driver) {
        drv = s_slots[slot].driver;
        ctx = s_slots[slot].driver_ctx;
    }
    unlock();

    if (!drv || !drv->sleep) return ESP_ERR_NOT_SUPPORTED;
    return drv->sleep(ctx);
}

void camera_manager_teardown_slot(int slot)
{
    if (!slot_valid(slot)) return;

    /* Stop the mismatch poll timer first — it acquires the mutex on tick. */
    stop_poll_timer(slot);

    const camera_driver_t *drv = NULL;
    void                  *ctx = NULL;

    lock();
    camera_slot_t *sl = &s_slots[slot];
    drv = sl->driver;
    ctx = sl->driver_ctx;

    /* Mark the slot as no longer ready so derived state (is_recording,
     * CAN 0x601 state) reflects the disconnect immediately, without waiting
     * for the BLE disconnect callback. */
    sl->wifi_status     = WIFI_CAM_NONE;
    sl->ip_addr         = 0;
    sl->grace_until_us  = 0;
    unlock();

    if (drv && drv->teardown) {
        drv->teardown(ctx);
    }

    ESP_LOGI(TAG, "slot %d: teardown (shutdown path)", slot);
}

/* ================================================================
 * Slot removal with compaction (§20.5)
 * ================================================================ */

esp_err_t camera_manager_remove_slot(int slot)
{
    if (!slot_valid(slot)) return ESP_ERR_INVALID_ARG;

    /*
     * Stop the poll timer before acquiring the mutex.
     * poll_timer_cb acquires the mutex, so we must not hold it here while
     * calling stop_poll_timer — that would deadlock if the timer fires
     * concurrently.  esp_timer_delete() blocks until any in-progress
     * callback has returned, guaranteeing no further firings.
     */
    stop_poll_timer(slot);

    lock();

    camera_slot_t *sl = &s_slots[slot];

    /* Tear down driver resources */
    if (sl->driver && sl->driver->teardown) {
        sl->driver->teardown(sl->driver_ctx);
    }

    /* Erase NVS at the removed slot's original index */
    char ns[16];
    nvs_namespace(slot, ns, sizeof(ns));
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    /* Compact: shift higher slots down by one */
    for (int i = slot; i < s_slot_count - 1; i++) {
        s_slots[i] = s_slots[i + 1];
        if (s_slots[i].driver && s_slots[i].driver->update_slot_index) {
            s_slots[i].driver->update_slot_index(s_slots[i].driver_ctx, i);
        }
        /* Rewrite NVS at the new (lower) index — best-effort */
        if (s_slots[i].model != CAMERA_MODEL_UNKNOWN) {
            save_slot_to_nvs(i);
        }
    }

    /* Clear the now-vacated top slot and erase its stale NVS copy */
    memset(&s_slots[s_slot_count - 1], 0, sizeof(camera_slot_t));
    s_slots[s_slot_count - 1].ble_handle = BLE_HS_CONN_HANDLE_NONE;
    s_slot_count--;

    nvs_namespace(s_slot_count, ns, sizeof(ns));
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    unlock();
    ESP_LOGI(TAG, "slot %d: removed; %d slot(s) remain", slot, s_slot_count);
    return ESP_OK;
}

/* ================================================================
 * Slot reordering (§20.6)
 * ================================================================ */

esp_err_t camera_manager_reorder_slots(const int *new_order, int count)
{
    if (!new_order || count <= 0 || count > CAMERA_MAX_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate indices and check connectivity. */
    lock();
    for (int i = 0; i < count; i++) {
        int src = new_order[i];
        if (src < 0 || src >= s_slot_count) {
            unlock();
            return ESP_ERR_INVALID_ARG;
        }
        camera_slot_t *sl = &s_slots[src];
        if (sl->wifi_status == WIFI_CAM_READY ||
            sl->ble_status  == CAM_BLE_CONNECTED) {
            unlock();
            return ESP_ERR_INVALID_STATE;
        }
    }
    unlock();

    /* Stop all poll timers for affected slots before touching RAM. */
    for (int i = 0; i < count; i++) {
        stop_poll_timer(new_order[i]);
    }

    /* Build the permuted RAM snapshot. */
    camera_slot_t tmp[CAMERA_MAX_SLOTS];
    lock();
    for (int i = 0; i < count; i++) {
        tmp[i] = s_slots[new_order[i]];
    }

    /* Copy permutation back into the live slot array. */
    for (int i = 0; i < count; i++) {
        s_slots[i] = tmp[i];
        if (s_slots[i].driver && s_slots[i].driver->update_slot_index) {
            s_slots[i].driver->update_slot_index(s_slots[i].driver_ctx, i);
        }
    }
    unlock();

    /* Rewrite camera records at their new indices. */
    for (int i = 0; i < count; i++) {
        char ns[16];
        nvs_namespace(i, ns, sizeof(ns));
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        save_slot_to_nvs(i);
    }

    ESP_LOGI(TAG, "slots reordered (%d entries)", count);
    return ESP_OK;
}

/* ================================================================
 * ble_core callbacks (§12.9)
 * ================================================================ */

bool camera_manager_is_known_ble_addr(ble_addr_t addr)
{
    return camera_manager_find_by_mac(addr.val) >= 0;
}

bool camera_manager_has_disconnected_cameras(void)
{
    lock();
    for (int i = 0; i < s_slot_count; i++) {
        camera_slot_t *sl = &s_slots[i];
        if (sl->is_configured && sl->requires_ble &&
            sl->ble_status != CAM_BLE_CONNECTED &&
            sl->ble_status != CAM_BLE_READY) {
            unlock();
            return true;
        }
    }
    unlock();
    return false;
}

/* ================================================================
 * Mismatch poll timer (§13.3 / §13.4)
 * ================================================================ */

static void poll_timer_cb(void *arg)
{
    int slot = (int)(intptr_t)arg;

    lock();
    camera_slot_t *sl = &s_slots[slot];

    if (!sl->driver || sl->wifi_status != WIFI_CAM_READY) {
        unlock();
        return;
    }

    camera_recording_status_t status =
        sl->driver->get_recording_status(sl->driver_ctx);

    /* Grace expires naturally when the deadline passes — no per-tick reset. */
    bool grace_active = (esp_timer_get_time() < sl->grace_until_us);

    mismatch_action_t action =
        mismatch_step(sl->desired_recording, status, grace_active);

    const camera_driver_t *drv = sl->driver;
    void *ctx = sl->driver_ctx;
    unlock();

    if (action == MISMATCH_ACTION_START) {
        ESP_LOGI(TAG, "slot %d: mismatch — issuing start_recording", slot);
        drv->start_recording(ctx);
        lock();
        s_slots[slot].grace_until_us = grace_deadline();
        unlock();
    } else if (action == MISMATCH_ACTION_STOP) {
        ESP_LOGI(TAG, "slot %d: mismatch — issuing stop_recording", slot);
        drv->stop_recording(ctx);
        lock();
        s_slots[slot].grace_until_us = grace_deadline();
        unlock();
    }
}

static void start_poll_timer(int slot)
{
    camera_slot_t *sl = &s_slots[slot];
    if (sl->poll_timer) return;

    esp_timer_create_args_t args = {
        .callback        = poll_timer_cb,
        .arg             = (void *)(intptr_t)slot,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "cam_poll",
    };
    esp_timer_handle_t h;
    if (esp_timer_create(&args, &h) == ESP_OK) {
        sl->poll_timer = h;
        esp_timer_start_periodic(h, (uint64_t)STATUS_POLL_INTERVAL_MS * 1000ULL);
    } else {
        ESP_LOGE(TAG, "slot %d: failed to create poll timer", slot);
    }
}

static void stop_poll_timer(int slot)
{
    camera_slot_t *sl = &s_slots[slot];
    if (!sl->poll_timer) return;
    esp_timer_stop(sl->poll_timer);
    esp_timer_delete(sl->poll_timer);
    sl->poll_timer = NULL;
}
