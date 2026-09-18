# Standalone first-use concurrency regression (TASK-109 review round 3,
# issue 4)
#
# The first-use lock-creation race can only be exercised in a process where
# NO network_manager API call has run before the concurrent burst.  The main
# Unity suite cannot guarantee that (earlier tests publish the lock), so this
# separate executable runs the burst first in a fresh process, guarded by an
# explicit NULL-premise assertion, and then covers the complementary
# already-published-lock lifecycle race.
#
# Usage:
#   cmake -S tests/network_manager/concurrency -B <build-dir>
#   cmake --build <build-dir>
#   ctest --test-dir <build-dir> --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(hq_led_lamp_network_manager_concurrency_tests LANGUAGES C)

set(NETWORK_MANAGER_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../components/network_manager)
set(LAMP_CONTROL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../components/lamp_control/include)
set(HAL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../platform/hq_platform/src/hal/include)
set(OSAL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../platform/hq_platform/src/osal/include)
set(OSAL_IMPL_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../platform/hq_platform/src/osal/posix)
set(WIFI_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../platform/hq_platform/src/wifi)
set(UNITY_DIR ${CMAKE_CURRENT_LIST_DIR}/../../lamp_control/unity)

find_package(Threads REQUIRED)

add_executable(network_manager_concurrency_tests
    ${CMAKE_CURRENT_LIST_DIR}/concurrency_first_use_test.c
    ${CMAKE_CURRENT_LIST_DIR}/../wifi_mgmt_mock.c
    ${CMAKE_CURRENT_LIST_DIR}/../osal_test_support.c
    ${CMAKE_CURRENT_LIST_DIR}/../lamp_control_mock.c
    ${NETWORK_MANAGER_DIR}/network_manager.c
    ${UNITY_DIR}/unity.c
)

target_include_directories(network_manager_concurrency_tests PRIVATE
    ${NETWORK_MANAGER_DIR}/include
    ${LAMP_CONTROL_INCLUDE_DIR}
    ${HAL_INCLUDE_DIR}
    ${OSAL_INCLUDE_DIR}
    ${OSAL_IMPL_DIR}
    ${WIFI_INCLUDE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/..
    ${CMAKE_CURRENT_LIST_DIR}/../config
    ${UNITY_DIR}
)

# Mirror the product default for the adapter's Kconfig-selected Wi-Fi start
# mode (KLC_WIFI_DEFAULT_MODE, main/Kconfig.projbuild): the host build uses
# AP+STA (T_WIFI_TYPE_CLI_SER), matching the main network-manager suite.
target_compile_definitions(network_manager_concurrency_tests PRIVATE
    CONFIG_KLC_WIFI_DEFAULT_MODE_APSTA=1
)

target_compile_options(network_manager_concurrency_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)

target_link_libraries(network_manager_concurrency_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME network_manager_concurrency_tests
         COMMAND network_manager_concurrency_tests)
set_tests_properties(network_manager_concurrency_tests PROPERTIES
                     LABELS "unit;network_manager")

message(STATUS "Tests: Building network-manager first-use concurrency test "
               "(network_manager_concurrency_tests)")
