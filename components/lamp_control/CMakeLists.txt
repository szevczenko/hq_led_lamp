# lamp_control — hardware-independent LED lamp state policy (TASK-106)
#
# ESP-IDF component registration.  The component implements the validated
# lamp policy on top of the portable HAL PWM contract and therefore requires
# the `hq_hal` component (src/hal in the hq_platform submodule), which
# selects the target backend (ESP-IDF LEDC on board, POSIX simulation on the
# host).  Nothing in this component references ESP-IDF LEDC/GPIO directly.

idf_component_register(
    SRCS "lamp_control.c"
    INCLUDE_DIRS "include"
    REQUIRES hq_hal
)