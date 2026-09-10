# tb_application host mock tests (TASK-112 / TASK-113 / TASK-114)
#
# Standalone POSIX host test project for the ThingsBoard desired-state
# synchronizer (components/tb_application).  It compiles the production
# component together with the REAL platform ThingsBoard client/attributes
# RPC/telemetry sources (tb_client.c, tb_attributes.c, tb_rpc.c,
# tb_telemetry.c, ...) against the platform's own mqtt_app test double
# (platform/hq_platform/tests/thingsboard/mqtt_app_mock.c) — exactly the
# harness the platform's own tb_tests use — plus a lamp-control double
# (lamp_control_mock.c) and the Unity framework.
#
# The mqtt_app double supports deterministic message delivery and lifecycle
# simulation:
#   - mqtt_app_mock_simulate_connect() / _remote_disconnect() drive the
#     ThingsBoard on_connect/on_disconnect glue -> tb_application_on_connected()
#     / tb_application_on_disconnected(),
#   - mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/<id>",
#     ...) delivers attribute responses through the real tb_attributes
#     request/response machinery,
#   - mqtt_app_mock_deliver_message("v1/devices/me/attributes", ...) delivers
#     shared-attribute updates through tb_attributes_subscribe's callback.
#
# A clock provider injected through tb_application_config_t::now_ms drives the
# sync-window deadline and the bounded retry/backoff deterministically.
#
# Coverage per the TASK-112 definition of done:
#   - valid complete synchronization (response and update paths),
#   - no output is enabled before valid complete synchronization,
#   - partial/invalid/out-of-range/oversized data rejected with fail-off,
#   - sync timeout fails off and retries with bounded backoff,
#   - duplicate updates and late/stale responses for consumed attempts are
#     never applied twice,
#   - disconnect always forces the lamp output inactive; reconnect starts a
#     fresh session and ThingsBoard stays authoritative after reconnect,
#   - bounded retry budget exhaustion parks the module (output off) until the
#     next reconnect.
#
# Coverage per the TASK-113 definition of done: the four documented RPC
# methods apply in order (hardware first, then success response, then
# telemetry), invalid/malformed/unknown/oversized requests are rejected with
# structured errors, and hardware failures never report success.
#
# Coverage per the TASK-114 definition of done: the documented seven-field
# telemetry record is published on connect, on every successful state change
# and periodically with rate limiting; `pwm_duty` reflects the applied PWM
# duty; publication is suppressed while disconnected; and the serialized
# payload never carries secrets (hostile config strings are sanitized to the
# safe charset, exactly seven fields, valid JSON framing).
#
# Usage:
#   cmake -S tests/tb_application -B <build-dir> -DCMAKE_BUILD_TYPE=Debug   (see top-level scripts)
#   cmake -S tests/tb_application -B tests/tb_application/build
#   cmake --build tests/tb_application/build
#   ctest --test-dir tests/tb_application/build --output-on-failure
#   # or run the binary directly:
#   tests/tb_application/build/tb_application_tests

cmake_minimum_required(VERSION 3.16)
project(hq_led_lamp_tb_application_tests LANGUAGES C)

set(TB_APP_DIR        ${CMAKE_CURRENT_LIST_DIR}/../../components/tb_application)
set(TB_SRC_DIR         ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/thingsboard)
set(TB_TEST_MOCK_DIR   ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/tests/thingsboard)
set(LAMP_CONTROL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components/lamp_control/include)
set(HAL_INCLUDE_DIR    ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/hal/include)
set(MONGOOSE_DIR       ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/mongoose)
set(OSAL_DIR           ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal)
set(OSAL_INCLUDE_DIR   ${OSAL_DIR}/include)
set(OSAL_IMPL_DIR       ${OSAL_DIR}/posix)
set(OSAL_COMMON_DIR     ${OSAL_DIR}/common)
set(LITTLEFS_DIR        ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/third_party/littlefs)
set(CJSON_DIR           ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/third_party/cJSON)
set(UNITY_DIR            ${CMAKE_CURRENT_LIST_DIR}/../lamp_control/unity)

