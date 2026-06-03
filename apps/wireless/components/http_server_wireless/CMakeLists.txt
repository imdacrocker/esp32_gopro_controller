idf_component_register(
    SRCS
        "driver_wireless.c"
        "api_cameras.c"
        "api_rc.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        # gopro_model.h still lives at apps/wireless/components/gopro/ for
        # now — Phase 3 promotes it into a shared component when cam_core
        # lands.
        "${CMAKE_CURRENT_SOURCE_DIR}/../gopro"
    REQUIRES
        # http_server_core is public — http_server_wireless_init returns
        # nothing, but its implementation forwards to http_server_core_start
        # and pulls in http_server_helpers.h.
        http_server_core
    PRIV_REQUIRES
        camera_manager_wireless
        open_gopro_ble
        gopro_wifi_rc
        wifi_manager
        esp_http_server
        espressif__cjson
        nvs_flash
        freertos
)
