#ifndef APP_STATE_H
#define APP_STATE_H

#include "hq_app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef hq_app_state_status_t app_state_status_t;
typedef hq_app_state_t app_state_t;
typedef hq_app_state_event_t app_state_event_t;
typedef hq_app_transition_owner_t app_transition_owner_t;
typedef hq_app_failure_class_t app_failure_class_t;
typedef hq_app_state_now_fn_t app_state_now_fn_t;
typedef hq_app_state_watchdog_fn_t app_state_watchdog_fn_t;
typedef hq_app_state_observer_fn_t app_state_observer_fn_t;

#define APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS HQ_APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS
#define APP_STATE_RETRY_MAX_DELAY_DEFAULT_MS HQ_APP_STATE_RETRY_MAX_DELAY_DEFAULT_MS
#define APP_STATE_RETRY_MAX_ATTEMPTS_DEFAULT HQ_APP_STATE_RETRY_MAX_ATTEMPTS_DEFAULT
#define APP_STATE_RETRY_BACKOFF_FACTOR_DEFAULT HQ_APP_STATE_RETRY_BACKOFF_FACTOR_DEFAULT
#define APP_STATE_RETRY_DELAY_MIN_MS HQ_APP_STATE_RETRY_DELAY_MIN_MS
#define APP_STATE_RETRY_DELAY_MAX_MS HQ_APP_STATE_RETRY_DELAY_MAX_MS
#define APP_STATE_WATCHDOG_TIMEOUT_DEFAULT_MS HQ_APP_STATE_WATCHDOG_TIMEOUT_DEFAULT_MS
#define APP_STATE_WATCHDOG_TIMEOUT_MIN_MS HQ_APP_STATE_WATCHDOG_TIMEOUT_MIN_MS
#define APP_STATE_WATCHDOG_TIMEOUT_MAX_MS HQ_APP_STATE_WATCHDOG_TIMEOUT_MAX_MS

#define APP_STATE_OK HQ_APP_STATE_OK
#define APP_STATE_ERR_INVALID_ARGUMENT HQ_APP_STATE_ERR_INVALID_ARGUMENT
#define APP_STATE_ERR_NOT_INITIALIZED HQ_APP_STATE_ERR_NOT_INITIALIZED
#define APP_STATE_ERR_ALREADY_INITIALIZED HQ_APP_STATE_ERR_ALREADY_INITIALIZED
#define APP_STATE_ERR_STATE HQ_APP_STATE_ERR_STATE
#define APP_STATE_ERR_STALE HQ_APP_STATE_ERR_STALE
#define APP_STATE_ERR_ILLEGAL HQ_APP_STATE_ERR_ILLEGAL
#define APP_STATE_ERR_WATCHDOG HQ_APP_STATE_ERR_WATCHDOG

#define APP_STATE_BOOT HQ_APP_STATE_BOOT
#define APP_STATE_FILESYSTEM HQ_APP_STATE_FILESYSTEM
#define APP_STATE_CONFIGURATION HQ_APP_STATE_CONFIGURATION
#define APP_STATE_NETWORK HQ_APP_STATE_NETWORK
#define APP_STATE_PROVISIONING HQ_APP_STATE_PROVISIONING
#define APP_STATE_TLS HQ_APP_STATE_TLS
#define APP_STATE_SYNC HQ_APP_STATE_SYNC
#define APP_STATE_ONLINE HQ_APP_STATE_ONLINE
#define APP_STATE_SAFE_OFF HQ_APP_STATE_SAFE_OFF
#define APP_STATE_FATAL HQ_APP_STATE_FATAL
#define APP_STATE_OTA HQ_APP_STATE_OTA

