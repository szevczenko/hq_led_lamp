# app_config host unit tests (TASK-108)
#
# Standalone POSIX host test project for the versioned product
# configuration service (components/app_config).  Two executables:
#
# - app_config_tests: the production source against a test-only OSAL file
#   double (osal_file_mock.c, an in-memory POSIX-style store that models
#   the LittleFS semantics the OSAL backend exposes) with deterministic
#   fault injection, the vendored cJSON codec and the Unity framework.
#
# - app_config_posix_lfs_tests: the production source against the REAL
#   POSIX OSAL backend (osal_file_impl.c + osal_mount_impl.c) on a
#   temporary file-backed LittleFS volume (vendored littlefs lfs.c +
#   bd/lfs_filebd.c), so success, corruption, recovery and
#   interrupted-write scenarios run against actual LittleFS rename /
#   copy / persistence semantics, including unmount/remount durability.
#
# Both run as normal CTest executables on the host.
#
# Usage:
#   cmake -S tests/app_config -B <build-dir>
#   cmake --build <build-dir>
#   ctest --test-dir <build-dir> --output-on-failure
#   # or run the binaries directly:
#   <build-dir>/app_config_tests
#   <build-dir>/app_config_posix_lfs_tests

cmake_minimum_required(VERSION 3.16)
project(hq_led_lamp_app_config_tests LANGUAGES C)

set(APP_CONFIG_DIR   ${CMAKE_CURRENT_LIST_DIR}/../../components/app_config)
set(OSAL_DIR         ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal)
set(OSAL_INCLUDE_DIR ${OSAL_DIR}/include)
set(OSAL_IMPL_DIR    ${OSAL_DIR}/posix)
set(LITTLEFS_DIR     ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/third_party/littlefs)
set(CJSON_DIR        ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/third_party/cJSON)
set(UNITY_DIR        ${CMAKE_CURRENT_LIST_DIR}/../lamp_control/unity)

add_executable(app_config_tests
    ${CMAKE_CURRENT_LIST_DIR}/app_config_test.c
    ${CMAKE_CURRENT_LIST_DIR}/osal_file_mock.c
    ${APP_CONFIG_DIR}/app_config.c
    ${CJSON_DIR}/cJSON.c
    ${UNITY_DIR}/unity.c
)

target_include_directories(app_config_tests PRIVATE
    ${APP_CONFIG_DIR}/include
    ${OSAL_INCLUDE_DIR}
    ${OSAL_IMPL_DIR}
    ${CJSON_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${UNITY_DIR}
)

# Match the platform project's strict host-test flags where Unity and the
# vendored cJSON tolerate them; Unity's and cJSON's own sources are built
# with warnings relaxed.
target_compile_options(app_config_tests PRIVATE
    -Wall -Wextra -Werror -Wpedantic)
set_source_files_properties(${CJSON_DIR}/cJSON.c ${UNITY_DIR}/unity.c
    PROPERTIES COMPILE_OPTIONS "-w")

enable_testing()
add_test(NAME app_config_tests COMMAND app_config_tests)
set_tests_properties(app_config_tests PROPERTIES LABELS "unit;app_config")

message(STATUS "Tests: Building product configuration host test (app_config_tests)")

# ---- POSIX LittleFS backend tests (TASK-108) ----
# The production app_config source compiled against the real POSIX OSAL
# file + mount implementations on a temporary file-backed LittleFS volume.
# This exercises the repository's actual OSAL/LittleFS backend: real
# osal_rename() (atomic replacement), osal_cp() and persistence across
# unmount/remount, plus success, corruption, last-known-good recovery and
# interrupted (stale temporary) write scenarios on that backend.
add_executable(app_config_posix_lfs_tests
    ${CMAKE_CURRENT_LIST_DIR}/app_config_lfs_test.c
    ${APP_CONFIG_DIR}/app_config.c
    ${OSAL_IMPL_DIR}/osal_file_impl.c
    ${OSAL_IMPL_DIR}/osal_dir_impl.c
    ${OSAL_IMPL_DIR}/osal_mount_impl.c
    ${OSAL_DIR}/common/osal_error.c
    ${LITTLEFS_DIR}/lfs.c
    ${LITTLEFS_DIR}/lfs_util.c
    ${LITTLEFS_DIR}/bd/lfs_filebd.c
    ${CJSON_DIR}/cJSON.c
    ${UNITY_DIR}/unity.c
)

target_include_directories(app_config_posix_lfs_tests PRIVATE
    ${APP_CONFIG_DIR}/include
    ${OSAL_INCLUDE_DIR}
    ${OSAL_IMPL_DIR}
    ${LITTLEFS_DIR}
    ${LITTLEFS_DIR}/bd
    ${CJSON_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${UNITY_DIR}
)

target_compile_definitions(app_config_posix_lfs_tests PRIVATE
    CONFIG_HQ_PLATFORM_POSIX)

target_compile_options(app_config_posix_lfs_tests PRIVATE
    -Wall -Wextra -Werror -Wpedantic)
set_source_files_properties(${CJSON_DIR}/cJSON.c ${UNITY_DIR}/unity.c
    ${LITTLEFS_DIR}/lfs.c ${LITTLEFS_DIR}/lfs_util.c
    ${LITTLEFS_DIR}/bd/lfs_filebd.c
    PROPERTIES COMPILE_OPTIONS "-w")

# Silence the vendored littlefs debug/warn/error/trace printf paths exactly
# like the platform build does (LFS_CONFIG=lfs_config.h on the ESP side).
target_compile_definitions(app_config_posix_lfs_tests PRIVATE
    CONFIG_HQ_PLATFORM_POSIX
    LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)

# The real LittleFS trace/printf code paths are disabled via LFS_NO_DEBUG /
# LFS_NO_WARN / LFS_NO_ERROR above; lfs.c still references <stdio.h> is not
# needed then.  Keep -lm off: littlefs needs no math library on POSIX hosts.

add_test(NAME app_config_posix_lfs_tests COMMAND app_config_posix_lfs_tests)
set_tests_properties(app_config_posix_lfs_tests PROPERTIES
    LABELS "unit;app_config;littlefs")

message(STATUS "Tests: Building product configuration host test (app_config_tests)")
