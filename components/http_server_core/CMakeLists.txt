idf_component_register(
    SRCS
        "driver.c"
        "api_system.c"
        "api_settings.c"
        "api_ota.c"
        "api_logs.c"
        "api_shutdown.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public dependencies — anything that appears in include/*.h must
        # be discoverable to consumers of this component (notably
        # http_server_helpers.h's shutdown_manager.h include).
        esp_http_server
        shutdown_manager
    PRIV_REQUIRES
        # Private dependencies needed only inside this component's .c files.
        # camera_manager is here for api_shutdown's slot-count read — Phase 3
        # redirects this to cam_core once it exists.
        camera_manager
        can_manager
        wifi_manager
        ota_io
        log_ring
        esp_app_format
        app_update
        esp_partition
        joltwallet__littlefs
        espressif__cjson
        lwip
        nvs_flash
        freertos
)
