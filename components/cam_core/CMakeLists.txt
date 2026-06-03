# cam_core — shared, BLE-free camera abstraction (driver vtable, recording
# intent + grace + mismatch step, status-cache & CAN-state translation, slot
# state record).  Multi-variant restructure §3 / §4
# (docs/multi-variant-restructure-plan.md).
#
# Phase 3.2 lands the type-only seam: pure logic units (mismatch,
# reorder_validate) and the public header defining the vtable + per-slot
# state record live here.  The recording engine still lives in
# apps/wireless/components/camera_manager/ at this phase; Phase 3.3 moves
# it in.
idf_component_register(
    SRCS
        "mismatch.c"
        "reorder_validate.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_timer       # esp_timer_handle_t exposed by cam_core_slot_t
)
