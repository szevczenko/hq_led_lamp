# mqtt_cfg — MQTT/TLS broker configuration and verified-transport setup
# (TASK-110)
#
# ESP-IDF component registration.  The component loads and validates
# /config/mqtt.json (hostname, port, client ID, TLS mode, certificate
# paths, authentication mode) and configures the Mongoose/ThingsBoard
# verified transport exclusively through the platform mqtt_config API.
#
# Dependency policy:
#   - `mongoose` (the platform Mongoose configuration + MQTT application
#     layers) is PRIVATE: mqtt_cfg's public header exposes only bounded
#     product types, so mqtt_config/mqtt_app types must not propagate to
#     product-domain dependents.
#   - `osal` (file I/O + logging + task delay) is implementation-only.
#   - `json` (cJSON) is the strict-schema document codec; also private.
#   - `lamp_control` is used for the mandatory fail-off on every
#     configuration / TLS-connection failure; private as well.
#
# Product-domain modules depend only on include/mqtt_cfg.h.

idf_component_register(
    SRCS "mqtt_cfg.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mongoose osal json lamp_control
)