
# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
# Pick up shared components from the monorepo's top-level components/ dir
# (currently just `ota_io`, shared with the recovery app).
# Tell ESP-IDF to also scan components/gopro/ for sub-components
#list(APPEND EXTRA_COMPONENT_DIRS "components/gopro")
# ---- Web UI LittleFS image -------------------------------------------------
# compress.py copies index.html and writes plain + gzipped CSS/JS into the
# staging dir; littlefs_create_partition_image builds storage.bin from it
# and wires it into `idf.py flash` via FLASH_IN_PROJECT.
# OTA runtime config (base URL, repo path) is served by the device via
# /api/version — no per-build file goes into LittleFS for that.
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_gopro_canbus_wired)
SET(IDF_TARGET "esp32s3")
SET(WEB_UI_SRC "${CMAKE_SOURCE_DIR}/web_ui")
SET(WEB_UI_OUT "${CMAKE_BINARY_DIR}/web_ui_out")
