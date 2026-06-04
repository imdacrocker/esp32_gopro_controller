cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET "esp32s3")

# Pick up shared components from the monorepo's top-level components/ dir
# (ota_io, wifi_manager, plus log_ring/can_manager/shutdown_manager/
# http_server_core after the Phase-2 lift). Recovery only links a small
# subset; the rest live there because the wireless app builds them.
list(APPEND EXTRA_COMPONENT_DIRS "../../components")

# shutdown_manager PRIV_REQUIRES open_gopro_ble (wireless-only — drops the
# BLE link during shutdown).  can_manager and http_server_core in turn
# REQUIRE shutdown_manager, so the whole chain has to be excluded from
# recovery's build to keep the dependency graph resolvable.  Recovery
# doesn't link any of these anyway.  A future cleanup that routes the BLE
# terminate through a vtable entry on camera_driver_t would let
# shutdown_manager (and the chain above it) become recovery-compatible.
set(EXCLUDE_COMPONENTS
    can_manager
    shutdown_manager
    http_server_core
    log_ring
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_gopro_canbus_recovery)
