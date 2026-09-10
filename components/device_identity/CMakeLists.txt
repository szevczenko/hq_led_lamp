# device_identity — access-token device identity (TASK-111)
#
# ESP-IDF component registration.  The component loads the validated
# ThingsBoard access token and stable client ID from the bounded identity
# record /config/identity.json, gated on the validated manufacturing
# record, and exposes them through the stable client-ID and token-provider
# interfaces.  Every load failure forces the lamp output inactive.
#
# Dependency policy:
#   - `app_config` (validated manufacturing/configuration records) is used
#     only by the load path; no app_config type appears in the public
#     header.
#   - `osal` (file I/O) and `json` (cJSON strict-schema parsing) are
#     implementation-only.
#   - `lamp_control` (fail-off) is used only by the implementation.
# All are private: product-domain modules depend only on
# include/device_identity.h.

idf_component_register(
    SRCS "device_identity.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES osal json app_config lamp_control
)