# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET "esp32s3")

# Pick up shared components from the monorepo's top-level components/ dir
# (currently just `ota_io`, shared with the recovery app).
list(APPEND EXTRA_COMPONENT_DIRS "../../components")

# Tell ESP-IDF to also scan components/gopro/ for sub-components
# (open_gopro_ble, open_gopro_http, gopro_wifi_rc live there).
list(APPEND EXTRA_COMPONENT_DIRS "components/gopro")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_gopro_canbus_controller_v2)

# ---- Web UI LittleFS image -------------------------------------------------
# compress.py copies index.html and writes plain + gzipped CSS/JS into the
# staging dir; littlefs_create_partition_image builds storage.bin from it
# and wires it into `idf.py flash` via FLASH_IN_PROJECT.
#
# OTA runtime config (base URL, repo path) is served by the device via
# /api/version — no per-build file goes into LittleFS for that.

set(WEB_UI_SRC "${CMAKE_SOURCE_DIR}/web_ui")
set(WEB_UI_OUT "${CMAKE_BINARY_DIR}/web_ui_out")

add_custom_target(web_ui_stage
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WEB_UI_OUT}"
    COMMAND ${PYTHON} "${WEB_UI_SRC}/compress.py" "${WEB_UI_SRC}" "${WEB_UI_OUT}"
    COMMENT "Staging web UI (copy + gzip)"
)

littlefs_create_partition_image(storage "${WEB_UI_OUT}" FLASH_IN_PROJECT DEPENDS web_ui_stage)
