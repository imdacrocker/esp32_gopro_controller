#include "wifi_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include "dhcpserver/dhcpserver.h"

#include <string.h>

static const char *TAG = "wifi_mgr";

/* Event-group bits */
#define AP_STARTED_BIT        BIT0
#define STA_GOT_IP_BIT        BIT1
#define STA_DISCONNECTED_BIT  BIT2

/* Tracks which mode the manager is *trying* to be in.  WIFI_EVENT_AP_STOP
 * only auto-restarts the AP when this is MODE_AP — during a deliberate
 * sta_join the value is MODE_STA and the auto-restart is suppressed. */
typedef enum {
    MODE_AP  = 0,
    MODE_STA = 1,
} target_mode_t;

typedef struct {
    bool     active;
    uint8_t  mac[6];
    uint32_t ip_addr;
} sta_entry_t;

static EventGroupHandle_t                  s_wifi_events;
static esp_netif_t                        *s_ap_netif;
static esp_netif_t                        *s_sta_netif;     /* created on first sta_join */
static wifi_config_t                       s_ap_config;     /* saved at init so we can re-apply */
static target_mode_t                       s_target_mode;

/*
 * LOAD-BEARING SINGLE-FLIGHT ASSUMPTION: s_sta_busy is read+set without a
 * mutex in wifi_manager_sta_join (the check-then-set at lines 309/313) on
 * the premise that exactly one caller can be in flight at a time.  The
 * only caller of wifi_manager_sta_join / wifi_manager_sta_leave is
 * pair_complete.c:pair_complete_task, which itself is single-flight under
 * pair_complete.c's s_busy gate (backed by s_gate_lock + s_pending[] queue
 * since the pair_complete-queue commit).
 *
 * If a non-pair_complete caller is ever added — e.g. a UI-initiated WiFi
 * scan/connect, or a CLI command that joins a STA — the check-then-set
 * MUST become atomic.  The minimal change is atomic_compare_exchange_strong
 * on an atomic_bool, plus matching atomic_store on the false-writes at
 * lines 333/341/359/367/375/413/427.  Same pattern as
 * apps/wireless/components/gopro/open_gopro_ble/pair_complete.c:s_busy and
 * apps/wireless/components/gopro/open_gopro_ble/status.c:s_band_busy.
 */
static bool                                s_sta_busy;
static uint32_t                            s_sta_gw_ip;     /* last STA gateway IP */
static sta_entry_t                         s_stations[AP_MAX_CONN];

static wifi_mgr_station_associated_cb_t    s_on_associated;
static wifi_mgr_station_disconnected_cb_t  s_on_disconnected;
static wifi_mgr_station_ip_assigned_cb_t   s_on_ip_assigned;

/* ---- station table (lock-free; minor races acceptable per spec §11.4) ---- */

static sta_entry_t *find_station(const uint8_t mac[6])
{
    for (int i = 0; i < AP_MAX_CONN; i++) {
        if (s_stations[i].active && memcmp(s_stations[i].mac, mac, 6) == 0) {
            return &s_stations[i];
        }
    }
    return NULL;
}

static sta_entry_t *alloc_station_slot(void)
{
    for (int i = 0; i < AP_MAX_CONN; i++) {
        if (!s_stations[i].active) return &s_stations[i];
    }
    return NULL;
}

