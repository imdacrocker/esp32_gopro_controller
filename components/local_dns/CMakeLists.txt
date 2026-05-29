idf_component_register(SRCS "local_dns.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif lwip freertos)
