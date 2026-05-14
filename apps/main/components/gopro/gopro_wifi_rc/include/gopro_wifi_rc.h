/*
 * gopro_wifi_rc.h — Public API for the GoPro WiFi Remote emulation driver.
 *
 * Implements camera_driver_t for Hero3 / Hero3+ / Hero4 / Hero5 / Hero6 / Hero7
 * cameras in their "Smart Remote" mode.  Cameras connect to the SoftAP
 * automatically when they see the correct SSID (HERO-RC-XXXXXX) and OUI
 * (d8:96:85) — both configured by wifi_manager.
 *
 * Every recurring exchange — keepalive, status, shutter, identify — is a
 * short binary UDP datagram (local port 8383, remote 8484).  HTTP/1.0 is
 * used only for the optional date/time set on cameras for which
 * gopro_model_supports_http_datetime() returns true (Hero4 Black/Silver
 * today).  No BLE is used.
 *
 * §17 of docs/design/camera-manager.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Register the RC-emulation driver with camera_manager, start the work and
 * shutter tasks, open the UDP socket, and start the global status-poll timer.
 *
 * Must be called after camera_manager_init() and before wifi_manager_init().
 */
void gopro_wifi_rc_init(void);

/* ---- Station lifecycle callbacks (wired in main.c §21.3) ---------------- */

/* Called by main on_station_associated.  Must not block. */
void gopro_wifi_rc_on_station_associated(const uint8_t mac[6]);

/* Called by main on_station_dhcp.  Must not block. */
void gopro_wifi_rc_on_station_dhcp(const uint8_t mac[6], uint32_t ip);

/* Called by main on_station_disassociated.  Must not block. */
void gopro_wifi_rc_on_station_disassociated(const uint8_t mac[6]);

/* ---- Manual add from web UI (POST /api/rc/add) --------------------------- */

/*
 * Register mac as a new RC-emulation camera, seed it with ip, and prime the
 * camera with a keepalive + `st` + `cv` UDP burst.  Called by http_server;
 * mac must already be associated to the SoftAP with a valid DHCP lease.
 *
 * Slot is registered with model = CAMERA_MODEL_GOPRO_HERO_LEGACY_RC; the
 * camera's `cv` response (parsed asynchronously by the RX task) upgrades
 * this to the specific Hero4 / Hero7 / etc. model.  The slot's name field
 * is left blank — there is no known WiFi RC protocol path to retrieve the
 * user-set camera name.  If the camera never answers `cv`, the slot stays
 * at LEGACY_RC and UDP control still works — only the resolved model and
 * HTTP-datetime capability check are missing.
 */
void gopro_wifi_rc_add_camera(const uint8_t mac[6], uint32_t ip);

/*
 * Tear down resources for slot.  Called from http_server before it invokes
 * camera_manager_remove_slot(); driver teardown is also called by
 * camera_manager_remove_slot() itself, so this is only needed for cleanup
 * that must happen before the slot record is erased.
 */
void gopro_wifi_rc_remove_camera(int slot);

/* ---- Predicates used by http_server for /api/rc/discovered --------------- */

/* True if slot is managed by this driver (uses RC-emulation model). */
bool gopro_wifi_rc_is_managed_slot(int slot);

/* True if mac belongs to a configured RC-emulation camera slot. */
bool gopro_wifi_rc_is_managed_mac(const uint8_t mac[6]);

/* ---- UTC sync (called from main.c on_utc_acquired) ----------------------- */

/*
 * Send the current date/time to all wifi-ready RC-emulation cameras.
 * Reads system time via time(); requires the system clock to be set by
 * can_manager before this fires (first valid GPS frame from RaceCapture).
 */
void gopro_wifi_rc_sync_time_all(void);

/* ---- DIAGNOSTIC (dev-only — not currently dispatched) ------------------- *
 *
 * Spawn a one-shot task that runs a battery of network probes against the
 * camera at (mac, ip) and logs the results to the serial console:
 *   - ICMP ping (5 packets)
 *   - TCP port sweep across common GoPro / general ports
 *   - HTTP/1.1 GET probes on any responding TCP ports against several known
 *     GoPro endpoint paths
 *   - Extra UDP opcode probes (`cv` GET-form 0/1, `wt`); replies are logged
 *     by the existing rc_udp_rx_task
 *
 * Used in 2026-05 to confirm Hero7's STA-mode behaviour (all TCP closed,
 * UDP `cv` works) which led to the cv-based identify path replacing HTTP.
 * Kept compiled-in for future hardware investigations; to wire it back to
 * the /api/rc/add endpoint, swap gopro_wifi_rc_add_camera() for
 * gopro_wifi_rc_diagnose() in components/http_server/api_rc.c.
 */
void gopro_wifi_rc_diagnose(const uint8_t mac[6], uint32_t ip);
