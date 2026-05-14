#pragma once

#include <stdint.h>

#define AP_CHANNEL           11     /* 2462 MHz — clear of BLE adv channels 37/38/39 (§4.2) */
#define AP_MAX_CONN           6     /* 4 cameras + 1 setup device + 1 spare */
#define AP_READY_TIMEOUT_MS  5000

/* SoftAP SSID is "<prefix><last-3-MAC-bytes>" so the recovery app and main app
 * advertise the same SSID on the same hardware (same MAC -> same suffix).
 * Auth is OPEN (no PSK) — closed CAN-bus device, no PII. See docs/design/ota.md §1. */
#define AP_SSID_PREFIX       "HERO-RC-"

typedef struct {
    uint8_t  mac[6];
    uint32_t ip_addr;   /* 0 until DHCP lease fires */
} wifi_mgr_sta_info_t;

typedef void (*wifi_mgr_station_associated_cb_t)(const uint8_t mac[6]);
typedef void (*wifi_mgr_station_disconnected_cb_t)(const uint8_t mac[6]);
typedef void (*wifi_mgr_station_ip_assigned_cb_t)(const uint8_t mac[6], uint32_t ip);

/** Bring up the SoftAP, configure DHCP, apply radio settings, start the AP.
 *  Caller must have called esp_netif_init() and esp_event_loop_create_default() first. */
void wifi_manager_init(void);

/** Block until WIFI_EVENT_AP_START fires or AP_READY_TIMEOUT_MS elapses.
 *  Call before BLE init so the beacon is on air before radio-intensive BLE work. */
void wifi_manager_wait_for_ap_ready(void);

/** Return the current IP of a station by MAC, or 0 if not found / no DHCP yet. */
uint32_t wifi_manager_get_station_ip(const uint8_t mac[6]);

/** Copy up to max_count active station entries into out[]. Returns count written. */
int wifi_manager_get_connected_stations(wifi_mgr_sta_info_t *out, int max_count);

/** Register station event callbacks. Call once at system init before wifi_manager_init(). */
void wifi_manager_set_callbacks(wifi_mgr_station_associated_cb_t   on_associated,
                                 wifi_mgr_station_disconnected_cb_t  on_disconnected,
                                 wifi_mgr_station_ip_assigned_cb_t   on_ip_assigned);
