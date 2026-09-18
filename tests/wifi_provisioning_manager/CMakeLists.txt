# wifi_provisioning_manager host unit tests (TASK-126)
#
# Standalone host test project for the product-owned provisioning adapter
# (components/wifi_provisioning_manager).  It compiles the production source
# against test-only doubles for every platform boundary — the provisioning
# application double (wifi_provisioning_mock.c), the automatic fallback
# controller double (wifi_provisioning_controller_mock.c, TASK-132), the
# shared Mongoose process double (mongoose_process_mock.c), the Wi-Fi
# manager saved-credentials double (wifi_mgmt_mock.c) and the OSAL doubles
# (osal_test_support.c) — plus the vendored Unity framework, and runs as a
# normal CTest executable on the host.
#
# The adapter is compiled with WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY
# so the runtime listener-URL overrides are exercised; on the ESP target
# that block is compiled out and the platform's compiled-in default listen
# URLs are used.
#
# Coverage: start/stop lifecycle and idempotency, start before Mongoose is
# running, stop with the portal already stopped, start/stop failure
# propagation, concurrent start/stop serialization, listener-URL override
# behavior, the is-portal-active and has-saved-credentials queries, deinit
# teardown of an active portal, the controller notification translation
# (TASK-132: hook registration, an event stored for each transition,
# stale-session notifications discarded, poll-event ordering STARTED then
# SUCCEEDED, portal start failure -> FAILED), the platform start-status
# mapping (TASK-134: every documented platform failure mode maps to its
# distinct adapter status, success maps to OK, and is_active() is true only
# when the whole portal - AP + both listeners - is genuinely reachable) and
# the credentials-never-logged secrecy rule (log-content regression).
#
# Usage:
#   cmake -S tests/wifi_provisioning_manager -B <build-dir>
#   cmake --build <build-dir>
#   ctest --test-dir <build-dir> --output-on-failure
#   # or run the binary directly:
#   <build-dir>/wifi_provisioning_manager_tests

cmake_minimum_required(VERSION 3.16)
project(hq_led_lamp_wifi_provisioning_manager_tests LANGUAGES C)

set(WIFI_PROVISIONING_MANAGER_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_provisioning_manager)
set(WIFI_PROVISIONING_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/wifi_provisioning)
set(MONGOOSE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/mongoose)
set(OSAL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal/include)
set(OSAL_IMPL_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal/posix)
set(WIFI_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/wifi)
set(UNITY_DIR ${CMAKE_CURRENT_LIST_DIR}/../lamp_control/unity)

find_package(Threads REQUIRED)

add_executable(wifi_provisioning_manager_tests
    ${CMAKE_CURRENT_LIST_DIR}/wifi_provisioning_manager_test.c
    ${CMAKE_CURRENT_LIST_DIR}/wifi_provisioning_mock.c
    ${CMAKE_CURRENT_LIST_DIR}/wifi_provisioning_controller_mock.c
    ${CMAKE_CURRENT_LIST_DIR}/mongoose_process_mock.c
    ${CMAKE_CURRENT_LIST_DIR}/wifi_mgmt_mock.c
    ${CMAKE_CURRENT_LIST_DIR}/osal_test_support.c
    ${WIFI_PROVISIONING_MANAGER_DIR}/wifi_provisioning_manager.c
    ${UNITY_DIR}/unity.c
)

