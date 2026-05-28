/*
 * captive_dns.h — Tiny wildcard DNS responder for the SoftAP captive portal.
 *
 * The device is an isolated SoftAP with no upstream internet. We hand out
 * our own IP (10.71.79.1) as the DHCP DNS server (see wifi_manager.c), then
 * this responder answers *every* A-record query with that same IP. The net
 * effect: any hostname a connected client looks up — the friendly
 * "control.gp", the OS captive-portal probe domains, anything — resolves to
 * the controller's web UI.
 *
 * This is what makes a memorable address work identically on iPhone and
 * Android: it is plain unicast DNS, not the multicast mDNS that ".local"
 * relies on (and that Android does not resolve from the address bar).
 */
#pragma once

/*
 * Start the DNS responder task. Must be called after the SoftAP netif is up
 * (i.e. after wifi_manager_wait_for_ap_ready()) so the AP IP can be read.
 * Idempotent — a second call is a no-op.
 */
void captive_dns_start(void);
