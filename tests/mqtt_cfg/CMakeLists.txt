# mqtt_cfg host unit tests (TASK-110)
#
# Standalone POSIX host test projects for the broker/TLS configuration owner
# (components/mqtt_cfg).  Two executables:
#
# - mqtt_cfg_tests: the production component against the REAL POSIX OSAL
#   backend on a temporary LittleFS volume, the REAL mqtt_config codec, a
#   test-only mqtt_app double and a lamp-control double; asserts the
#   verified-TLS-only policy (plaintext/skip-verify/CA-path/hostname
#   rejection, fail-off) and the verified transport configuration through
#   the actual mqtt_config getters.
#
# - mqtt_cfg_tls_it_tests: the production component end-to-end against the
#   real Mongoose MQTT application layer (mqtt_app.c + mongoose_process.c)
#   with an in-process TLS MQTT broker: trusted-CA connect (verified TLS),
#   unknown CA failure, hostname mismatch failure, plaintext rejection and
#   invalid path rejection.  See mqtt_cfg_tls_it_test.c.
#
# Usage:
#   cmake -S tests/mqtt_cfg -B <build-dir>
#   cmake --build <build-dir>
#   ctest --test-dir <build-dir> --output-on-failure
#   # or run the binaries directly:
#   <build-dir>/mqtt_cfg_tests
#   <build-dir>/mqtt_cfg_tls_it_tests

cmake_minimum_required(VERSION 3.16)
project(hq_led_lamp_mqtt_cfg_tests LANGUAGES C)

set(MQTT_CFG_DIR     ${CMAKE_CURRENT_LIST_DIR}/../../components/mqtt_cfg)
set(LAMP_CONTROL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components/lamp_control/include)
set(HAL_INCLUDE_DIR  ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/hal/include)
set(OSAL_DIR         ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal)
set(OSAL_INCLUDE_DIR  ${OSAL_DIR}/include)
set(OSAL_IMPL_DIR     ${OSAL_DIR}/posix)
set(MONGOOSE_DIR      ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/mongoose)
set(LITTLEFS_DIR      ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/third_party/littlefs)
set(CJSON_DIR         ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/third_party/cJSON)
set(CERT_DIR          ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/cert)
set(OSAL_COMMON_DIR   ${OSAL_DIR}/common)
set(UNITY_DIR         ${CMAKE_CURRENT_LIST_DIR}/../lamp_control/unity)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# mqtt_cfg_tests — component + real OSAL/LittleFS + real mqtt_config codec
# ---------------------------------------------------------------------------
add_executable(mqtt_cfg_tests
    ${CMAKE_CURRENT_LIST_DIR}/mqtt_cfg_test.c
    ${CMAKE_CURRENT_LIST_DIR}/mqtt_app_mock.c
    ${CMAKE_CURRENT_LIST_DIR}/lamp_control_mock.c
    ${MQTT_CFG_DIR}/mqtt_cfg.c
    ${MONGOOSE_DIR}/mqtt_config.c
    ${OSAL_IMPL_DIR}/osal_file_impl.c
    ${OSAL_IMPL_DIR}/osal_dir_impl.c
    ${OSAL_IMPL_DIR}/osal_mount_impl.c
    ${OSAL_IMPL_DIR}/osal_log_impl.c
    ${OSAL_IMPL_DIR}/osal_mutex_impl.c
    ${OSAL_IMPL_DIR}/osal_task_impl.c
    ${OSAL_IMPL_DIR}/osal_assert.c
    ${OSAL_COMMON_DIR}/osal_error.c
    ${OSAL_COMMON_DIR}/osal_log.c
    ${LITTLEFS_DIR}/lfs.c
    ${LITTLEFS_DIR}/lfs_util.c
    ${LITTLEFS_DIR}/bd/lfs_filebd.c
    ${CJSON_DIR}/cJSON.c
    ${UNITY_DIR}/unity.c
)

