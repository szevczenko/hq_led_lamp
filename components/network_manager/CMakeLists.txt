# network_manager — product-owned adapter around the platform Wi-Fi manager
# (TASK-109)
#
# ESP-IDF component registration.  The adapter narrows the platform Wi-Fi
# manager down to the documented network_callbacks_t product contract:
# connected/disconnected callbacks, a connected-state query and a blocking
# connection gate that orders Wi-Fi onboarding before ThingsBoard connect
# attempts.
#
# Dependency policy:
#   - `wifi` (the platform Wi-Fi manager) is the ONLY source of Wi-Fi
#     knowledge; its persistence internals (wifi_config, credential schema)
#     never leak through this component's public header.
#   - `lamp_control` is used for the immediate fail-off on the disconnect
#     path (Wi-Fi loss drives the output off before the application state
#     machine is informed).
#   - `osal` supplies the synchronization primitives (adapter lock, wait
#     polling) and logging.
# Product-domain modules depend only on include/network_manager.h.

idf_component_register(
    SRCS "network_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES wifi lamp_control osal
)
