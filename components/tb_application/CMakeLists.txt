# tb_application — ThingsBoard desired-state synchronization application
# logic (TASK-112), server-side RPC control (TASK-113) and telemetry/health
# reporting (TASK-114)
#
# ESP-IDF component registration.  The component owns the product policy
# "shared attributes power/brightness are authoritative": on every
# successful connection it subscribes to shared-attribute updates and
# requests both attributes as one synchronization operation through the
# existing platform ThingsBoard primitives, validates complete responses
# (bounded payloads, JSON types, range, session identity), applies only
# complete valid states, rejects partial/duplicate/stale/late data and
# re-synchronizes with bounded retry/backoff after reconnect.  Every
# rejection path and every disconnect forces the lamp output inactive
# (fail-off).  It also arms the server-side RPC control surface and
# publishes the documented health telemetry record on connect, on every
# successful state change and periodically (rate-limited and suppressed
# while disconnected, serialized with a bounded buffer and secret-free).
#
# Dependency policy:
#   - `thingsboard` (the platform ThingsBoard client/attributes primitives)
#     is PRIVATE: the public header only forward-declares the opaque
#     tb_client_t handle and exposes the connection-glue contract, so no
#     platform type reaches product-domain dependents.
#   - `lamp_control` is used for the mandatory fail-off / apply policy;
#     also private (its header is not part of this component's public API).
#   - `osal` (mutex + monotonic clock + logging) is implementation-only.
#   - `json` (cJSON) is the strict payload codec; private as well.
#
# Product-domain modules depend only on include/tb_application.h.

idf_component_register(
    SRCS "tb_application.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES thingsboard lamp_control osal json
)