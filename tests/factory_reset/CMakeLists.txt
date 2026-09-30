cmake_minimum_required(VERSION 3.16)
project(factory_reset_tests LANGUAGES C)

add_executable(factory_reset_tests
    factory_reset_test.c
    hal_gpio_mock.c
    ../../components/factory_reset/factory_reset.c
    ../../platform/hq_platform/src/hq_input/hq_button.c
    ../../platform/hq_platform/src/hq_input/hq_factory_reset.c
    ../lamp_control/unity/unity.c
)
target_include_directories(factory_reset_tests PRIVATE
    ../../components/factory_reset/include
    ../../platform/hq_platform/src/hq_input/include
    ../../platform/hq_platform/src/hal/include
    ../lamp_control/unity
)
target_compile_options(factory_reset_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
enable_testing()
add_test(NAME factory_reset_tests COMMAND factory_reset_tests)
