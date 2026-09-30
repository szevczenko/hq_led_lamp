# ota_manager — ThingsBoard OTA supervisor (firmware update lifecycle,
# check triggers, progress indicator, deferred restart).
#
# `thingsboard` supplies tb_firmware_update; `osal` the clock and logging.
# Both are private: the public header only forward-declares tb_client_t.

idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES thingsboard osal
)
