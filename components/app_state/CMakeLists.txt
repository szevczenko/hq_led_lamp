# app_state — application state machine and watchdog policy (TASK-115)
#
# ESP-IDF component registration.  The component owns the explicit
# application state machine (boot -> safe-off -> filesystem -> configuration
# -> Wi-Fi -> verified MQTT/TLS -> state sync -> online, with safe-off/fatal/
# OTA error states), the legal-transition/owner table, stale-callback
# (generation/session) rejection, the bounded retry/backoff schedule and the
# application watchdog policy (single polling owner, deadline refreshed by
# poll() and by every successful transition).
#
# Dependency policy:
#   - `lamp_control` supplies the fail-off invariant (the output is forced
#     inactive on every non-online entry).  Private: no lamp type appears in
#     the public header.
#   - `osal` supplies the mutex, logging and the default monotonic clock.
#     Implementation-only and private.
# Product-domain modules depend only on include/app_state.h.

idf_component_register(
    SRCS "app_state.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES lamp_control osal
)