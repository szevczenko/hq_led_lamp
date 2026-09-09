# lamp_fs host unit tests (TASK-107)
#
# Standalone host test project for the filesystem bootstrap component
# (components/lamp_fs).  It compiles the production source against a
# test-only OSAL filesystem double (osal_fs_mock.c) plus the vendored Unity
# framework and runs as a normal CTest executable on the host.
#
# Usage:
#   cmake -S tests/lamp_fs -B <build-dir>
#   cmake --build <build-dir>
#   ctest --test-dir <build-dir> --output-on-failure
#   # or run the binary directly:
#   <build-dir>/lamp_fs_tests

cmake_minimum_required(VERSION 3.16)
project(hq_led_lamp_lamp_fs_tests LANGUAGES C)

set(LAMP_FS_DIR      ${CMAKE_CURRENT_LIST_DIR}/../../components/lamp_fs)
set(OSAL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal/include)
set(OSAL_IMPL_DIR    ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal/posix)
set(LAMP_CONTROL_UNITY_DIR ${CMAKE_CURRENT_LIST_DIR}/../lamp_control/unity)

add_executable(lamp_fs_tests
    ${CMAKE_CURRENT_LIST_DIR}/lamp_fs_test.c
    ${CMAKE_CURRENT_LIST_DIR}/osal_fs_mock.c
    ${LAMP_FS_DIR}/lamp_fs.c
    ${LAMP_CONTROL_UNITY_DIR}/unity.c
)

target_include_directories(lamp_fs_tests PRIVATE
    ${LAMP_FS_DIR}/include
    ${OSAL_INCLUDE_DIR}
    ${OSAL_IMPL_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${LAMP_CONTROL_UNITY_DIR}
)

# Match the platform project's strict host-test flags where Unity tolerates
# them; Unity's own sources rarely trigger warnings on current GCC/Clang.
target_compile_options(lamp_fs_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)

enable_testing()
add_test(NAME lamp_fs_tests COMMAND lamp_fs_tests)
set_tests_properties(lamp_fs_tests PROPERTIES LABELS "unit;lamp_fs")

message(STATUS "Tests: Building filesystem bootstrap host test (lamp_fs_tests)")
