idf_component_register(
    SRCS
        "gopro_usb.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # camera_driver_t / camera_types appear in the public header.
        cam_core
    PRIV_REQUIRES
        can_manager
        esp_http_client
        espressif__cjson
        esp_timer
        freertos
)