/* ---- event handler ------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            /* HT20 prevents 40 MHz from interfering with BLE (§11.2).
               WIFI_PS_NONE and inactive_time are re-applied here so they survive AP restarts. */
            esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
            esp_wifi_set_ps(WIFI_PS_NONE);
            esp_wifi_set_inactive_time(WIFI_IF_AP, 60);
            /* Drop any stale station entries. esp_wifi_stop() during an AP/STA
             * bounce (wifi_manager_sta_join) does not emit AP_STADISCONNECTED
             * for connected cameras, so without this their entries would
             * linger across the bounce and orphan the table. Cameras
             * re-announce themselves via AP_STACONNECTED after restart.
             * Safe here: the event loop is single-threaded, so this runs
             * before any STACONNECTED for the new AP session. */
            memset(s_stations, 0, sizeof(s_stations));
            xEventGroupSetBits(s_wifi_events, AP_STARTED_BIT);
            ESP_LOGI(TAG, "AP started, ch=%d", AP_CHANNEL);
            break;

        case WIFI_EVENT_AP_STOP:
            xEventGroupClearBits(s_wifi_events, AP_STARTED_BIT);
            if (s_target_mode == MODE_AP) {
                ESP_LOGW(TAG, "AP stopped unexpectedly — restarting");
                esp_wifi_start();
            } else {
                ESP_LOGI(TAG, "AP stopped (deliberate, switching to STA)");
            }
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = event_data;
            /* Reuse an existing same-MAC entry if present, else allocate a
             * fresh slot. A re-association that wasn't preceded by an
             * AP_STADISCONNECTED (e.g. a fast reconnect) would otherwise create
             * a duplicate entry and orphan the table. */
            sta_entry_t *slot = find_station(ev->mac);
            if (!slot) slot = alloc_station_slot();
            if (slot) {
                slot->active  = true;
                memcpy(slot->mac, ev->mac, 6);
                slot->ip_addr = 0;
            } else {
                ESP_LOGW(TAG, "station table full — " MACSTR " not tracked", MAC2STR(ev->mac));
            }
            if (s_on_associated) s_on_associated(ev->mac);
            ESP_LOGI(TAG, "station " MACSTR " associated", MAC2STR(ev->mac));
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = event_data;
            sta_entry_t *slot = find_station(ev->mac);
            if (slot) {
                slot->active  = false;
                slot->ip_addr = 0;
            }
            if (s_on_disconnected) s_on_disconnected(ev->mac);
            ESP_LOGI(TAG, "station " MACSTR " disconnected (reason=%d)",
                     MAC2STR(ev->mac), ev->reason);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED:
            /* Only flagged for the in-flight sta_join; passive disconnects
             * (e.g. signal loss before sta_leave fires) are harmless. */
            xEventGroupSetBits(s_wifi_events, STA_DISCONNECTED_BIT);
            ESP_LOGI(TAG, "STA disconnected");
            break;

        default:
            break;
        }

    } else if (base == IP_EVENT) {
        if (event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
            ip_event_assigned_ip_to_client_t *ev = event_data;
            sta_entry_t *slot = find_station(ev->mac);
            if (slot) slot->ip_addr = ev->ip.addr;
            if (s_on_ip_assigned) s_on_ip_assigned(ev->mac, ev->ip.addr);
            ESP_LOGI(TAG, "station " MACSTR " assigned " IPSTR,
                     MAC2STR(ev->mac), IP2STR(&ev->ip));
        } else if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *ev = event_data;
            s_sta_gw_ip = ev->ip_info.gw.addr;
            xEventGroupSetBits(s_wifi_events, STA_GOT_IP_BIT);
            ESP_LOGI(TAG, "STA got IP " IPSTR " gw " IPSTR,
                     IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
        }
    }
}

/* ---- public API ---------------------------------------------------------- */

void wifi_manager_set_callbacks(wifi_mgr_station_associated_cb_t   on_associated,
                                 wifi_mgr_station_disconnected_cb_t  on_disconnected,
                                 wifi_mgr_station_ip_assigned_cb_t   on_ip_assigned)
{
    s_on_associated   = on_associated;
    s_on_disconnected = on_disconnected;
    s_on_ip_assigned  = on_ip_assigned;
}

