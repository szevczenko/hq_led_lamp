#include "app_state.h"

#include <stddef.h>

#include "lamp_control.h"
#include "osal_log.h"

static void app_state_safe(hq_app_state_t state,
                           hq_app_state_event_t event,
                           hq_app_transition_owner_t owner,
                           uint32_t session)
{
    lamp_status_t status;

    (void)state;
    (void)event;
    (void)owner;
    (void)session;
    status = lamp_control_force_inactive();
    if (status != LAMP_OK)
    {
        osal_log_error("[app_state] fail-off could not force output inactive: %d",
                       (int)status);
    }
}

app_state_status_t app_state_init(const app_state_config_t *config)
{
    hq_app_state_config_t platform_config;

    if (config == NULL)
    {
        return APP_STATE_ERR_INVALID_ARGUMENT;
    }

    platform_config.now_ms = config->now_ms;
    platform_config.retry_initial_delay_ms = config->retry_initial_delay_ms;
    platform_config.retry_max_delay_ms = config->retry_max_delay_ms;
    platform_config.retry_max_attempts = config->retry_max_attempts;
    platform_config.retry_backoff_factor = config->retry_backoff_factor;
    platform_config.watchdog_timeout_ms = config->watchdog_timeout_ms;
    platform_config.on_watchdog_expired = config->on_watchdog_expired;
    platform_config.on_state_enter = NULL;
    platform_config.on_safe_state = app_state_safe;
    platform_config.observer = config->observer;

    return hq_app_state_init(&platform_config);
}

void app_state_deinit(void)
{
    hq_app_state_deinit();
}

app_state_status_t app_state_start(void)
{
    return hq_app_state_start();
}

app_state_status_t app_state_deliver(app_state_event_t event,
                                     app_transition_owner_t owner)
{
    return hq_app_state_deliver(event, owner);
}

app_state_status_t app_state_deliver_session(app_state_event_t event,
                                             app_transition_owner_t owner,
                                             uint32_t session)
{
    return hq_app_state_deliver_session(event, owner, session);
}

app_state_status_t app_state_poll(void)
{
    return hq_app_state_poll();
}

app_state_status_t app_state_poll_at(uint32_t now_ms)
{
    return hq_app_state_poll_at(now_ms);
}

app_state_t app_state_current(void)
{
    return hq_app_state_current();
}

uint32_t app_state_session(void)
{
    return hq_app_state_session();
}

app_transition_owner_t app_state_last_owner(void)
{
    return hq_app_state_last_owner();
}

bool app_state_is_online(void)
{
    return hq_app_state_is_online();
}

bool app_state_retry_pending(void)
{
    return hq_app_state_retry_pending();
}

uint32_t app_state_retry_delay_ms(void)
{
    return hq_app_state_retry_delay_ms();
}

uint32_t app_state_retry_attempts_used(void)
{
    return hq_app_state_retry_attempts_used();
}

bool app_state_retry_exhausted(void)
{
    return hq_app_state_retry_exhausted();
}

uint32_t app_state_stale_dropped(void)
{
    return hq_app_state_stale_dropped();
}

uint32_t app_state_invalid_dropped(void)
{
    return hq_app_state_invalid_dropped();
}