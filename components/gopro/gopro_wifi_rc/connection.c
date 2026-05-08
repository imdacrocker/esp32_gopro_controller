/*
 * connection.c — Station lifecycle handlers, slot promotion, UDP `cv`
 * identify apply, keepalive watchdog, and per-slot keepalive / WoL-retry
 * timer management.  All functions here run on the work task unless
 * otherwise noted.
 *
 * §17.4, §17.5, §17.6 of camera_manager_design.md.
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera_manager.h"
#include "gopro_model.h"
#include "gopro_wifi_rc_internal.h"

static const char *TAG = "gopro_rc/conn";

/* ---- Internal helpers ---------------------------------------------------- */

static int find_managed_slot(const uint8_t mac[6])
{
    int slot = camera_manager_find_by_mac(mac);
    if (slot < 0) return -1;
    if (!gopro_model_uses_rc_emulation(camera_manager_get_model(slot))) return -1;
    return slot;
}

/* ---- Per-slot keepalive timer -------------------------------------------- */

static void keepalive_timer_cb(void *arg)
{
    int slot = (int)(intptr_t)arg;
    rc_work_cmd_t cmd = { .type = RC_CMD_KEEPALIVE_TICK,
                          .slot_cmd = { .slot = slot } };
    xQueueSend(s_work_queue, &cmd, 0);
}

void rc_arm_keepalive_timer(gopro_wifi_rc_ctx_t *ctx)
{
    if (ctx->keepalive_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback = keepalive_timer_cb,
            .arg      = (void *)(intptr_t)ctx->slot,
            .name     = "rc_keepalive",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &ctx->keepalive_timer));
    }
    /* Stop first so we can call start even if already running. */
    esp_timer_stop(ctx->keepalive_timer);
    ESP_ERROR_CHECK(esp_timer_start_periodic(ctx->keepalive_timer,
                                             (uint64_t)RC_KEEPALIVE_INTERVAL_MS * 1000));
}

void rc_disarm_keepalive_timer(gopro_wifi_rc_ctx_t *ctx)
{
    if (ctx->keepalive_timer) {
        esp_timer_stop(ctx->keepalive_timer);
    }
}

/* ---- Per-slot WoL retry timer -------------------------------------------- */

static void wol_retry_timer_cb(void *arg)
{
    int slot = (int)(intptr_t)arg;
    rc_work_cmd_t cmd = { .type = RC_CMD_WOL_RETRY,
                          .slot_cmd = { .slot = slot } };
    xQueueSend(s_work_queue, &cmd, 0);
}

void rc_arm_wol_retry_timer(gopro_wifi_rc_ctx_t *ctx)
{
    if (ctx->wol_retry_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback = wol_retry_timer_cb,
            .arg      = (void *)(intptr_t)ctx->slot,
            .name     = "rc_wol_retry",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &ctx->wol_retry_timer));
    }
    esp_timer_stop(ctx->wol_retry_timer);
    ESP_ERROR_CHECK(esp_timer_start_periodic(ctx->wol_retry_timer,
                                             (uint64_t)RC_WOL_RETRY_INTERVAL_MS * 1000));
}

void rc_disarm_wol_retry_timer(gopro_wifi_rc_ctx_t *ctx)
{
    if (ctx->wol_retry_timer) {
        esp_timer_stop(ctx->wol_retry_timer);
    }
}

/* ---- Station event handlers ---------------------------------------------- */

void rc_handle_station_associated(const uint8_t mac[6])
{
    int slot = find_managed_slot(mac);
    if (slot < 0) return; /* Unknown or non-RC MAC — ignore. */

    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];

    uint32_t ip = camera_manager_get_last_ip(slot);
    if (ip == 0) {
        /* No DHCP lease yet; CMD_STATION_DHCP will follow when the camera wakes. */
        ESP_LOGD(TAG, "slot %d: associated with no cached IP — waiting for DHCP", slot);
        return;
    }

    /* Camera is associating with a known cached IP (reuse from previous session).
     * It may be asleep; send WoL, prime with a keepalive, and arm the timer. */
    ESP_LOGI(TAG, "slot %d: associated (no DHCP), sending WoL burst", slot);
    rc_send_wol(ip, ctx->mac);
    rc_send_keepalive(ip);
    rc_arm_keepalive_timer(ctx);
}

void rc_handle_station_dhcp(const uint8_t mac[6], uint32_t ip)
{
    int slot = find_managed_slot(mac);
    if (slot < 0) return;

    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    ctx->last_ip = ip;
    memcpy(ctx->mac, mac, 6); /* Keep MAC in sync (also set by driver add flow). */
    camera_manager_save_slot(slot);

    /* Prime the camera with keepalive + status + camera-version so it responds
     * within ms instead of waiting for the next scheduled timer tick.  The cv
     * response gives us model + firmware over UDP; the RX task posts
     * CMD_PROMOTE on the first received datagram and CMD_APPLY_CV when cv
     * specifically arrives. */
    rc_send_keepalive(ip);
    rc_send_st(ip);
    rc_send_cv(ip);

    rc_arm_keepalive_timer(ctx);

    ESP_LOGI(TAG, "slot %d: DHCP ip=%lu — primed, waiting for first response",
             slot, (unsigned long)ip);
}

