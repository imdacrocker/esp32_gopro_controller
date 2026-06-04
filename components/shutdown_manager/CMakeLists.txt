idf_component_register(
    SRCS
        "shutdown_manager.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cam_core
        esp_timer
        freertos
    # open_gopro_ble is wireless-specific; shutdown_manager still calls
    # open_gopro_ble_terminate_slot() to drop the BLE link cleanly during
    # shutdown.  A future cleanup will route this through a new
    # `terminate_link` vtable entry on camera_driver_t so shutdown_manager
    # depends only on cam_core; tracked in docs/multi-variant-restructure-plan.md.
    PRIV_REQUIRES
        open_gopro_ble
)
