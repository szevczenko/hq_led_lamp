# Kitchen LED Controller main application component.
#
# app_main owns the boot order (TASK-107):
#   1. lamp_control_init() — output off,
#   2. lamp_fs_init() — mount LittleFS via OSAL, create /cert /config /state,
#   3. configuration loading only after a successful filesystem bootstrap.
#
# Kconfig-derived values (CONFIG_KLC_LED_PWM_GPIO etc.) come from the
# IDF-generated sdkconfig.h, which is on every component's include path.

idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES lamp_control lamp_fs app_config
)
