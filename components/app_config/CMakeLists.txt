# app_config — versioned product configuration service (TASK-108)
#
# ESP-IDF component registration.  The component implements validated,
# recoverable, atomically-replaced product documents (/config/device.json
# and /config/manufacturing.json) over the portable OSAL filesystem
# contract, with cJSON as the JSON codec.  It never persists lamp state:
# ThingsBoard stays the desired-state authority in release 1.

idf_component_register(
    SRCS "app_config.c"
    INCLUDE_DIRS "include"
    REQUIRES osal json
)
