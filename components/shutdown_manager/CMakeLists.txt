idf_component_register(
    SRCS
        "shutdown_manager.c"
    INCLUDE_DIRS
        "include"
    # Reaches into the wireless app to pick up gopro_model.h for
    # gopro_model_uses_ble_control(). Logically inverted (shared component
    # referencing variant-specific files) — Phase 3 (decompose camera_manager
    # → cam_core) routes the BLE terminate through the driver vtable and lets
    # us drop this cross-layer reference. See docs/multi-variant-restructure-plan.md §4.
    PRIV_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/wireless/components/gopro"
    REQUIRES
        camera_manager
        esp_timer
        freertos
    PRIV_REQUIRES
        open_gopro_ble
)
