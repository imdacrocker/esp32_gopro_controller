# cam_core — shared, BLE-free camera abstraction (driver vtable, recording
# intent + grace + mismatch step, status-cache & CAN-state translation, slot
# state record).  Multi-variant restructure §3 / §4
# (docs/multi-variant-restructure-plan.md).
#
# Phase 3.3 lands the runtime: cam_core.c owns the recording-intent
# engine, mismatch poll, broadcast-driver dispatch dedup, CAN-state
# translation, sleep/teardown helpers, and the global auto-control flag.
# The wireless camera_manager component now embeds a cam_core_slot_t in
# each of its slots and delegates here.
idf_component_register(
    SRCS
        "cam_core.c"
        "mismatch.c"
        "reorder_validate.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_timer       # esp_timer_handle_t exposed by cam_core_slot_t
        freertos        # SemaphoreHandle_t for the cam_core mutex
)
