idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS ""
                       REQUIRES nvs_flash esp_netif esp_event wifi_manager ble_core
                                camera_manager open_gopro_ble
                                gopro_wifi_rc can_manager http_server
                                app_update esp_timer)
