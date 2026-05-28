/*
 * captive_dns.h — Tiny selective DNS responder for the SoftAP.
 *
 * The device is an isolated SoftAP with no upstream internet. We hand out
 * our own IP (10.71.79.1) as the DHCP DNS server (see wifi_manager.c), then
 * this responder resolves only the friendly local name "control.gp" to that
 * IP and answers NXDOMAIN for every other name.
 *
 * Returning NXDOMAIN for foreign lookups — rather than the old captive-portal
 * behaviour of pointing *every* name at ourselves — is deliberate. A wildcard
 * responder makes the OS captive-portal probe (e.g. captive.apple.com) resolve
 * to us, so iOS/Android flag the network as a captive portal and get sticky
 * about reaching outside networks. By telling the truth (only control.gp
 * exists here), the phone sees an AP with no internet and cleanly uses
 * cellular for everything else, while "control.gp" still loads the web UI.
 *
 * Using plain unicast DNS (not multicast mDNS / ".local") is what makes the
 * memorable address work identically on iPhone and Android — Android does not
 * resolve ".local" from the address bar.
 */
#pragma once

/*
 * Start the DNS responder task. Must be called after the SoftAP netif is up
 * (i.e. after wifi_manager_wait_for_ap_ready()) so the AP IP can be read.
 * Idempotent — a second call is a no-op.
 */
void captive_dns_start(void);
