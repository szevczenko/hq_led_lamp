# device_identity host unit tests (TASK-111)
#
# Standalone POSIX host test project for the access-token device identity
# owner (components/device_identity).  It compiles the production
# device_identity source together with the production app_config service
# against the REAL POSIX OSAL filesystem backend on a temporary
# file-backed LittleFS volume (osal_file_impl.c + osal_mount_impl.c +
# vendored littlefs), the vendored cJSON codec, a test-only lamp-control
# double and the Unity framework, and runs as a normal CTest executable on
# the host.
#
# Coverage per the TASK-111 definition of done:
#   - valid identity: a provisioned manufacturing record plus a valid
#     /config/identity.json loads; the stable client-ID and token-provider
#     interfaces return the validated values verbatim,
#   - every invalid record category is rejected with a distinct status:
#     missing record, unprovisioned/quarantined/credential-less
#     manufacturing record, missing member, empty client ID / token,
#     oversized file / field, malformed JSON, trailing garbage, unknown
#     member, duplicate member, control characters, unknown schema,
#     wrong-typed members,
#   - fail-off: every rejection forces the lamp output inactive
#     (lamp_control_force_inactive()) and leaves the module unloaded,
#   - no fallback: getters / the token provider report not-loaded after
#     any rejection and can never return an anonymous or partial identity,
#   - redaction/zeroization: status names carry no record content, the
#     component never logs, and device_identity_clear() invalidates the
#     identity and wipes the module token buffer.
#
# Usage:
#   cmake -S tests/device_identity -B <build-dir>
#   cmake --build <build-dir>
#   ctest --test-dir <build-dir> --output-on-failure
#   # or run the binary directly:
#   <build-dir>/device_identity_tests

cmake_minimum_required(VERSION 3.16)
project(hq_led_lamp_device_identity_tests LANGUAGES C)

set(DEVICE_IDENTITY_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components/device_identity)
set(APP_CONFIG_DIR      ${CMAKE_CURRENT_LIST_DIR}/../../components/app_config)
set(LAMP_CONTROL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components/lamp_control/include)
set(HAL_INCLUDE_DIR     ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/hal/include)
set(OSAL_DIR            ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal)
set(OSAL_INCLUDE_DIR    ${OSAL_DIR}/include)
set(OSAL_IMPL_DIR       ${OSAL_DIR}/posix)
set(LITTLEFS_DIR        ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/third_party/littlefs)
set(CJSON_DIR           ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/third_party/cJSON)
set(OSAL_COMMON_DIR     ${OSAL_DIR}/common)
set(UNITY_DIR           ${CMAKE_CURRENT_LIST_DIR}/../lamp_control/unity)

add_executable(device_identity_tests
    ${CMAKE_CURRENT_LIST_DIR}/device_identity_test.c
    ${CMAKE_CURRENT_LIST_DIR}/lamp_control_mock.c
    ${DEVICE_IDENTITY_DIR}/device_identity.c
    ${APP_CONFIG_DIR}/app_config.c
    ${OSAL_IMPL_DIR}/osal_file_impl.c
    ${OSAL_IMPL_DIR}/osal_dir_impl.c
    ${OSAL_IMPL_DIR}/osal_mount_impl.c
    ${OSAL_IMPL_DIR}/osal_assert.c
    ${OSAL_COMMON_DIR}/osal_error.c
    ${LITTLEFS_DIR}/lfs.c
    ${LITTLEFS_DIR}/lfs_util.c
    ${LITTLEFS_DIR}/bd/lfs_filebd.c
    ${CJSON_DIR}/cJSON.c
    ${UNITY_DIR}/unity.c
)

target_include_directories(device_identity_tests PRIVATE
    ${DEVICE_IDENTITY_DIR}/include
    ${APP_CONFIG_DIR}/include
    ${LAMP_CONTROL_INCLUDE_DIR}
    ${HAL_INCLUDE_DIR}
    ${OSAL_INCLUDE_DIR}
    ${OSAL_IMPL_DIR}
    ${LITTLEFS_DIR}
    ${LITTLEFS_DIR}/bd
    ${CJSON_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${UNITY_DIR}
)

# Strict flags for the components under test and the test file; vendored
# Unity/cJSON/LittleFS and the OSAL POSIX implementation are built with
# warnings relaxed (exactly like the other host test projects).
target_compile_options(device_identity_tests PRIVATE
    -Wall -Wextra -Werror -Wpedantic)
set_source_files_properties(${CJSON_DIR}/cJSON.c ${UNITY_DIR}/unity.c
    ${LITTLEFS_DIR}/lfs.c ${LITTLEFS_DIR}/lfs_util.c
    ${LITTLEFS_DIR}/bd/lfs_filebd.c
    ${OSAL_IMPL_DIR}/osal_assert.c
    ${OSAL_IMPL_DIR}/osal_dir_impl.c
    ${OSAL_IMPL_DIR}/osal_file_impl.c
    ${OSAL_IMPL_DIR}/osal_mount_impl.c
    PROPERTIES COMPILE_OPTIONS "-w")

target_compile_definitions(device_identity_tests PRIVATE
    CONFIG_HQ_PLATFORM_POSIX
    LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)

enable_testing()
add_test(NAME device_identity_tests COMMAND device_identity_tests)
set_tests_properties(device_identity_tests PROPERTIES
    LABELS "unit;device_identity")

message(STATUS "Tests: Building device-identity host test (device_identity_tests)")