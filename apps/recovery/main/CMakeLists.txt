idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS ""
    EMBED_TXTFILES "recovery.html"
    REQUIRES
        nvs_flash
        esp_netif
        esp_event
        wifi_manager
        local_dns
        recovery_http
)
