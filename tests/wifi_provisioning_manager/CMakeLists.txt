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

message(STATUS "Tests: Building provisioning-manager host test "
               "(wifi_provisioning_manager_tests)")