idf_component_register(
    SRCS
        "camera_manager.c"
        "pair_attempt.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cam_core        # shared types, driver vtable, per-slot record (Phase 3.2)
        bt              # NimBLE (host/ble_hs.h via camera_manager_ble.h)
        ble_core        # drop a removed camera's bond
        nvs_flash
        esp_timer
        freertos
)
