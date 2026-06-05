idf_component_register(
    SRCS
        "driver_wired.c"
        "api_camera_wired.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # http_server_core is public — http_server_wired_init forwards to
        # http_server_core_start and pulls in its header.
        http_server_core
    PRIV_REQUIRES
        cam_core
        gopro_usb
        usb_host_net
        esp_http_server
        espressif__cjson
)
