# app_state host unit tests (TASK-115)
#
# Standalone host test project for the application state machine and
# watchdog policy (components/app_state).  It compiles the production
# source against test-only doubles — the lamp-control double
# (lamp_control_mock.c, fail-off recording) and the OSAL doubles
# (osal_test_support.c: pthread mutexes, captured logging, settable
# monotonic clock) — plus the vendored Unity framework, and runs as a
# normal CTest executable on the host.
#
# A deterministic test clock is injected through
# app_state_config_t::now_ms and an optional transition observer records
# every (from, to, event, owner, session) transition, so the fault-
# injection tests fully control time and observe every transition:
#
#   - every failure transition forces the lamp output inactive,
#   - online is reachable only through all five gates,
#   - the reconnect loop (disconnect -> bounded backoff -> re-gate ->
#     online) with an anti-storm budget,
#   - timeouts (connect/sync/wait) and watchdog expiry,
#   - watchdog recovery through an external reset,
#   - OTA entry/exit (begin -> OTA -> end -> boot -> re-boot -> online,
#     and OTA failure -> safe-off),
#   - stale-session and wrong-owner callbacks are rejected and counted,
#     with the safety-first fail-off before stale disconnect drops.
#
# Usage:
#   cmake -S tests/app_state -B <build-dir>
#   cmake --build <build-dir>
#   ctest --test-dir <build-dir> --output-on-failure
#   # or run the binary directly:
#   <build-dir>/app_state_tests

cmake_minimum_required(VERSION 3.16)
project(hq_led_lamp_app_state_tests LANGUAGES C)

set(APP_STATE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components/app_state)
set(LAMP_CONTROL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components/lamp_control/include)
set(HAL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/hal/include)
set(OSAL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal/include)
set(OSAL_IMPL_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal/posix)
set(LAMP_CONTROL_UNITY_DIR ${CMAKE_CURRENT_LIST_DIR}/../lamp_control/unity)

find_package(Threads REQUIRED)

add_executable(app_state_tests
    ${CMAKE_CURRENT_LIST_DIR}/app_state_test.c
    ${CMAKE_CURRENT_LIST_DIR}/lamp_control_mock.c
    ${CMAKE_CURRENT_LIST_DIR}/osal_test_support.c
    ${APP_STATE_DIR}/app_state.c
    ${LAMP_CONTROL_UNITY_DIR}/unity.c
)

target_include_directories(app_state_tests PRIVATE
    ${APP_STATE_DIR}/include
    ${LAMP_CONTROL_INCLUDE_DIR}
    ${HAL_INCLUDE_DIR}
    ${OSAL_INCLUDE_DIR}
    ${OSAL_IMPL_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${LAMP_CONTROL_UNITY_DIR}
)

# Match the platform project's strict host-test flags where Unity tolerates
# them; Unity's own sources rarely trigger warnings on current GCC/Clang.
target_compile_options(app_state_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)

target_link_libraries(app_state_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME app_state_tests COMMAND app_state_tests)
set_tests_properties(app_state_tests PROPERTIES LABELS "unit;app_state")

message(STATUS "Tests: Building application state machine host test (app_state_tests)")