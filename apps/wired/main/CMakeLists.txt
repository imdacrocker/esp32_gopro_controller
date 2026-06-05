idf_component_register(SRCS "main.c"
                       REQUIRES nvs_flash esp_netif esp_event
                                cam_core wifi_manager can_manager
                                http_server_wired log_ring shutdown_manager
                                app_update)
