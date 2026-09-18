# network_manager host unit tests (TASK-109)
#
# Standalone host test project for the product-owned network adapter
# (components/network_manager).  It compiles the production source against
# test-only doubles — the Wi-Fi manager double (wifi_mgmt_mock.c), the OSAL
# doubles (osal_test_support.c) and the lamp-control double
# (lamp_control_mock.c) — plus the vendored Unity framework, and runs as a
# normal CTest executable on the host.
#
# Coverage: connect, disconnect (fail-off ordering), invalid credentials,
# reconnect, stale/late callbacks after stop, start/stop lifecycle contract,
# the mode policy (TASK-130: the default build requests the Kconfig-selected
# AP+STA mode BEFORE wifi_mgmt_start(), callbacks fire identically in AP+STA,
# and a concurrent stop during start still disarms the session regardless of
# the mode) and the credentials-never-logged secrecy rule.
#
# Usage:
#   cmake -S tests/network_manager -B <build-dir>
#   cmake --build <build-dir>
#   ctest --test-dir <build-dir> --output-on-failure
#   # or run the binary directly:
#   <build-dir>/network_manager_tests

cmake_minimum_required(VERSION 3.16)
project(hq_led_lamp_network_manager_tests LANGUAGES C)

set(NETWORK_MANAGER_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components/network_manager)
set(LAMP_CONTROL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components/lamp_control/include)
set(HAL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/hal/include)
set(OSAL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal/include)
set(OSAL_IMPL_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal/posix)
set(WIFI_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/wifi)
set(LAMP_CONTROL_UNITY_DIR ${CMAKE_CURRENT_LIST_DIR}/../lamp_control/unity)

find_package(Threads REQUIRED)

add_executable(network_manager_tests
    ${CMAKE_CURRENT_LIST_DIR}/network_manager_test.c
    ${CMAKE_CURRENT_LIST_DIR}/wifi_mgmt_mock.c
    ${CMAKE_CURRENT_LIST_DIR}/osal_test_support.c
    ${CMAKE_CURRENT_LIST_DIR}/lamp_control_mock.c
    ${NETWORK_MANAGER_DIR}/network_manager.c
    ${LAMP_CONTROL_UNITY_DIR}/unity.c
)

target_include_directories(network_manager_tests PRIVATE
    ${NETWORK_MANAGER_DIR}/include
    ${LAMP_CONTROL_INCLUDE_DIR}
    ${HAL_INCLUDE_DIR}
    ${OSAL_INCLUDE_DIR}
    ${OSAL_IMPL_DIR}
    ${WIFI_INCLUDE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${LAMP_CONTROL_UNITY_DIR}
)

# The adapter's Wi-Fi start mode is a product policy owned by Kconfig
# (KLC_WIFI_DEFAULT_MODE, main/Kconfig.projbuild): the network adapter
# compiles the Kconfig-selected mode into network_manager_start().  The host
# test build mirrors the product DEFAULT (AP+STA, T_WIFI_TYPE_CLI_SER — the
# provisioning portal's soft-AP must be available), which is exactly what
# the mode-policy cases (TASK-130) assert.  Keep this in lockstep with the
# project default in main/Kconfig.projbuild.
target_compile_definitions(network_manager_tests PRIVATE
    CONFIG_KLC_WIFI_DEFAULT_MODE_APSTA=1
)

# Match the platform project's strict host-test flags where Unity tolerates
# them; Unity's own sources rarely trigger warnings on current GCC/Clang.
target_compile_options(network_manager_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)

target_link_libraries(network_manager_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME network_manager_tests COMMAND network_manager_tests)
set_tests_properties(network_manager_tests PROPERTIES LABELS "unit;network_manager")

message(STATUS "Tests: Building network-manager host test (network_manager_tests)")
