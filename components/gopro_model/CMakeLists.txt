# gopro_model — shared, ESP-IDF-free GoPro camera_model_t capability helpers
# (docs/design/camera-manager.md §5.2).  Promoted out of
# apps/wireless/components/gopro/ so both the wireless (RC/BLE) and wired (USB)
# variants share one canonical set of model predicates instead of duplicating
# them (docs/design/wired-variant.md §7).
#
# Intentionally depends only on cam_core (for camera_types.h) and no ESP-IDF
# headers, so gopro_model.c is also compiled directly by the host unit tests
# (tests/host/test_gopro_model.c).
idf_component_register(
    SRCS
        "gopro_model.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # camera_model_t / camera_types.h appear in the public header.
        cam_core
)
