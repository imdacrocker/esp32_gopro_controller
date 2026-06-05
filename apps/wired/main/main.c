#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "lwip/ip4_addr.h"

#include "usbh_core.h"

static const char *TAG = "main";

// GoPro USB DHCP convention: camera reserves .51, client gets .52 (or similar).
// We derive the camera IP from our assigned IP by replacing the last octet with 51.
#define GOPRO_LAST_OCTET 51
#define GOPRO_HTTP_PORT  8080

static const char *event_name(uint8_t event)
{
    switch (event) {
    case USBH_EVENT_DEVICE_RESET:          return "DEVICE_RESET";
    case USBH_EVENT_DEVICE_CONNECTED:      return "DEVICE_CONNECTED";
    case USBH_EVENT_DEVICE_DISCONNECTED:   return "DEVICE_DISCONNECTED";
    case USBH_EVENT_DEVICE_CONFIGURED:     return "DEVICE_CONFIGURED";
    case USBH_EVENT_INTERFACE_UNSUPPORTED: return "INTERFACE_UNSUPPORTED";
    case USBH_EVENT_INTERFACE_START:       return "INTERFACE_START";
    case USBH_EVENT_INTERFACE_STOP:        return "INTERFACE_STOP";
    case USBH_EVENT_INIT:                  return "INIT";
    case USBH_EVENT_DEINIT:                return "DEINIT";
    case USBH_EVENT_ERROR:                 return "ERROR";
    default:                               return "?";
    }
}

static void dump_hubport(uint8_t busid, uint8_t hub_index, uint8_t hub_port)
{
    struct usbh_hubport *hport = usbh_find_hubport(busid, hub_index, hub_port);
    if (!hport || !hport->connected) {
        ESP_LOGW(TAG, "hubport %u/%u not connected", hub_index, hub_port);
        return;
    }

    const struct usb_device_descriptor *dd = &hport->device_desc;
    ESP_LOGI(TAG, "--- Device on bus %u hub %u port %u (addr %u, speed %u) ---",
             busid, hub_index, hub_port, hport->dev_addr, hport->speed);
    ESP_LOGI(TAG, "  VID:PID         = 0x%04x:0x%04x", dd->idVendor, dd->idProduct);
    ESP_LOGI(TAG, "  bcdDevice       = 0x%04x", dd->bcdDevice);
    ESP_LOGI(TAG, "  bcdUSB          = 0x%04x", dd->bcdUSB);
    ESP_LOGI(TAG, "  class/sub/proto = 0x%02x / 0x%02x / 0x%02x",
             dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol);
    ESP_LOGI(TAG, "  Manufacturer    = %s", hport->iManufacturer ? hport->iManufacturer : "(none)");
    ESP_LOGI(TAG, "  Product         = %s", hport->iProduct ? hport->iProduct : "(none)");
    ESP_LOGI(TAG, "  SerialNumber    = %s", hport->iSerialNumber ? hport->iSerialNumber : "(none)");
    ESP_LOGI(TAG, "  bNumInterfaces  = %u", hport->config.config_desc.bNumInterfaces);

    for (uint8_t i = 0; i < hport->config.config_desc.bNumInterfaces; i++) {
        const struct usbh_interface *intf = &hport->config.intf[i];
        for (uint8_t a = 0; a < intf->altsetting_num; a++) {
            const struct usb_interface_descriptor *id = &intf->altsetting[a].intf_desc;
            ESP_LOGI(TAG, "  intf %u alt %u: class/sub/proto = 0x%02x / 0x%02x / 0x%02x, %u endpoints",
                     id->bInterfaceNumber, id->bAlternateSetting,
                     id->bInterfaceClass, id->bInterfaceSubClass, id->bInterfaceProtocol,
                     id->bNumEndpoints);
        }
    }
}

static void on_usb_event(uint8_t busid, uint8_t hub_index, uint8_t hub_port,
                         uint8_t intf, uint8_t event)
{
    ESP_LOGI(TAG, "event=%s bus=%u hub=%u port=%u intf=%u",
             event_name(event), busid, hub_index, hub_port, intf);

    if (event == USBH_EVENT_DEVICE_CONFIGURED) {
        dump_hubport(busid, hub_index, hub_port);
    }
}

// --- HTTP client ----------------------------------------------------------

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGI(TAG, "  header: %s: %s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        // Print body chunks as they arrive. Cap each line to avoid flooding.
        if (evt->data_len > 0) {
            int n = evt->data_len > 256 ? 256 : evt->data_len;
            ESP_LOGI(TAG, "  body (%d bytes): %.*s%s",
                     evt->data_len, n, (const char *)evt->data,
                     evt->data_len > n ? "..." : "");
        }
        break;
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "  HTTP_EVENT_ERROR");
        break;
    default:
        break;
    }
    return ESP_OK;
}

typedef struct {
    char gopro_ip[16];  // dotted-quad string, e.g. "172.28.142.51"
} gopro_query_args_t;

