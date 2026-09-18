# wifi_provisioning_manager — product-owned Wi-Fi provisioning adapter
# (TASK-126)
#
# ESP-IDF component registration.  The adapter narrows the platform
# provisioning application (wifi_http_provisioning.h) plus the automatic
# fallback controller (wifi_provisioning_controller.h, TASK-131/132), the
# shared Mongoose process and the Wi-Fi manager saved-credentials query down
# to the documented product contract: init/deinit, idempotent serialized
# start/stop (stop also ends the controller lifecycle — grace timer cancelled,
# temporary AP retired), a portal state query, an is-portal-active query, the
# has-saved-credentials provisioning decision and the controller outcome
# event queue (STARTED/SUCCEEDED/FAILED, consumed by the TASK-133
# supervisor via wifi_provisioning_manager_poll_event()).
#
# Dependency policy — every platform dependency is PRIVATE:
#   - `wifi_provisioning` provides the platform provisioning application
#     (start/stop/get_state and the test-only URL overrides); its platform
#     state types and listen-URL machinery must not reach product-domain
#     dependents, so it is private.
#   - `wifi` (the platform Wi-Fi manager) supplies the saved-credentials
#     query (wifi_mgmt_is_read_data); its credential/schema internals must
#     not leak, so it is private.
#   - `mongoose` supplies the shared Mongoose process query
#     (MongooseProcess_IsRunning); the Mongoose types must not leak, so it
#     is private.
#   - `osal` supplies the adapter lock and logging; implementation-only.
# Product-domain modules depend only on include/wifi_provisioning_manager.h,
# which contains no Wi-Fi, Mongoose or provisioning platform type.

idf_component_register(
    SRCS "wifi_provisioning_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES wifi mongoose osal wifi_provisioning
)