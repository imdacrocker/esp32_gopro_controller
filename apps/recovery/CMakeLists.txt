cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET "esp32s3")

# Pick up shared components from the monorepo's top-level components/ dir
# (ota_io, wifi_manager, plus log_ring/can_manager/shutdown_manager/
# http_server_core after the Phase-2 lift). Recovery only links a small
# subset; the rest live there because the wireless app builds them.
list(APPEND EXTRA_COMPONENT_DIRS "../../components")

# Some top-level components still REQUIRE camera_manager (wireless-only,
# pending the Phase-3 cam_core extraction — see
# docs/multi-variant-restructure-plan.md §4). Recovery doesn't link them
# anyway, so exclude them from its build to keep the dependency graph
# resolvable.
set(EXCLUDE_COMPONENTS
    can_manager
    shutdown_manager
    http_server_core
    log_ring
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_gopro_canbus_recovery)