# The OSAL POSIX backend sources, exactly the set hq_osal assembles for the
# platform host builds (common/*.c + posix/*.c + the vendored littlefs trio).
# The POSIX OTA implementation (osal_ota_impl.c / osal_ota_state_impl.c) is
# replaced by the test-only osal_ota_mock.c: it pulls in Mongoose's
# mg_sha256_* and firmware update is out of scope for the TASK-112 tests.
file(GLOB OSAL_COMMON_SOURCES ${OSAL_COMMON_DIR}/*.c)
file(GLOB OSAL_PLATFORM_SOURCES ${OSAL_IMPL_DIR}/*.c)
list(REMOVE_ITEM OSAL_PLATFORM_SOURCES
    ${OSAL_IMPL_DIR}/osal_ota_impl.c
    ${OSAL_IMPL_DIR}/osal_ota_state_impl.c)

# The REAL platform ThingsBoard modules (the same modules the platform's own
# tb_tests link against the mqtt_app test double).
set(TB_SOURCES
    ${TB_SRC_DIR}/tb_client.c
    ${TB_SRC_DIR}/tb_telemetry.c
    ${TB_SRC_DIR}/tb_attributes.c
    ${TB_SRC_DIR}/tb_rpc.c
    ${TB_SRC_DIR}/tb_provision.c
    ${TB_SRC_DIR}/tb_claim.c
    ${TB_SRC_DIR}/tb_firmware_update.c
)

find_package(Threads REQUIRED)

add_executable(tb_application_tests
    ${CMAKE_CURRENT_LIST_DIR}/tb_application_test.c
    ${CMAKE_CURRENT_LIST_DIR}/lamp_control_mock.c
    ${CMAKE_CURRENT_LIST_DIR}/osal_ota_mock.c
    ${TB_TEST_MOCK_DIR}/mqtt_app_mock.c
    ${TB_APP_DIR}/tb_application.c
    ${TB_SOURCES}
    ${OSAL_COMMON_SOURCES}
    ${OSAL_PLATFORM_SOURCES}
    ${LITTLEFS_DIR}/lfs.c
    ${LITTLEFS_DIR}/lfs_util.c
    ${LITTLEFS_DIR}/bd/lfs_filebd.c
    ${CJSON_DIR}/cJSON.c
    ${UNITY_DIR}/unity.c
)

target_include_directories(tb_application_tests PRIVATE
    ${TB_APP_DIR}/include
    ${TB_SRC_DIR}/include
    ${TB_TEST_MOCK_DIR}
    ${LAMP_CONTROL_INCLUDE_DIR}
    ${HAL_INCLUDE_DIR}
    ${MONGOOSE_DIR}
    ${OSAL_INCLUDE_DIR}
    ${OSAL_IMPL_DIR}
    ${LITTLEFS_DIR}
    ${LITTLEFS_DIR}/bd
    ${CJSON_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${UNITY_DIR}
)

# Strict flags for the component under test, its mock harness and the test
# file; vendored Unity/cJSON/LittleFS, the platform ThingsBoard/MQTT-mock
# sources and the OSAL POSIX implementation are built with warnings relaxed
# (exactly like the other host test projects).
target_compile_options(tb_application_tests PRIVATE
    -Wall -Wextra -Werror -Wpedantic)
set_source_files_properties(${CJSON_DIR}/cJSON.c ${UNITY_DIR}/unity.c
    ${LITTLEFS_DIR}/lfs.c ${LITTLEFS_DIR}/lfs_util.c
    ${LITTLEFS_DIR}/bd/lfs_filebd.c
    ${TB_SRC_DIR}/tb_client.c
    ${TB_SRC_DIR}/tb_telemetry.c
    ${TB_SRC_DIR}/tb_attributes.c
    ${TB_SRC_DIR}/tb_rpc.c
    ${TB_SRC_DIR}/tb_provision.c
    ${TB_SRC_DIR}/tb_claim.c
    ${TB_SRC_DIR}/tb_firmware_update.c
    ${TB_TEST_MOCK_DIR}/mqtt_app_mock.c
    ${OSAL_COMMON_DIR}/osal_error.c
    ${OSAL_COMMON_DIR}/osal_log.c
    ${OSAL_IMPL_DIR}/osal_assert.c
    ${OSAL_IMPL_DIR}/osal_bin_sem_impl.c
    ${OSAL_IMPL_DIR}/osal_count_sem_impl.c
    ${OSAL_IMPL_DIR}/osal_dir_impl.c
    ${OSAL_IMPL_DIR}/osal_file_impl.c
    ${OSAL_IMPL_DIR}/osal_log_impl.c
    ${OSAL_IMPL_DIR}/osal_mount_impl.c
    ${OSAL_IMPL_DIR}/osal_mutex_impl.c
    ${OSAL_IMPL_DIR}/osal_queue_impl.c
    ${OSAL_IMPL_DIR}/osal_task_impl.c
    ${OSAL_IMPL_DIR}/osal_timer_impl.c
    PROPERTIES COMPILE_OPTIONS "-w")

target_compile_definitions(tb_application_tests PRIVATE
    CONFIG_HQ_PLATFORM_POSIX
    LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)

target_link_libraries(tb_application_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME tb_application_tests COMMAND tb_application_tests)
set_tests_properties(tb_application_tests PROPERTIES
    LABELS "unit;tb_application")

message(STATUS "Tests: Building ThingsBoard desired-state sync test (tb_application_tests)")