// Fire a single GET against the GoPro. Returns the HTTP status code on success,
// or -1 if the request itself failed (connect, timeout, etc.).
static int gopro_get(const char *gopro_ip, const char *path)
{
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             gopro_ip, GOPRO_HTTP_PORT, path);

    ESP_LOGI(TAG, "--- Firing GET %s ---", url);

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_cb,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return -1;
    }

    int result = -1;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int len    = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "--- HTTP done: status=%d, content_length=%d ---", status, len);
        result = status;
    } else {
        ESP_LOGE(TAG, "esp_http_client_perform failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return result;
}

static void gopro_query_task(void *pv)
{
    gopro_query_args_t *args = (gopro_query_args_t *)pv;

    // === HERO 9 video record sequence (modeled on gopro-py-api shoot_video) ===
    // The HERO 9 over USB needs the camera taken OUT of the "USB Connected"
    // passive screen (status 89 = 12) and INTO a real shooting mode before it
    // will accept shutter commands. The python lib does this via mode() before
    // shutter(). New /gopro/... endpoints return 404 on this firmware so we
    // use the legacy /gp/gpControl/... API throughout.

    // Watch for these field IDs in the state JSON across the probes:
    //   "8"  = system busy (1 = busy)
    //   "10" = encoding active (1 = recording)
    //   "89" = active app (12 = USB Connected; we want this to change)
    //   "31" = ready / similar
    // Comparing the values across probes tells us exactly when the camera
    // moves out of USB Connected, when it's busy, and when it's accepting commands.

    ESP_LOGI(TAG, "===== STEP 1: state probe BEFORE =====");
    if (gopro_get(args->gopro_ip, "/gopro/camera/state") != 200) {
        ESP_LOGE(TAG, "State probe failed. Aborting.");
        goto done;
    }

    ESP_LOGI(TAG, "===== STEP 2: exit webcam mode =====");
    vTaskDelay(pdMS_TO_TICKS(500));
    gopro_get(args->gopro_ip, "/gp/gpWebcam/STOP");

    ESP_LOGI(TAG, "===== STEP 3: settle 3 s, probe state AFTER STOP =====");
    vTaskDelay(pdMS_TO_TICKS(3000));
    gopro_get(args->gopro_ip, "/gopro/camera/state");

    ESP_LOGI(TAG, "===== STEP 4: set Video sub_mode =====");
    vTaskDelay(pdMS_TO_TICKS(500));
    gopro_get(args->gopro_ip, "/gp/gpControl/command/sub_mode?mode=0&sub_mode=0");

    ESP_LOGI(TAG, "===== STEP 5: settle 3 s, probe state AFTER mode change =====");
    vTaskDelay(pdMS_TO_TICKS(3000));
    gopro_get(args->gopro_ip, "/gopro/camera/state");

    // Try shutter with retries — maybe the camera just needs a few more
    // seconds to settle.
    ESP_LOGI(TAG, "===== STEP 6: shutter start, up to 3 attempts =====");
    int start_status = -1;
    for (int attempt = 1; attempt <= 3; attempt++) {
        ESP_LOGI(TAG, "Shutter attempt %d/3...", attempt);
        start_status = gopro_get(args->gopro_ip,
                                 "/gp/gpControl/command/shutter?p=1");
        if (start_status == 200) {
            ESP_LOGI(TAG, "Shutter started on attempt %d!", attempt);
            break;
        }
        ESP_LOGW(TAG, "Attempt %d returned %d. Waiting 3 s...", attempt, start_status);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    if (start_status != 200) {
        ESP_LOGE(TAG, "All shutter attempts failed. Final probe to diagnose...");
        gopro_get(args->gopro_ip, "/gopro/camera/state");
        goto done;
    }

    ESP_LOGI(TAG, "Recording 10 s...");
    vTaskDelay(pdMS_TO_TICKS(10000));

    ESP_LOGI(TAG, "===== STEP 7: stop shutter =====");
    gopro_get(args->gopro_ip, "/gp/gpControl/command/shutter?p=0");

    ESP_LOGI(TAG, "===== STEP 8: final state probe =====");
    vTaskDelay(pdMS_TO_TICKS(500));
    gopro_get(args->gopro_ip, "/gopro/camera/state");

    ESP_LOGI(TAG, "Record sequence complete.");

done:
    free(args);
    vTaskDelete(NULL);
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip = &evt->ip_info;

    ESP_LOGI(TAG, "*** IP_EVENT_ETH_GOT_IP on %s ***", esp_netif_get_ifkey(evt->esp_netif));
    ESP_LOGI(TAG, "    self IP : " IPSTR, IP2STR(&ip->ip));
    ESP_LOGI(TAG, "    netmask : " IPSTR, IP2STR(&ip->netmask));
    ESP_LOGI(TAG, "    gateway : " IPSTR, IP2STR(&ip->gw));

    // Derive GoPro IP: same /24, last octet = 51.
    gopro_query_args_t *args = malloc(sizeof(*args));
    if (!args) {
        ESP_LOGE(TAG, "OOM allocating query args");
        return;
    }
    uint32_t self_be = ip->ip.addr;  // already in network byte order
    uint32_t mask_be = htonl(0xFFFFFF00);
    uint32_t gopro_be = (self_be & mask_be) | htonl(GOPRO_LAST_OCTET);
    ip4_addr_t gopro_addr = { .addr = gopro_be };
    snprintf(args->gopro_ip, sizeof(args->gopro_ip), IPSTR, IP2STR(&gopro_addr));
    ESP_LOGI(TAG, "    GoPro IP: %s", args->gopro_ip);

    // Defer HTTP to its own task — don't block the event loop.
    xTaskCreate(gopro_query_task, "gopro_query", 4096, args, 5, NULL);
}

// --------------------------------------------------------------------------

void app_main(void)
{
    ESP_LOGI(TAG, "Booting USB host enumeration sketch");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Fire HTTP request when DHCP gives us an IP on the USB-Ethernet netif.
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               on_got_ip, NULL));

    usbh_initialize(0, ESP_USBH_BASE, on_usb_event);

    ESP_LOGI(TAG, "USB host initialized — plug in the GoPro now");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