#define APP_EVENT_START HQ_APP_EVENT_START
#define APP_EVENT_FS_OK HQ_APP_EVENT_FS_OK
#define APP_EVENT_FS_FAIL HQ_APP_EVENT_FS_FAIL
#define APP_EVENT_CONFIG_OK HQ_APP_EVENT_CONFIG_OK
#define APP_EVENT_CONFIG_FAIL HQ_APP_EVENT_CONFIG_FAIL
#define APP_EVENT_NETWORK_CONNECTED HQ_APP_EVENT_NETWORK_CONNECTED
#define APP_EVENT_NETWORK_FAILED HQ_APP_EVENT_NETWORK_FAILED
#define APP_EVENT_PROVISIONING_STARTED HQ_APP_EVENT_PROVISIONING_STARTED
#define APP_EVENT_PROVISIONING_SUCCEEDED HQ_APP_EVENT_PROVISIONING_SUCCEEDED
#define APP_EVENT_PROVISIONING_FAILED HQ_APP_EVENT_PROVISIONING_FAILED
#define APP_EVENT_TLS_CONNECTED HQ_APP_EVENT_TLS_CONNECTED
#define APP_EVENT_TLS_FAILED HQ_APP_EVENT_TLS_FAILED
#define APP_EVENT_SYNC_COMPLETE HQ_APP_EVENT_SYNC_COMPLETE
#define APP_EVENT_SYNC_FAILED HQ_APP_EVENT_SYNC_FAILED
#define APP_EVENT_DISCONNECTED HQ_APP_EVENT_DISCONNECTED
#define APP_EVENT_INVALID_STATE HQ_APP_EVENT_INVALID_STATE
#define APP_EVENT_OTA_BEGIN HQ_APP_EVENT_OTA_BEGIN
#define APP_EVENT_OTA_END HQ_APP_EVENT_OTA_END
#define APP_EVENT_OTA_FAILED HQ_APP_EVENT_OTA_FAILED
#define APP_EVENT_RETRY_DUE HQ_APP_EVENT_RETRY_DUE
#define APP_EVENT_RESET HQ_APP_EVENT_RESET
#define APP_EVENT_FATAL HQ_APP_EVENT_FATAL

#define APP_OWNER_BOOTSTRAP HQ_APP_OWNER_BOOTSTRAP
#define APP_OWNER_FILESYSTEM HQ_APP_OWNER_FILESYSTEM
#define APP_OWNER_CONFIGURATION HQ_APP_OWNER_CONFIGURATION
#define APP_OWNER_NETWORK HQ_APP_OWNER_NETWORK
#define APP_OWNER_MQTT HQ_APP_OWNER_MQTT
#define APP_OWNER_THINGSBOARD HQ_APP_OWNER_THINGSBOARD
#define APP_OWNER_TIMER HQ_APP_OWNER_TIMER
#define APP_OWNER_OTA HQ_APP_OWNER_OTA
#define APP_OWNER_WATCHDOG HQ_APP_OWNER_WATCHDOG
#define APP_OWNER_EXTERNAL HQ_APP_OWNER_EXTERNAL

#define APP_FAILURE_RETRYABLE HQ_APP_FAILURE_RETRYABLE
#define APP_FAILURE_DEGRADED HQ_APP_FAILURE_DEGRADED
#define APP_FAILURE_FATAL HQ_APP_FAILURE_FATAL

typedef struct app_state_config {
    app_state_now_fn_t now_ms;
    uint32_t retry_initial_delay_ms;
    uint32_t retry_max_delay_ms;
    uint32_t retry_max_attempts;
    uint32_t retry_backoff_factor;
    uint32_t watchdog_timeout_ms;
    app_state_watchdog_fn_t on_watchdog_expired;
    app_state_observer_fn_t observer;
} app_state_config_t;

app_state_status_t app_state_init(const app_state_config_t *config);
void app_state_deinit(void);
app_state_status_t app_state_start(void);
app_state_status_t app_state_deliver(app_state_event_t event,
                                     app_transition_owner_t owner);
app_state_status_t app_state_deliver_session(app_state_event_t event,
                                             app_transition_owner_t owner,
                                             uint32_t session);
app_state_status_t app_state_poll(void);
app_state_status_t app_state_poll_at(uint32_t now_ms);
app_state_t app_state_current(void);
uint32_t app_state_session(void);
app_transition_owner_t app_state_last_owner(void);
bool app_state_is_online(void);
bool app_state_retry_pending(void);
uint32_t app_state_retry_delay_ms(void);
uint32_t app_state_retry_attempts_used(void);
bool app_state_retry_exhausted(void);
uint32_t app_state_stale_dropped(void);
uint32_t app_state_invalid_dropped(void);

#ifdef __cplusplus
}
#endif

#endif