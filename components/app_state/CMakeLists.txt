# app_state — application state machine and watchdog policy (TASK-115)
#
# Compatibility facade for the platform-owned application state service.
#
idf_component_register(
    SRCS "app_state.c"
    INCLUDE_DIRS "include"
    REQUIRES hq_app_services
    PRIV_REQUIRES lamp_control osal
)