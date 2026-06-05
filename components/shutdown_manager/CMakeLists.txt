idf_component_register(
    SRCS
        "shutdown_manager.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cam_core
        esp_timer
        freertos
    # No wireless-only dependencies: BLE-link teardown during shutdown is
    # routed through cam_core's terminate_link vtable entry (the BLE driver
    # implements it), so this shared component depends only on cam_core.
    # See docs/design/wired-variant.md §2.
)
