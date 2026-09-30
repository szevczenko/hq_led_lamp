cmake_minimum_required(VERSION 3.16)

get_filename_component(HQ_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/platform/hq_platform" ABSOLUTE)

set(HQ_DEFCONFIG "${HQ_REPO_ROOT}/defconfig/esp.defconfig")

set(EXTRA_COMPONENT_DIRS
    ${HQ_REPO_ROOT}/src/osal
    ${HQ_REPO_ROOT}/src/hq_storage
    ${HQ_REPO_ROOT}/src/hq_app_services
    ${HQ_REPO_ROOT}/src/hq_input
    ${HQ_REPO_ROOT}/src/hal/hq_hal
    ${HQ_REPO_ROOT}/src/thingsboard
    ${HQ_REPO_ROOT}/src/mongoose
    ${HQ_REPO_ROOT}/src/wifi
    ${HQ_REPO_ROOT}/src/hq_net
    ${HQ_REPO_ROOT}/src/wifi_provisioning
    ${CMAKE_CURRENT_LIST_DIR}/components
)

include(${HQ_REPO_ROOT}/cmake/configure.cmake)
include(${HQ_REPO_ROOT}/cmake/esp.cmake)
include(${HQ_REPO_ROOT}/cmake/modules.cmake)

set(KLC_WIFI_AP_NAME "$ENV{KLC_WIFI_AP_NAME}" CACHE STRING
    "Provisioning AP SSID for this device build")
set(KLC_WIFI_AP_PASSWORD "$ENV{KLC_WIFI_AP_PASSWORD}" CACHE STRING
    "Provisioning AP password for this device build")

add_compile_definitions(
    WIFI_AP_NAME="${KLC_WIFI_AP_NAME}"
    WIFI_AP_PASSWORD="${KLC_WIFI_AP_PASSWORD}"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kitchen_led_controller)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/klc_validate.cmake)