target_include_directories(wifi_provisioning_manager_tests PRIVATE
    ${WIFI_PROVISIONING_MANAGER_DIR}/include
    ${WIFI_PROVISIONING_DIR}
    ${MONGOOSE_DIR}
    ${OSAL_INCLUDE_DIR}
    ${OSAL_IMPL_DIR}
    ${WIFI_INCLUDE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${UNITY_DIR}
)

# Test/host-build-only adapter surface (runtime listener-URL overrides).
target_compile_definitions(wifi_provisioning_manager_tests PRIVATE
    WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY=1
)

# Match the platform project's strict host-test flags where Unity tolerates
# them; Unity's own sources rarely trigger warnings on current GCC/Clang.
target_compile_options(wifi_provisioning_manager_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)

target_link_libraries(wifi_provisioning_manager_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME wifi_provisioning_manager_tests
         COMMAND wifi_provisioning_manager_tests)
set_tests_properties(wifi_provisioning_manager_tests PROPERTIES
                     LABELS "unit;wifi_provisioning_manager")

# --------------------------------------------------------------------- #
# TASK-136: wifi_storage_overwrite_tests — portal submit-overwrite path #
# --------------------------------------------------------------------- #
#
# Second host test executable for the portal "submit a credential while a
# stale one already exists" path.  Unlike the adapter tests above (which
# double every platform boundary), this one compiles the REAL platform
# Wi-Fi manager (wifi_managment.c) and its persistence module
# (wifi_config.c) on the host and drives the exact submission sequence the
# portal POST /api/v1/wifi/credentials handler executes:
#
#   wifi_mgmt_set_ap_name() + wifi_mgmt_set_password() + wifi_mgmt_connect()
#
# against:
#   - a deterministic radio double (wifi_hal_mock_min.c) that rejects the
#     stale credential and accepts the newly submitted one,
#   - a real temp-directory-backed file system (wifi_storage_osal_support.c)
#     that stands in for the product storage mount (lamp_fs mounts the
#     LittleFS "storage" partition at /littlefs; the ESP OSAL maps the
#     relative WIFI_CONFIG_FILE_PATH "wifi_ap.json" onto
#     /littlefs/wifi_ap.json — the host double applies the same logical-path
#     mapping and the same CREATE|TRUNCATE open semantics against a real
#     file),
#   - the REAL platform POSIX OSAL back-ends for mutex / binary semaphore /
#     task (/osal/posix), so the manager's worker task, its semaphores and
#     the connect/persist state machine behave exactly like on target.
#
# The tests prove that a submitted credential unconditionally replaces the
# saved one (same-SSID submissions update in place; different-SSID
# submissions become the last-used entry), that the station switches within
# the same lifecycle (no reboot), and that no log line contains the old or
# new secret.

set(CJSON_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/third_party/cJSON)

add_executable(wifi_storage_overwrite_tests
    ${CMAKE_CURRENT_LIST_DIR}/wifi_storage_overwrite_test.c
    ${CMAKE_CURRENT_LIST_DIR}/wifi_hal_mock_min.c
    ${CMAKE_CURRENT_LIST_DIR}/wifi_storage_osal_support.c
    ${WIFI_INCLUDE_DIR}/wifi_managment.c
    ${WIFI_INCLUDE_DIR}/wifi_config.c
    ${OSAL_IMPL_DIR}/osal_mutex_impl.c
    ${OSAL_IMPL_DIR}/osal_bin_sem_impl.c
    ${OSAL_IMPL_DIR}/osal_task_impl.c
    ${OSAL_IMPL_DIR}/osal_assert.c
    ${CJSON_DIR}/cJSON.c
    ${UNITY_DIR}/unity.c
)

target_include_directories(wifi_storage_overwrite_tests PRIVATE
    ${WIFI_INCLUDE_DIR}
    ${OSAL_INCLUDE_DIR}
    ${OSAL_IMPL_DIR}
    ${CJSON_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${UNITY_DIR}
)

target_compile_options(wifi_storage_overwrite_tests PRIVATE
    -Wall -Wextra -Werror -Wpedantic)

target_link_libraries(wifi_storage_overwrite_tests PRIVATE Threads::Threads)

add_test(NAME wifi_storage_overwrite_tests
         COMMAND wifi_storage_overwrite_tests)
set_tests_properties(wifi_storage_overwrite_tests PROPERTIES
                     LABELS "unit;wifi_provisioning_manager;overwrite")

message(STATUS "Tests: Building wifi_ap.json overwrite host test "
               "(wifi_storage_overwrite_tests)")