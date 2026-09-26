# ota_manager host tests
#
# Compiles components/ota_manager against a controllable tb_firmware_update
# double (tb_firmware_update_mock.c, which also stubs the two OSAL symbols
# the module uses) and Unity.
#
# Usage:
#   cmake -S tests/ota_manager -B <build-dir>
#   cmake --build <build-dir>
#   ctest --test-dir <build-dir> --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(hq_led_lamp_ota_manager_tests LANGUAGES C)

set(OTA_DIR          ${CMAKE_CURRENT_LIST_DIR}/../../components/ota_manager)
set(TB_INCLUDE_DIR   ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/thingsboard/include)
set(OSAL_DIR         ${CMAKE_CURRENT_LIST_DIR}/../../platform/hq_platform/src/osal)
set(UNITY_DIR        ${CMAKE_CURRENT_LIST_DIR}/../lamp_control/unity)

add_executable(ota_manager_tests
    ${CMAKE_CURRENT_LIST_DIR}/ota_manager_test.c
    ${CMAKE_CURRENT_LIST_DIR}/tb_firmware_update_mock.c
    ${OTA_DIR}/ota_manager.c
    ${UNITY_DIR}/unity.c
)

target_include_directories(ota_manager_tests PRIVATE
    ${OTA_DIR}/include
    ${TB_INCLUDE_DIR}
    ${OSAL_DIR}/include
    ${OSAL_DIR}/posix
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${UNITY_DIR}
)

target_compile_options(ota_manager_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
set_source_files_properties(${UNITY_DIR}/unity.c PROPERTIES COMPILE_OPTIONS "-w")

enable_testing()
add_test(NAME ota_manager_tests COMMAND ota_manager_tests)
set_tests_properties(ota_manager_tests PROPERTIES LABELS "unit;ota_manager")
