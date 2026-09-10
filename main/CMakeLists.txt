# Kitchen LED Controller main application component.
#
# app_main owns the boot order (TASK-107 / TASK-115):
#   1. lamp_control_init() — output off,
#   2. the application state machine + watchdog (app_state, TASK-115)
#      starts next; the supervisor loop is the single watchdog feeder,
#   3. lamp_fs_init() — mount LittleFS via OSAL, create /cert /config /state,
#   4. configuration loading only after a successful filesystem bootstrap,
#   5. device identity (TASK-111) after configuration validation and before
#      any ThingsBoard initialization,
#   6. Wi-Fi onboarding and the ThingsBoard gates (TASK-109/110/111) behind
#      the machine's NETWORK/TLS states (the consumed
#      network_manager_is_connected() gate loop).
#
# `app_state` supplies the state machine and `osal` the supervisor's
# sleep cadence (osal_task_delay_ms).  Kconfig-derived values
# (CONFIG_KLC_LED_PWM_GPIO etc.) come from the IDF-generated sdkconfig.h,
# which is on every component's include path.

idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES lamp_control lamp_fs app_config network_manager mqtt_cfg
             device_identity app_state osal
)
