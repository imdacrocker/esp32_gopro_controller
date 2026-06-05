/*
 * usb_host_net.c — USB-host network transport for the wired GoPro link.
 *
 * See usb_host_net.h.  Lifted from the wired proof-of-concept (git aa4e7d7)
 * and reshaped into a component with a wifi_manager-style link callback.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lwip/ip4_addr.h"

#include "usbh_core.h"

#include "usb_host_net.h"

static const char *TAG = "usb_host_net";

/* GoPro USB-network convention: the camera runs a DHCP server and reserves
 * the .51 host in the same /24 it leases to us.  We derive the camera IP from
 * our assigned address by replacing the last octet (docs/wired-variant-followup.md). */
#define GOPRO_LAST_OCTET 51

static usb_host_net_link_cb_t s_on_link;
static volatile uint32_t      s_camera_ip;   /* network byte order; 0 when down */
static volatile bool          s_up;

/* ---- Diagnostics --------------------------------------------------------- */

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
    ESP_LOGI(TAG, "  class/sub/proto = 0x%02x / 0x%02x / 0x%02x",
             dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol);
    ESP_LOGI(TAG, "  Product         = %s", hport->iProduct ? hport->iProduct : "(none)");
    ESP_LOGI(TAG, "  bNumInterfaces  = %u", hport->config.config_desc.bNumInterfaces);
}

/* ---- Link state transitions ---------------------------------------------- */

static void set_link_down(void)
{
    if (!s_up && s_camera_ip == 0) {
        return;   /* already down */
    }
    s_up        = false;
    s_camera_ip = 0;
    ESP_LOGI(TAG, "camera link DOWN");
    if (s_on_link) {
        s_on_link(false, 0);
    }
}

/* ---- USB host event handler ---------------------------------------------- */

static void on_usb_event(uint8_t busid, uint8_t hub_index, uint8_t hub_port,
                         uint8_t intf, uint8_t event)
{
    ESP_LOGI(TAG, "usb event=%s bus=%u hub=%u port=%u intf=%u",
             event_name(event), busid, hub_index, hub_port, intf);

    switch (event) {
    case USBH_EVENT_DEVICE_CONFIGURED:
        dump_hubport(busid, hub_index, hub_port);
        /* The CDC-NCM/RNDIS class driver brings up the esp_netif + DHCP
         * client from here; link "up" is signalled later on IP_EVENT_ETH_GOT_IP. */
        break;
    case USBH_EVENT_DEVICE_DISCONNECTED:
        set_link_down();
        break;
    default:
        break;
    }
}

/* ---- IP event handler ---------------------------------------------------- */

static void on_got_ip(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg; (void)base; (void)event_id;
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip = &evt->ip_info;

    /* Derive the camera IP: same /24, last octet = 51. */
    uint32_t self_be  = ip->ip.addr;            /* already network byte order */
    uint32_t mask_be  = htonl(0xFFFFFF00);
    uint32_t gopro_be = (self_be & mask_be) | htonl(GOPRO_LAST_OCTET);

    ip4_addr_t gopro_addr = { .addr = gopro_be };
    ESP_LOGI(TAG, "USB netif %s got IP " IPSTR " — camera at " IPSTR,
             esp_netif_get_ifkey(evt->esp_netif), IP2STR(&ip->ip), IP2STR(&gopro_addr));

    s_camera_ip = gopro_be;
    s_up        = true;
    if (s_on_link) {
        s_on_link(true, gopro_be);
    }
}

/* ---- Public API ---------------------------------------------------------- */

void usb_host_net_init(usb_host_net_link_cb_t on_link)
{
    s_on_link   = on_link;
    s_camera_ip = 0;
    s_up        = false;

    /* CherryUSB's CDC-NCM/RNDIS host class creates an "eth"-type esp_netif, so
     * the camera's DHCP lease surfaces as IP_EVENT_ETH_GOT_IP. */
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               on_got_ip, NULL));

    usbh_initialize(0, ESP_USBH_BASE, on_usb_event);
    ESP_LOGI(TAG, "USB host initialized — waiting for camera");
}

bool usb_host_net_is_up(void)
{
    return s_up;
}

uint32_t usb_host_net_camera_ip(void)
{
    return s_camera_ip;
}
