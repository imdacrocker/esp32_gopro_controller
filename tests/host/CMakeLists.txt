# Host-side unit tests (docs/design/camera-manager.md §23).
#
# Compiles the pure-logic firmware sources (no ESP-IDF headers) against Unity
# and runs them on the development host via CTest — completely independent of
# the ESP32 build. Unity is fetched and pinned by CMake, so the only host
# requirements are CMake, a C compiler, and network access at configure time.
#
#   cmake -S tests/host -B tests/host/build
#   cmake --build tests/host/build
#   ctest --test-dir tests/host/build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(esp32_gopro_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.6.1
)
FetchContent_MakeAvailable(unity)

set(REPO_ROOT  "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(CAM_INC    "${REPO_ROOT}/apps/wireless/components/camera_manager/include")
set(GOPRO_DIR  "${REPO_ROOT}/apps/wireless/components/gopro")
set(OTA_IO_INC "${REPO_ROOT}/components/ota_io/include")

enable_testing()

# Each entry: <test-name> <test-source> <production-source...>
function(add_host_test name test_src)
    add_executable(${name} ${test_src} ${ARGN})
    target_include_directories(${name} PRIVATE
        "${CAM_INC}" "${GOPRO_DIR}" "${OTA_IO_INC}")
    target_link_libraries(${name} PRIVATE unity)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_host_test(test_gopro_model
    "${CMAKE_CURRENT_SOURCE_DIR}/test_gopro_model.c"
    "${GOPRO_DIR}/gopro_model.c")

add_host_test(test_mismatch
    "${CMAKE_CURRENT_SOURCE_DIR}/test_mismatch.c"
    "${REPO_ROOT}/apps/wireless/components/camera_manager/mismatch.c")

add_host_test(test_reorder_validate
    "${CMAKE_CURRENT_SOURCE_DIR}/test_reorder_validate.c"
    "${REPO_ROOT}/apps/wireless/components/camera_manager/reorder_validate.c")

add_host_test(test_app_date_compare
    "${CMAKE_CURRENT_SOURCE_DIR}/test_app_date_compare.c"
    "${REPO_ROOT}/components/ota_io/app_date_compare.c")
