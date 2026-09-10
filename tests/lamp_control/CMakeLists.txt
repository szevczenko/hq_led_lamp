# lamp_control host unit tests (TASK-106)
#
# Standalone host test project for the hardware-independent lamp-control
# component.  It compiles the production source (components/lamp_control)
# against a test-only HAL PWM double (hal_pwm_mock.c) plus the vendored
# Unity framework and runs as a normal CTest executable on the host.
#
# Usage:
#   cmake -S tests/lamp_control -B <build-dir>
#   cmake --build <build-dir>
#   ctest --test-dir <build-dir> --output-on-failure
#   # or run the binary directly:
#   <build-dir>/lamp_control_tests

cmake_minimum_required(VERSION 3.16)
project(hq_led_lamp_lamp_control_tests LANGUAGES C)

set(LAMP_CONTROL_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components/lamp_control)
set(HAL_INCLUDE_DIR  ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/hal/include)

find_package(Threads REQUIRED)

add_executable(lamp_control_tests
    ${CMAKE_CURRENT_LIST_DIR}/lamp_control_test.c
    ${CMAKE_CURRENT_LIST_DIR}/hal_pwm_mock.c
    ${LAMP_CONTROL_DIR}/lamp_control.c
    ${CMAKE_CURRENT_LIST_DIR}/unity/unity.c
)

target_include_directories(lamp_control_tests PRIVATE
    ${LAMP_CONTROL_DIR}/include
    ${HAL_INCLUDE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/unity
)

# Match the platform project's strict host-test flags where Unity tolerates
# them; Unity's own sources rarely trigger warnings on current GCC/Clang.
target_compile_options(lamp_control_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)

target_link_libraries(lamp_control_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME lamp_control_tests COMMAND lamp_control_tests)
set_tests_properties(lamp_control_tests PROPERTIES LABELS "unit;lamp_control")

message(STATUS "Tests: Building lamp-control host test (lamp_control_tests)")