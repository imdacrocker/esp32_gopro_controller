#include "camera_manager.h"
#include "camera_manager_ble.h"   /* ble_addr_t, BLE_HS_CONN_HANDLE_NONE */
#include "cam_core.h"
#include "esp_log.h"
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

/* Internal per-slot state.
 *
 * The variant-agnostic slice (driver vtable + ctx, recording intent,
 * grace deadline, poll timer, ready flag, requires_ble) lives in the
 * embedded `core` field, owned by cam_core.  This file MUST NOT read or
 * write `core.*` fields directly — all access goes through the cam_core
 * APIs which serialize them under cam_core's mutex.
 */
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

    /* Variant-agnostic recording / driver state — owned by cam_core. */
    cam_core_slot_t   core;
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
static SemaphoreHandle_t s_mutex;
static driver_reg_t      s_drivers[MAX_DRIVER_REGS];
static int               s_driver_count;

/* ---- Forward declarations ---- */
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

/* ================================================================
 * Init
 * ================================================================ */

void camera_manager_init(void)
{
    ESP_LOGI(TAG, "initiating camera manager..");
    s_mutex = xSemaphoreCreateRecursiveMutex();
    assert(s_mutex != NULL);

    cam_core_init();

    memset(s_slots, 0, sizeof(s_slots));
    s_slot_count   = 0;
    s_driver_count = 0;

    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        s_slots[i].ble_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    /* Load persisted slots; gaps are left as unconfigured.  Each loaded
     * slot registers its embedded cam_core_slot_t with cam_core so the
     * shared engine can dispatch/poll on it. */
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        esp_err_t err = load_slot_from_nvs(i);
        if (err == ESP_OK) {
            if (s_slot_count <= i) s_slot_count = i + 1;
            cam_core_register_slot(i, &s_slots[i].core);
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
 * ================================================================
 *
 * INIT-TIME-ONLY CONTRACT: the write to s_drivers[s_driver_count++] below
 * is intentionally OUTSIDE the lock.  This is safe ONLY because every
 * caller (gopro_wifi_rc_init at app_main:109, open_gopro_ble_init at
 * app_main:114) runs sequentially during app_main before any other task
 * spawns — at registration time there is no concurrency to race against.
 * The follow-up "Assign to already-loaded matching slots" loop acquires
 * the lock because it reads s_slots[] which CAN be concurrently touched
 * by the time later registrations happen.
 *
 * Do NOT call camera_manager_register_driver from a runtime context (UI,
 * HTTP handler, timer callback, BLE host task).  If a future need to
 * register drivers at runtime arises, the write at line 188 below must
 * move inside the lock and s_driver_count must be re-read after each
 * lock acquisition.
 */
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

    /* cam_core_slot_attach_driver asserts the vtable's non-NULL contract
     * (start_recording / stop_recording / get_recording_status) at attach
     * time below, so contract violations crash at boot rather than at the
     * first poll. */

    s_drivers[s_driver_count++] = (driver_reg_t){
        .driver      = driver,
        .matches     = matches,
        .create_ctx  = create_ctx,
        .requires_ble = requires_ble,
    };

    /* Assign to already-loaded matching slots (first-attach-wins; later
     * drivers don't overwrite earlier matches). */
    lock();
    for (int i = 0; i < s_slot_count; i++) {
        camera_slot_t *sl = &s_slots[i];
        if (sl->is_configured && !cam_core_slot_has_driver(i) && matches(sl->model)) {
            void *ctx = create_ctx(i);
            esp_err_t err = cam_core_slot_attach_driver(i, driver, ctx, requires_ble);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "slot %d: driver assigned on register", i);
            } else {
                ESP_LOGW(TAG, "slot %d: cam_core attach failed: %s",
                         i, esp_err_to_name(err));
            }
        }
    }

    /* Warn for any configured slots still without a driver */
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].is_configured && !cam_core_slot_has_driver(i)) {
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
    memcpy(sl->mac, mac, 6);
    /* Embedded core was zeroed by the memset above; register its address
     * with cam_core so this slot participates in dispatch/poll. */
    cam_core_register_slot(slot, &sl->core);
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
    int  found      = -1;
    bool was_ready  = false;
    bool requires_ble = false;

    lock();
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].ble_handle == conn_handle) {
            s_slots[i].ble_handle = BLE_HS_CONN_HANDLE_NONE;
            s_slots[i].ble_status = CAM_BLE_NONE;
            requires_ble = cam_core_slot_requires_ble(i);

            /* For BLE-control cameras, wifi_status is overloaded as the
             * universal "fully ready" flag (set by the driver's readiness
             * sequence).  Clear it on disconnect so is_recording derivation
             * and the UI status mapping see the camera as no-longer-ready. */
            if (requires_ble) {
                was_ready = (s_slots[i].wifi_status == WIFI_CAM_READY);
                s_slots[i].wifi_status = WIFI_CAM_NONE;
            }
            found = i;
            break;
        }
    }
    unlock();

    if (found < 0) return;

    /* For BLE slots, mirror the universal ready=false into cam_core so the
     * mismatch poll timer is stopped (it was started by on_camera_ready
     * for both transports). */
    if (was_ready) {
        cam_core_slot_set_ready(found, false);
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

    /* Assign driver now if not already assigned (first-match-wins) */
    if (!cam_core_slot_has_driver(slot)) {
        bool assigned = false;
        for (int d = 0; d < s_driver_count; d++) {
            if (s_drivers[d].matches(model)) {
                void *ctx = s_drivers[d].create_ctx(slot);
                if (cam_core_slot_attach_driver(slot, s_drivers[d].driver,
                                                 ctx, s_drivers[d].requires_ble)
                    == ESP_OK) {
                    ESP_LOGI(TAG, "slot %d: driver assigned after model set", slot);
                    assigned = true;
                }
                break;
            }
        }
        if (!assigned) {
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

    /* Mirror into cam_core; this also starts the mismatch poll timer. */
    cam_core_slot_set_ready(slot, true);

    ESP_LOGI(TAG, "slot %d: camera ready", slot);
}

void camera_manager_on_wifi_disconnected(int slot)
{
    if (!slot_valid(slot)) return;

    /* Stop mismatch poll + clear ready first (must not be done under our
     * own mutex — see cam_core_slot_set_ready threading notes). */
    cam_core_slot_set_ready(slot, false);

    lock();
    s_slots[slot].wifi_status = WIFI_CAM_NONE;
    s_slots[slot].ip_addr     = 0;
    unlock();

    /* Fire the driver's on_wifi_disconnected hook (RC-emulation cameras
     * use it to stop in-flight network work). */
    cam_core_notify_wifi_disconnected(slot);

    ESP_LOGI(TAG, "slot %d: wifi disconnected", slot);
}

void camera_manager_on_camera_unresponsive(int slot)
{
    if (!slot_valid(slot)) return;

    bool was_ready;
    lock();
    was_ready = (s_slots[slot].wifi_status == WIFI_CAM_READY);
    if (was_ready) {
        s_slots[slot].wifi_status = WIFI_CAM_PROBING;
    }
    unlock();

    if (!was_ready) return;

    /* Mirror into cam_core: clears ready + stops mismatch poll.  The
     * timer was only armed once on_camera_ready fired, so a paired stop
     * is safe and required — leaving it running would dispatch
     * start/stop commands to a camera we already know is not answering. */
    cam_core_slot_set_ready(slot, false);

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
    /* Recording intent isn't persisted — cam_core's slot record is
     * zero-initialized at struct allocation, which leaves desired at
     * DESIRED_RECORDING_UNKNOWN.  No explicit assignment needed. */

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
    out->first_pair_complete = sl->first_pair_complete;
    out->wifi_associated     = sl->wifi_associated;
    memcpy(out->mac, sl->mac, 6);
    strncpy(out->name, sl->name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
    unlock();

    /* cam_core-owned fields fetched outside our mutex — they take cam_core's
     * lock independently. */
    out->desired_recording = cam_core_get_desired(slot);
    out->is_recording      = cam_core_is_recording(slot);
    return ESP_OK;
}

/* Recording intent + auto-control + can-state + sleep + teardown all
 * live in cam_core; callers (wireless or shared) call cam_core_* APIs
 * directly.  The wireless-only `wifi_status` / `ip_addr` clear that the
 * old camera_manager_teardown_slot() wrapper performed has been dropped:
 * teardown is only used on the shutdown path, where the small window of
 * stale wifi_status is invisible (the system is going down anyway).
 *
 * Slot-removal compaction below still uses cam_core_teardown_slot for
 * the per-slot timer + driver teardown before shifting slot data. */

/* ================================================================
 * Slot removal with compaction (§20.5)
 * ================================================================ */

esp_err_t camera_manager_remove_slot(int slot)
{
    if (!slot_valid(slot)) return ESP_ERR_INVALID_ARG;

    /* cam_core stops the poll timer, clears ready, and invokes the
     * driver's teardown — must run unlocked because the timer callback
     * itself takes cam_core's lock and esp_timer_delete blocks for
     * in-flight callbacks. */
    cam_core_teardown_slot(slot);

    /* The compaction below moves each higher slot's embedded cam_core_slot_t
     * (including core.poll_timer) to a lower index, but cam_core baked the OLD
     * index into the timer's callback arg — so a moved timer would misroute the
     * mismatch poll.  Stop the poll timer of every READY slot above the removed
     * one now and recreate it at the new index after the shift.  This uses
     * cam_core_slot_set_ready(false) (timer only), NOT teardown, so the live
     * driver/connection of those still-connected cameras is untouched.
     * set_ready must run UNLOCKED (it deletes timers whose callbacks take
     * cam_core's lock), so snapshot which slots were ready first. */
    int  orig_count;
    bool was_ready[CAMERA_MAX_SLOTS] = { false };
    lock();
    orig_count = s_slot_count;
    for (int j = slot + 1; j < orig_count; j++) {
        was_ready[j] = (s_slots[j].wifi_status == WIFI_CAM_READY);
    }
    unlock();
    for (int j = slot + 1; j < orig_count; j++) {
        if (was_ready[j]) cam_core_slot_set_ready(j, false);
    }

    lock();

    /* Erase NVS at the removed slot's original index */
    char ns[16];
    nvs_namespace(slot, ns, sizeof(ns));
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    /* Compact: shift higher slots down by one.  The struct copy moves
     * the embedded cam_core_slot_t along with the wireless fields, so
     * cam_core's registry pointer (&s_slots[i].core, stable) keeps
     * pointing at the right memory.  Poll-timer index rebinding is handled by
     * the set_ready(false)/set_ready(true) bracket around this block. */
    for (int i = slot; i < s_slot_count - 1; i++) {
        s_slots[i] = s_slots[i + 1];
        /* Tell the driver its slot index changed (for any internal
         * accounting it keeps). */
        cam_core_notify_slot_index_changed(i);
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

    /* Unregister the now-empty top slot from cam_core (drops the pointer
     * from its registry so iteration/queries skip it). */
    cam_core_unregister_slot(s_slot_count);

    /* Re-arm the formerly-ready shifted slots at their NEW index (j-1) so a
     * fresh poll timer is created bound to the correct index.  All such indices
     * are < the new s_slot_count, i.e. still-registered slots. */
    for (int j = slot + 1; j < orig_count; j++) {
        if (was_ready[j]) cam_core_slot_set_ready(j - 1, true);
    }

    ESP_LOGI(TAG, "slot %d: removed; %d slot(s) remain", slot, s_slot_count);
    return ESP_OK;
}

/* ================================================================
 * Slot reordering (§20.6)
 * ================================================================ */

esp_err_t camera_manager_reorder_slots(const int *new_order, int count)
{
    /* Strict-permutation contract: `count` must equal s_slot_count and
     * `new_order` must contain each index in [0, s_slot_count) exactly once.
     *
     * Rejecting non-permutations up-front prevents two silent-data-loss
     * paths that the previous loose validation allowed:
     *
     *   - Truncation (count < s_slot_count): tail slots at indices
     *     [count, s_slot_count) were left untouched in RAM and NVS while
     *     s_slot_count stayed unchanged, and indices [0, count) were
     *     overwritten — so a "shrink the list" call would lose old slot 0
     *     entirely and duplicate the tail across two slots.
     *
     *   - Duplicates in new_order: e.g. order=[0,0,0] would copy slot 0
     *     into every position and overwrite slots 1 and 2 in NVS.
     *
     * For explicit removal, use camera_manager_remove_slot instead. */
    lock();
    if (!reorder_is_valid_permutation(new_order, count, s_slot_count)) {
        unlock();
        return ESP_ERR_INVALID_ARG;
    }

    /* Reject if any source slot is currently connected — reordering a live
     * driver context out from under itself would orphan its state. */
    for (int i = 0; i < count; i++) {
        camera_slot_t *sl = &s_slots[new_order[i]];
        if (sl->wifi_status == WIFI_CAM_READY ||
            sl->ble_status  == CAM_BLE_CONNECTED) {
            unlock();
            return ESP_ERR_INVALID_STATE;
        }
    }
    unlock();

    /* Tear down poll timers for affected slots before touching RAM
     * (cam_core_teardown_slot also runs the driver teardown — but no slot
     * is READY here, so it's a no-op for an idle slot).  Pre-reorder
     * cleanup mirrors the pre-split semantics. */
    for (int i = 0; i < count; i++) {
        cam_core_teardown_slot(new_order[i]);
    }

    /* Build the permuted RAM snapshot. */
    camera_slot_t tmp[CAMERA_MAX_SLOTS];
    lock();
    for (int i = 0; i < count; i++) {
        tmp[i] = s_slots[new_order[i]];
    }

    /* Copy permutation back into the live slot array.  Embedded cam_core
     * state moves with the struct copy; cam_core's registry pointers are
     * stable (always &s_slots[i].core), so iteration sees the new data
     * automatically. */
    for (int i = 0; i < count; i++) {
        s_slots[i] = tmp[i];
        cam_core_notify_slot_index_changed(i);
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
    /* `requires_ble` lives in cam_core; everything else (`is_configured`,
     * `ble_status`) is wireless.  Snapshot the wireless side under our
     * mutex, then consult cam_core per-slot.  cam_core_slot_requires_ble
     * returns false when no driver is attached, so newly-paired slots
     * (model UNKNOWN, awaiting driver attach) don't falsely count as
     * "disconnected BLE cameras" here. */
    bool any = false;
    lock();
    for (int i = 0; i < s_slot_count; i++) {
        camera_slot_t *sl = &s_slots[i];
        if (sl->is_configured &&
            sl->ble_status != CAM_BLE_CONNECTED &&
            sl->ble_status != CAM_BLE_READY &&
            cam_core_slot_requires_ble(i)) {
            any = true;
            break;
        }
    }
    unlock();
    return any;
}

