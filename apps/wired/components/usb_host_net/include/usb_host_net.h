/*
 * usb_host_net.h — USB-host network transport for the wired GoPro link.
 *
 * Brings up the CherryUSB host stack and surfaces the camera link as a small
 * callback API modeled on wifi_manager's station callbacks
 * (docs/design/wired-variant.md §2, §5).
 *
 * The heavy lifting — enumerating the camera's USB network gadget
 * (CDC-NCM / RNDIS), creating an esp_netif, running the DHCP client, and
 * pumping frames between USB and lwIP — is done automatically inside
 * CherryUSB's platform/idf/usbh_net.c once CONFIG_CHERRYUSB_HOST_CDC_NCM /
 * _CDC_RNDIS are enabled.  This component owns only the host-bus init, the
 * IP-event → camera-IP derivation, and the attach/detach signalling that the
 * USB GoPro driver (gopro_usb, Phase 3) consumes.
 *
 * Caller must have called esp_netif_init() and esp_event_loop_create_default()
 * before usb_host_net_init().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Link-state callback.
 *
 *   up=true,  camera_ip != 0 — the USB camera has a DHCP lease and its HTTP
 *                              endpoint (camera_ip:8080) is reachable.
 *   up=false, camera_ip == 0 — the camera was unplugged / the link dropped.
 *
 * Fired from the esp_netif event loop / USB host task context — do not block;
 * post work to your own task.  camera_ip is in network byte order (as stored
 * in esp_netif_ip_info_t / lwip ip4_addr_t).
 */
typedef void (*usb_host_net_link_cb_t)(bool up, uint32_t camera_ip);

/*
 * Initialise the USB host bus and register the link callback.  Idempotent is
 * NOT guaranteed — call exactly once at boot.  `on_link` may be NULL (the
 * component still tracks state, queryable via the accessors below).
 */
void usb_host_net_init(usb_host_net_link_cb_t on_link);

/* True while the camera link is up (DHCP lease held). */
bool usb_host_net_is_up(void);

/*
 * Current camera IP (network byte order), or 0 when the link is down.
 * Safe to call from any task.
 */
uint32_t usb_host_net_camera_ip(void);
