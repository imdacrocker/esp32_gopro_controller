cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET "esp32s3")

# Pick up shared components from the monorepo's top-level components/ dir
# (currently just `ota_io`). The wireless app's components live under
# apps/wireless/components/ and are intentionally invisible here, so EXCLUDE_COMPONENTS
# is no longer needed.
list(APPEND EXTRA_COMPONENT_DIRS "../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_gopro_canbus_recovery)
