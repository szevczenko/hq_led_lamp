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
#   - `net` owns reusable network lifecycle behavior. No platform header or
#     type appears in network_manager.h.
#   - `lamp_control` is used for the immediate fail-off on the disconnect
#     path (Wi-Fi loss drives the output off before the application state
#     machine is informed).  Its header is not part of the adapter's public
#     contract, so it is private as well.
#   - `osal` supplies adapter logging; implementation-only and private.
# Product-domain modules depend only on include/network_manager.h.

idf_component_register(
    SRCS "network_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES hq_net lamp_control osal
)
