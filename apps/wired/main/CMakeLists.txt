
idf_component_register(SRCS "main.c"
                       REQUIRES "esp_netif" "esp_event" "esp_http_client" "lwip"
                                "cherry-embedded__cherryusb")
