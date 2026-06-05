idf_component_register(
    SRCS
        "driver_wireless.c"
        "api_cameras.c"
        "api_rc.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # http_server_core is public — http_server_wireless_init returns
        # nothing, but its implementation forwards to http_server_core_start
        # and pulls in http_server_helpers.h.
        http_server_core
    PRIV_REQUIRES
        gopro_model
        camera_manager_wireless
        open_gopro_ble
        gopro_wifi_rc
        wifi_manager
        esp_http_server
        espressif__cjson
        nvs_flash
        freertos
)
