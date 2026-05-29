# wifi_manager

Manages an ESP32 SoftAP that presents the device as a GoPro WiFi Remote to up to six Wi-Fi stations. Handles DHCP, station lifecycle callbacks, and radio settings required for BLE coexistence.

---

## Responsibilities

- Create and start the SoftAP with a fixed SSID (`HERO-RC-XXXXXX`), a configurable IP (`CONFIG_WIFI_AP_IP_ADDR`, default `10.71.79.1`), and a DHCP pool derived from its /24 subnet (`.2–.50`).
- Spoof the WiFi MAC OUI to `d8:96:85` (GoPro WiFi Remote) so GoPro cameras recognise the device as a known remote type.
- Track connected stations and notify callers via callbacks when a station associates, disconnects, or receives a DHCP lease.
- Apply HT20 bandwidth and disable power save on AP start to minimise BLE interference.

---

## Dependencies

```
REQUIRES: esp_wifi, esp_netif, esp_event, freertos, lwip
```

**Preconditions before calling `wifi_manager_init()`:**

```c
esp_netif_init();
esp_event_loop_create_default();
```

---

## Public API

Header: `include/wifi_manager.h`

### Constants

```c
#define AP_CHANNEL          11      // 2462 MHz — clear of BLE adv channels 37/38/39
#define AP_MAX_CONN          6      // 4 cameras + 1 setup device + 1 spare
#define AP_READY_TIMEOUT_MS  5000
```

### Data Types

```c
typedef struct {
    uint8_t  mac[6];
    uint32_t ip_addr;   // 0 until DHCP lease fires
} wifi_mgr_sta_info_t;
```

### Callbacks

All callbacks fire on the WiFi system task. They must not block.

```c
// Station associated (pre-DHCP; ip_addr not yet valid)
typedef void (*wifi_mgr_station_associated_cb_t)(const uint8_t mac[6]);

// Station disconnected
typedef void (*wifi_mgr_station_disconnected_cb_t)(const uint8_t mac[6]);

// DHCP lease assigned to station
typedef void (*wifi_mgr_station_ip_assigned_cb_t)(const uint8_t mac[6], uint32_t ip);
```

### Functions

---

#### `wifi_manager_set_callbacks`

```c
void wifi_manager_set_callbacks(
    wifi_mgr_station_associated_cb_t    on_associated,
    wifi_mgr_station_disconnected_cb_t  on_disconnected,
    wifi_mgr_station_ip_assigned_cb_t   on_ip_assigned
);
```

Register station event callbacks. All parameters are optional (pass `NULL` to skip). Call once **before** `wifi_manager_init()`.

---

#### `wifi_manager_init`

```c
void wifi_manager_init(void);
```

Bring up the SoftAP. Configures the network interface, registers event handlers, sets AP mode, applies MAC spoof, configures DHCP, sets SoftAP parameters, and calls `esp_wifi_start()`.

Radio settings (HT20 bandwidth, power save off, 60 s inactive timeout) are applied asynchronously on `WIFI_EVENT_AP_START`.

Panics via `ESP_ERROR_CHECK` on any critical failure (driver init, event registration, config apply). Non-critical failures (MAC spoof rejection) log a warning and continue.

---

#### `wifi_manager_wait_for_ap_ready`

```c
void wifi_manager_wait_for_ap_ready(void);
```

Block the calling task until `WIFI_EVENT_AP_START` fires, or until `AP_READY_TIMEOUT_MS` (5 s) elapses. Call after `wifi_manager_init()` to ensure the beacon is on-air before dependent code runs.

---

#### `wifi_manager_get_station_ip`

```c
uint32_t wifi_manager_get_station_ip(const uint8_t mac[6]);
```

Return the IPv4 address (host byte order) assigned to the station identified by `mac`. Returns `0` if the station is not in the table or has not yet received a DHCP lease.

---

#### `wifi_manager_get_connected_stations`

```c
int wifi_manager_get_connected_stations(wifi_mgr_sta_info_t *out, int max_count);
```

Copy up to `max_count` active station records into `out`. Returns the number of entries written. Stations with `ip_addr == 0` are included (associated but no lease yet).

---

## Initialization Order

```c
// 1. System prerequisites
esp_netif_init();
esp_event_loop_create_default();

// 2. Register callbacks before init (so no events are missed)
wifi_manager_set_callbacks(on_assoc, on_disc, on_ip);

// 3. Bring up the AP
wifi_manager_init();

// 4. Block until beacon is on-air
wifi_manager_wait_for_ap_ready();
```

---

## Network Settings

| Item | Value |
|------|-------|
| AP IP | `CONFIG_WIFI_AP_IP_ADDR` (default `10.71.79.1`) |
| Netmask | `255.255.255.0` (fixed /24) |
| DHCP pool | `.2` – `.50` of the AP subnet |
| Local domain | `CONFIG_WIFI_LOCAL_DOMAIN` (default `control.gp`) |
| SSID | `HERO-RC-XXXXXX` (last 3 bytes of factory MAC) |
| Auth | Open (no password) |
| Channel | 11 (2462 MHz) |
| Max stations | 6 |
| MAC OUI | `d8:96:85` (GoPro WiFi Remote) |

The MAC OUI is spoofed at init. If the IDF rejects the spoofed address (e.g., due to hardware restrictions), a warning is logged and the factory MAC is used — the SSID suffix will still reflect the factory MAC bytes.

---

## Radio Settings (Applied on AP Start)

| Setting | Value | Reason |
|---------|-------|--------|
| Bandwidth | HT20 (20 MHz) | Prevents upper half of 40 MHz channel overlapping BLE |
| Power save | `WIFI_PS_NONE` | Keeps radio active during BLE scan windows |
| Inactive timeout | 60 s | Cleans up stale associations |

---

## Error Handling

| Scenario | Behaviour |
|----------|-----------|
| Driver/config failure | `ESP_ERROR_CHECK` — firmware halts |
| MAC spoof rejected by IDF | Log warning; continue with factory MAC |
| `WIFI_EVENT_AP_STOP` unexpectedly | Log warning; call `esp_wifi_start()` to restart |
| Station table full at association | Log warning; station not tracked but `on_associated` callback still fires |
