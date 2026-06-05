idf_component_register(
    SRCS
        "usb_host_net.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # esp_netif types (ip_event_got_ip_t, IP2STR) appear in the public
        # callback contract's documentation but not the header; kept private.
        esp_netif
    PRIV_REQUIRES
        esp_event
        lwip
        cherry-embedded__cherryusb
)