void wifi_manager_init(void)
{
    s_wifi_events = xEventGroupCreate();
    s_ap_netif    = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    /* MAC spoof: OUI d8:96:85 so Hero4 cameras recognise the AP as a WiFi Remote (§11.3) */
    uint8_t factory_mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, factory_mac));
    uint8_t ap_mac[6] = { 0xd8, 0x96, 0x85,
                           factory_mac[3], factory_mac[4], factory_mac[5] };
    esp_err_t mac_err = esp_wifi_set_mac(WIFI_IF_AP, ap_mac);
    if (mac_err != ESP_OK) {
        /* IDF may reject 0xD8 because bit-1 (locally-administered) is not set (§11.3). */
        ESP_LOGW(TAG, "MAC spoof rejected (0x%x) — Hero4 RC-emulation will not auto-connect",
                 mac_err);
        memcpy(ap_mac, factory_mac, 6);
    }

    /* DHCP: fixed IP 10.71.79.1/24, pool 10.71.79.2–50 (§11.2) */
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));

    /* IP4_ADDR + cast aliases esp_ip4_addr_t as lwIP's ip4_addr_t. The two
     * are layout-compatible (single uint32_t addr field), but -Os promotes
     * the strict-aliasing warning to a hard error. Build the value via a
     * local lwIP type and copy across. */
    esp_netif_ip_info_t ip_info = {};
    ip4_addr_t tmp;
    IP4_ADDR(&tmp, 10, 71, 79,   1); ip_info.ip.addr      = tmp.addr;
    IP4_ADDR(&tmp, 10, 71, 79,   1); ip_info.gw.addr      = tmp.addr;
    IP4_ADDR(&tmp, 255, 255, 255, 0); ip_info.netmask.addr = tmp.addr;
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));

    dhcps_lease_t lease = { .enable = true };
    IP4_ADDR(&lease.start_ip, 10, 71, 79,  2);
    IP4_ADDR(&lease.end_ip,   10, 71, 79, 50);
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                            ESP_NETIF_REQUESTED_IP_ADDRESS,
                                            &lease, sizeof(lease)));

    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));

    /* SoftAP: open auth, ch 11, 60 s inactive timeout, PMF disabled (§11.2) */
    wifi_config_t ap_config = {};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid),
             AP_SSID_PREFIX "%02X%02X%02X", ap_mac[3], ap_mac[4], ap_mac[5]);
    ap_config.ap.ssid_len         = (uint8_t)strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel          = AP_CHANNEL;
    ap_config.ap.authmode         = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection   = AP_MAX_CONN;
    ap_config.ap.pmf_cfg.required = false;

    s_ap_config   = ap_config;
    s_target_mode = MODE_AP;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "init complete: SSID=\"%s\" IP=10.71.79.1 ch=%d",
             (char *)ap_config.ap.ssid, AP_CHANNEL);
}

void wifi_manager_wait_for_ap_ready(void)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, AP_STARTED_BIT,
                                            pdFALSE, pdTRUE,
                                            pdMS_TO_TICKS(AP_READY_TIMEOUT_MS));
    if (!(bits & AP_STARTED_BIT)) {
        ESP_LOGW(TAG, "AP not ready after %d ms", AP_READY_TIMEOUT_MS);
    }
}

uint32_t wifi_manager_get_station_ip(const uint8_t mac[6])
{
    sta_entry_t *slot = find_station(mac);
    return slot ? slot->ip_addr : 0;
}

int wifi_manager_get_connected_stations(wifi_mgr_sta_info_t *out, int max_count)
{
    int n = 0;
    for (int i = 0; i < AP_MAX_CONN && n < max_count; i++) {
        if (s_stations[i].active) {
            memcpy(out[n].mac, s_stations[i].mac, 6);
            out[n].ip_addr = s_stations[i].ip_addr;
            n++;
        }
    }
    return n;
}

/* ---- AP/STA mode switch ------------------------------------------------- */

static esp_err_t bring_ap_back_up(void)
{
    s_target_mode = MODE_AP;
    xEventGroupClearBits(s_wifi_events, AP_STARTED_BIT);

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "bring_ap: esp_wifi_stop rc=0x%x", err);
    }

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bring_ap: set_mode(AP) rc=0x%x", err);
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bring_ap: set_config rc=0x%x", err);
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bring_ap: esp_wifi_start rc=0x%x", err);
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, AP_STARTED_BIT,
                                            pdFALSE, pdTRUE,
                                            pdMS_TO_TICKS(AP_READY_TIMEOUT_MS));
    if (!(bits & AP_STARTED_BIT)) {
        ESP_LOGW(TAG, "bring_ap: AP did not start within %d ms", AP_READY_TIMEOUT_MS);
    }
    return ESP_OK;
}