void rc_handle_station_disconnected(const uint8_t mac[6])
{
    int slot = find_managed_slot(mac);
    if (slot < 0) return;

    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    rc_disarm_keepalive_timer(ctx);
    rc_disarm_wol_retry_timer(ctx);
    ctx->wifi_ready       = false;
    ctx->recording_status = CAMERA_RECORDING_UNKNOWN;
    /* identify_attempted is intentionally NOT cleared — once a probe has run
     * this firmware session, the slot's model is settled (HERO4_* on success,
     * HERO_LEGACY_RC on failure).  Re-running the probe on every reconnect
     * adds no new information and risks misclassifying a transient HTTP
     * failure as a model downgrade. */
    camera_manager_on_wifi_disconnected(slot);

    ESP_LOGI(TAG, "slot %d: disassociated", slot);
}

/* ---- Promotion (replaces probe) ------------------------------------------ */

/*
 * Posted by the UDP RX task when the first datagram arrives from a slot's IP
 * (keepalive ACK, `st` response, `SH` echo, or `cv` response).  Idempotent —
 * duplicate posts are a no-op because the first one flips wifi_ready.
 *
 * Identification logic (UDP-only):
 *   - If a cv response has already been parsed into ctx->parsed_model_name,
 *     apply it directly.
 *   - Otherwise send another cv now.  The keepalive tick will keep retrying
 *     cv every 3 s until a response arrives (some cameras drop the very
 *     first cv on a fresh pair before the keepalive cycle is established).
 */
void rc_handle_promote(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    if (ctx->wifi_ready) return;

    ctx->wifi_ready = true;
    camera_manager_on_camera_ready(slot);
    ESP_LOGI(TAG, "slot %d: promoted — camera ready", slot);

    if (!ctx->identify_attempted && ctx->parsed_model_name[0] != '\0') {
        ctx->identify_attempted = true;
        ESP_LOGI(TAG, "slot %d: cv-identified at promote — model='%s' fw='%s'",
                 slot, ctx->parsed_model_name, ctx->parsed_firmware);
        camera_model_t mapped = gopro_model_from_name(ctx->parsed_model_name);
        camera_manager_set_model(slot, mapped);
        camera_manager_save_slot(slot);
    } else if (!ctx->identify_attempted) {
        /* cv hasn't arrived yet — kick another one off; keepalive_tick will
         * keep retrying every 3 s until the camera answers.  No HTTP probe. */
        rc_send_cv(ctx->last_ip);
    }

    /* Date/time is internally gated on gopro_model_supports_http_datetime() —
     * a no-op on every model except Hero4 Black/Silver. */
    rc_send_datetime(slot);
}

/* ---- Apply parsed cv data (work task) ------------------------------------ *
 *
 * Posted by the UDP RX task (rc_parse_cv_response) when a `cv` reply arrives
 * AFTER promote has already run — typically because the camera's keepalive
 * ACK reached us before the cv response.  Maps the parsed model_name string
 * to the camera_model_t enum and persists it (NVS-saved), then marks
 * identify_attempted so keepalive_tick stops re-sending `cv`.  The slot's
 * name field is intentionally left blank — there is no known WiFi RC
 * protocol path to retrieve the user-set camera name, and the model_name
 * string carried by `cv` is the model identity, not a user-set name.
 */
void rc_handle_apply_cv(int slot)
{
    if (slot < 0 || slot >= CAMERA_MAX_SLOTS) return;
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    if (ctx->parsed_model_name[0] == '\0') return;  /* nothing to apply */

    camera_model_t mapped = gopro_model_from_name(ctx->parsed_model_name);
    ESP_LOGI(TAG, "slot %d: applying cv data — model='%s' (enum=%d) fw='%s'",
             slot, ctx->parsed_model_name, (int)mapped, ctx->parsed_firmware);

    camera_manager_set_model(slot, mapped);
    camera_manager_save_slot(slot);

    ctx->identify_attempted = true;
}

/* ---- Keepalive tick handler ---------------------------------------------- */

/*
 * Send a keepalive every RC_KEEPALIVE_INTERVAL_MS, then check the silence
 * window: if no UDP datagram (ACK or `st` response) has arrived from the camera
 * for RC_KEEPALIVE_SILENCE_MS, arm the WoL retry timer.  When traffic resumes
 * the RX task refreshes last_response_tick and the next tick disarms the timer.
 *
 * Also re-send `cv` on every tick until identify_attempted flips true — the
 * camera reliably ignores the very first cv on a fresh pair (it arrives
 * before the RC pairing has fully settled), so we keep nudging it once per
 * keepalive cycle.  apply_cv flips identify_attempted when a cv response
 * lands, terminating the retries.
 */
void rc_handle_keepalive_tick(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];

    rc_send_keepalive(ctx->last_ip);

    if (!ctx->identify_attempted) {
        rc_send_cv(ctx->last_ip);
    }

    TickType_t now     = xTaskGetTickCount();
    TickType_t silence = now - ctx->last_response_tick;

    bool silent = (ctx->last_response_tick != 0 &&
                   silence > pdMS_TO_TICKS(RC_KEEPALIVE_SILENCE_MS));

    if (silent) {
        if (ctx->wol_retry_timer == NULL ||
            !esp_timer_is_active(ctx->wol_retry_timer)) {
            ESP_LOGW(TAG, "slot %d: silence %lu ms — arming WoL retry",
                     slot, (unsigned long)(silence * portTICK_PERIOD_MS));
            rc_arm_wol_retry_timer(ctx);
        }
    } else if (ctx->wol_retry_timer && esp_timer_is_active(ctx->wol_retry_timer)) {
        rc_disarm_wol_retry_timer(ctx);
    }
}

/* ---- WoL retry handler --------------------------------------------------- */

void rc_handle_wol_retry(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    ESP_LOGD(TAG, "slot %d: WoL retry", slot);
    rc_send_wol(ctx->last_ip, ctx->mac);
    rc_send_keepalive(ctx->last_ip);
}