target_include_directories(mqtt_cfg_tests PRIVATE
    ${MQTT_CFG_DIR}/include
    ${LAMP_CONTROL_INCLUDE_DIR}
    ${HAL_INCLUDE_DIR}
    ${OSAL_INCLUDE_DIR}
    ${OSAL_IMPL_DIR}
    ${MONGOOSE_DIR}
    ${LITTLEFS_DIR}
    ${LITTLEFS_DIR}/bd
    ${CJSON_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${UNITY_DIR}
)

# Strict flags for the component under test; vendored Unity/cJSON/LittleFS and
# platform (mqtt_config) sources are built with warnings relaxed (they rarely
# are clean under -Wpedantic on current GCC/Clang, exactly like the other
# host test projects).
target_compile_options(mqtt_cfg_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
set_source_files_properties(${CJSON_DIR}/cJSON.c ${UNITY_DIR}/unity.c
    ${LITTLEFS_DIR}/lfs.c ${LITTLEFS_DIR}/lfs_util.c
    ${LITTLEFS_DIR}/bd/lfs_filebd.c
    ${MONGOOSE_DIR}/mqtt_config.c
    PROPERTIES COMPILE_OPTIONS "-w")

target_compile_definitions(mqtt_cfg_tests PRIVATE
    CONFIG_HQ_PLATFORM_POSIX
    MQTT_CFG_TEST_CERT_DIR="${CERT_DIR}"
    LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)

target_link_libraries(mqtt_cfg_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME mqtt_cfg_tests COMMAND mqtt_cfg_tests)
set_tests_properties(mqtt_cfg_tests PROPERTIES
    LABELS "unit;mqtt_cfg")

message(STATUS "Tests: Building MQTT/TLS configuration host test (mqtt_cfg_tests)")

# ---------------------------------------------------------------------------
# mqtt_cfg_tls_it_tests — end-to-end TLS integration tests (real transport)
# ---------------------------------------------------------------------------
# Runs the production component against the REAL Mongoose transport built
# with OpenSSL (MG_ARCH_UNIX + MG_TLS_OPENSSL), the REAL mqtt_app.c and
# mongoose_process.c application layers and an in-process TLS MQTT broker
# on loopback.  Covers the TASK-110 definition-of-done transport cases:
# trusted CA success, unknown CA failure, hostname mismatch failure,
# plaintext rejection (no transport) and invalid CA-path rejection.
set(MONGOOSE_POSIX_DIR ${MONGOOSE_DIR}/posix)

add_executable(mqtt_cfg_tls_it_tests
    ${CMAKE_CURRENT_LIST_DIR}/mqtt_cfg_tls_it_test.c
    ${CMAKE_CURRENT_LIST_DIR}/lamp_control_mock.c
    ${MQTT_CFG_DIR}/mqtt_cfg.c
    ${MONGOOSE_DIR}/mqtt_app.c
    ${MONGOOSE_DIR}/mqtt_config.c
    ${MONGOOSE_DIR}/mongoose.c
    ${MONGOOSE_DIR}/mongoose_process.c
    ${OSAL_IMPL_DIR}/osal_assert.c
    ${OSAL_IMPL_DIR}/osal_bin_sem_impl.c
    ${OSAL_IMPL_DIR}/osal_dir_impl.c
    ${OSAL_IMPL_DIR}/osal_file_impl.c
    ${OSAL_IMPL_DIR}/osal_log_impl.c
    ${OSAL_IMPL_DIR}/osal_mount_impl.c
    ${OSAL_IMPL_DIR}/osal_mutex_impl.c
    ${OSAL_IMPL_DIR}/osal_queue_impl.c
    ${OSAL_IMPL_DIR}/osal_task_impl.c
    ${OSAL_IMPL_DIR}/osal_timer_impl.c
    ${OSAL_COMMON_DIR}/osal_error.c
    ${OSAL_COMMON_DIR}/osal_log.c
    ${LITTLEFS_DIR}/lfs.c
    ${LITTLEFS_DIR}/lfs_util.c
    ${LITTLEFS_DIR}/bd/lfs_filebd.c
    ${CJSON_DIR}/cJSON.c
    ${UNITY_DIR}/unity.c
)

target_include_directories(mqtt_cfg_tls_it_tests PRIVATE
    ${MQTT_CFG_DIR}/include
    ${LAMP_CONTROL_INCLUDE_DIR}
    ${HAL_INCLUDE_DIR}
    ${OSAL_INCLUDE_DIR}
    ${OSAL_IMPL_DIR}
    ${MONGOOSE_DIR}
    ${MONGOOSE_POSIX_DIR}
    ${LITTLEFS_DIR}
    ${LITTLEFS_DIR}/bd
    ${CJSON_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${UNITY_DIR}
)

# Strict warnings for the component under test and the test file; vendored and
# platform sources (Mongoose amalgamation, mqtt_app/mqtt_config/mongoose_process,
# OSAL POSIX implementation, cJSON, LittleFS, Unity) are built with warnings
# relaxed.
target_compile_options(mqtt_cfg_tls_it_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
set_source_files_properties(${CJSON_DIR}/cJSON.c ${UNITY_DIR}/unity.c
    ${LITTLEFS_DIR}/lfs.c ${LITTLEFS_DIR}/lfs_util.c
    ${LITTLEFS_DIR}/bd/lfs_filebd.c
    ${MONGOOSE_DIR}/mongoose.c
    ${MONGOOSE_DIR}/mqtt_app.c
    ${MONGOOSE_DIR}/mqtt_config.c
    ${MONGOOSE_DIR}/mongoose_process.c
    ${OSAL_IMPL_DIR}/osal_assert.c
    ${OSAL_IMPL_DIR}/osal_bin_sem_impl.c
    ${OSAL_IMPL_DIR}/osal_dir_impl.c
    ${OSAL_IMPL_DIR}/osal_file_impl.c
    ${OSAL_IMPL_DIR}/osal_log_impl.c
    ${OSAL_IMPL_DIR}/osal_mount_impl.c
    ${OSAL_IMPL_DIR}/osal_mutex_impl.c
    ${OSAL_IMPL_DIR}/osal_queue_impl.c
    ${OSAL_IMPL_DIR}/osal_task_impl.c
    ${OSAL_IMPL_DIR}/osal_timer_impl.c
    ${OSAL_COMMON_DIR}/osal_error.c
    ${OSAL_COMMON_DIR}/osal_log.c
    PROPERTIES COMPILE_OPTIONS "-w")

# The real transport is built for the host with OpenSSL as the TLS backend:
# the verified handshake (trusted CA, unknown CA, hostname mismatch) can
# then be exercised end-to-end exactly like the ESP-IDF mbedTLS path.
target_compile_definitions(mqtt_cfg_tls_it_tests PRIVATE
    CONFIG_HQ_PLATFORM_POSIX
    MG_ARCH=MG_ARCH_UNIX
    MG_TLS=MG_TLS_OPENSSL
    MQTT_CFG_TEST_CERT_DIR="${CERT_DIR}"
    LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)

find_package(OpenSSL REQUIRED)
target_link_libraries(mqtt_cfg_tls_it_tests PRIVATE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto)

add_test(NAME mqtt_cfg_tls_it_tests COMMAND mqtt_cfg_tls_it_tests)
set_tests_properties(mqtt_cfg_tls_it_tests PROPERTIES
    LABELS "integration;mqtt_cfg")

message(STATUS "Tests: Building MQTT/TLS end-to-end integration test (mqtt_cfg_tls_it_tests)")