wifi_mgr_err_t wifi_manager_sta_join(const char *ssid,
                                      const char *password,
                                      uint32_t   *gw_out,
                                      uint32_t    timeout_ms)
{
    if (!ssid) return WIFI_MGR_ERR_INTERNAL;
    if (s_sta_busy) {
        ESP_LOGW(TAG, "sta_join: already busy");
        return WIFI_MGR_ERR_BUSY;
    }
    s_sta_busy = true;

    ESP_LOGI(TAG, "sta_join: target SSID=\"%s\", timeout=%lums",
             ssid, (unsigned long)timeout_ms);

    /* Lazily create the STA netif on first use. */
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    /* Pause the AP — set target first so WIFI_EVENT_AP_STOP doesn't auto-restart. */
    s_target_mode = MODE_STA;
    xEventGroupClearBits(s_wifi_events,
                          AP_STARTED_BIT | STA_GOT_IP_BIT | STA_DISCONNECTED_BIT);
    s_sta_gw_ip = 0;

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "sta_join: esp_wifi_stop rc=0x%x", err);
        bring_ap_back_up();
        s_sta_busy = false;
        return WIFI_MGR_ERR_INTERNAL;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sta_join: set_mode(STA) rc=0x%x", err);
        bring_ap_back_up();
        s_sta_busy = false;
        return WIFI_MGR_ERR_INTERNAL;
    }

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    if (password) {
        strlcpy((char *)sta_cfg.sta.password, password,
                sizeof(sta_cfg.sta.password));
    }
    sta_cfg.sta.threshold.authmode = (password && password[0])
                                      ? WIFI_AUTH_WPA2_PSK
                                      : WIFI_AUTH_OPEN;

    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sta_join: set_config rc=0x%x", err);
        bring_ap_back_up();
        s_sta_busy = false;
        return WIFI_MGR_ERR_INTERNAL;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sta_join: esp_wifi_start rc=0x%x", err);
        bring_ap_back_up();
        s_sta_busy = false;
        return WIFI_MGR_ERR_INTERNAL;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sta_join: esp_wifi_connect rc=0x%x", err);
        bring_ap_back_up();
        s_sta_busy = false;
        return WIFI_MGR_ERR_ASSOC_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        STA_GOT_IP_BIT | STA_DISCONNECTED_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & STA_GOT_IP_BIT) {
        if (gw_out) *gw_out = s_sta_gw_ip;
        esp_ip4_addr_t gw_print = { .addr = s_sta_gw_ip };

        /* Best-effort: pull the actual channel + RSSI from the driver so the
         * caller can confirm which band the camera ended up on (2.4 GHz =
         * channels 1-14; 5 GHz = 36+). */
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "sta_join: success, gw=" IPSTR
                          " ch=%u rssi=%d band=%s",
                     IP2STR(&gw_print), (unsigned)ap.primary, (int)ap.rssi,
                     ap.primary >= 36 ? "5 GHz" : "2.4 GHz");
        } else {
            ESP_LOGI(TAG, "sta_join: success, gw=" IPSTR " (ap_info unavailable)",
                     IP2STR(&gw_print));
        }
        return WIFI_MGR_OK;  /* leave radio in STA mode for caller's HTTP work */
    }

    /* Failure path: either DISCONNECTED fired, or we timed out. */
    if (bits & STA_DISCONNECTED_BIT) {
        ESP_LOGW(TAG, "sta_join: assoc failed");
    } else {
        ESP_LOGW(TAG, "sta_join: DHCP timeout after %lums",
                 (unsigned long)timeout_ms);
    }
    bring_ap_back_up();
    s_sta_busy = false;
    return (bits & STA_DISCONNECTED_BIT) ? WIFI_MGR_ERR_ASSOC_FAIL
                                          : WIFI_MGR_ERR_DHCP_TIMEOUT;
}

void wifi_manager_sta_leave(void)
{
    if (!s_sta_busy) {
        return;
    }
    ESP_LOGI(TAG, "sta_leave: returning to AP mode");

    esp_wifi_disconnect();  /* best-effort */
    bring_ap_back_up();
    s_sta_busy  = false;
    s_sta_gw_ip = 0;
}
