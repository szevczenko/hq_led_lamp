/**
 * @file app_state.c
 * @brief Application state machine and watchdog policy (TASK-115)
 *
 * See app_state.h for the normative contract.  Implementation notes:
 *
 *   - the legal-transition table is data (a static array) and every
 *     delivery validates event, owner AND session against the machine's
 *     current state before any transition is performed; everything that
 *     does not match is dropped and counted,
 *   - the fail-off invariant ("the output is forced inactive on every
 *     non-online entry") is implemented inside the transition helper, so
 *     no caller can bypass it,
 *   - the retry schedule is a single (attempt counter, delay, deadline)
 *     triple driven by poll(); the delay grows exponentially
 *     (retry_delay_ms * factor, capped) and parks when the per-episode
 *     budget is spent, which bounds the reconnect storm,
 *   - the watchdog is a single deadline refreshed by poll() (the one feed
 *     point) and by every successful transition; an expired deadline is
 *     only observed on the next poll and always ends in
 *     #APP_STATE_FATAL — never in a spontaneous resume,
 *   - lock discipline: one module mutex guards every public operation;
 *     the optional observer and watchdog callbacks run with the lock held
 *     (short, non-blocking, no API re-entry — same contract as the
 *     network adapter's callbacks).
 */

#include "app_state.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "lamp_control.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_task.h"

/* --------------------------------------------------------------------- */
/* Module state                                                           */
/* --------------------------------------------------------------------- */

/* Sentinel used for "no retry target" table slots; must not match a
 * real state (real states are 0..10, so -1 is safe). */
#define APP_STATE_NONE_SAFE ((app_state_t)-1)

/** @brief One legal transition of the machine. */
typedef struct app_state_transition {
    app_state_t           from;          /**< Source state. */
    app_state_event_t     event;         /**< Event that fires it. */
    app_state_t           to;            /**< Destination state. */
    app_transition_owner_t owner;        /**< REQUIRED transition owner. */
    app_failure_class_t   fcls;          /**< Failure class (failures only). */
    app_state_t           retry_target;  /**< Retry re-entry state (retryable only). */
    bool                  new_episode;   /**< Bump the session/generation. */
} app_state_transition_t;

/**
 * @brief Legal transitions (TASK-115 table; see app_state.h).
 *
 * The special transitions are handled before the table:
 *   - start():      BOOT -> FILESYSTEM (owner BOOTSTRAP),
 *   - RESET:        any state -> BOOT (owner EXTERNAL; new episode),
 *   - FATAL:        any state -> FATAL (any owner; always honored),
 *   - RETRY_DUE:    SAFE_OFF -> retry target (owner TIMER; new episode).
 *
 * The DISCONNECTED rows below are encoded with owner APP_OWNER_MQTT;
 * app_owner_ok() deliberately widens DISCONNECT-class rows to ALSO accept
 * APP_OWNER_NETWORK (both transports may report "transport down" — see the
 * header table, which documents those rows as owned by NETWORK/MQTT).
 *
 * The PROVISIONING rows are owned by APP_OWNER_NETWORK: the provisioning
 * adapter belongs to the network stage, so NETWORK may enter PROVISIONING
 * on PROVISIONING_STARTED (no saved credential) and PROVISIONING returns
 * to NETWORK on PROVISIONING_SUCCEEDED (credential saved) or degrades to
 * SAFE_OFF on PROVISIONING_FAILED with the bounded retry target NETWORK
 * (the retry re-enters the network stage, never PROVISIONING directly).
 */
static const app_state_transition_t APP_STATE_TRANSITIONS[] = {
    { APP_STATE_FILESYSTEM,    APP_EVENT_FS_OK,             APP_STATE_CONFIGURATION, APP_OWNER_FILESYSTEM,    APP_FAILURE_RETRYABLE, APP_STATE_NONE_SAFE,          false },
    { APP_STATE_FILESYSTEM,    APP_EVENT_FS_FAIL,           APP_STATE_SAFE_OFF,      APP_OWNER_FILESYSTEM,    APP_FAILURE_DEGRADED,  APP_STATE_NONE_SAFE,          false },
    { APP_STATE_FILESYSTEM,    APP_EVENT_DISCONNECTED,      APP_STATE_SAFE_OFF,      APP_OWNER_MQTT,          APP_FAILURE_RETRYABLE, APP_STATE_NETWORK,           false },
    { APP_STATE_CONFIGURATION, APP_EVENT_CONFIG_OK,         APP_STATE_NETWORK,       APP_OWNER_CONFIGURATION, APP_FAILURE_RETRYABLE, APP_STATE_NONE_SAFE,          false },
    { APP_STATE_CONFIGURATION, APP_EVENT_CONFIG_FAIL,       APP_STATE_SAFE_OFF,      APP_OWNER_CONFIGURATION, APP_FAILURE_DEGRADED,  APP_STATE_NONE_SAFE,          false },
    { APP_STATE_CONFIGURATION, APP_EVENT_DISCONNECTED,      APP_STATE_SAFE_OFF,      APP_OWNER_MQTT,          APP_FAILURE_RETRYABLE, APP_STATE_NETWORK,           false },
    { APP_STATE_NETWORK,       APP_EVENT_NETWORK_CONNECTED, APP_STATE_TLS,           APP_OWNER_NETWORK,       APP_FAILURE_RETRYABLE, APP_STATE_NONE_SAFE,          false },
    { APP_STATE_NETWORK,       APP_EVENT_NETWORK_FAILED,    APP_STATE_SAFE_OFF,      APP_OWNER_NETWORK,       APP_FAILURE_RETRYABLE, APP_STATE_NETWORK,           false },
    { APP_STATE_NETWORK,       APP_EVENT_PROVISIONING_STARTED,   APP_STATE_PROVISIONING, APP_OWNER_NETWORK,  APP_FAILURE_RETRYABLE, APP_STATE_NONE_SAFE,          false },
    { APP_STATE_PROVISIONING,  APP_EVENT_PROVISIONING_SUCCEEDED, APP_STATE_NETWORK,      APP_OWNER_NETWORK,  APP_FAILURE_RETRYABLE, APP_STATE_NONE_SAFE,          false },
    { APP_STATE_PROVISIONING,  APP_EVENT_PROVISIONING_FAILED,    APP_STATE_SAFE_OFF,     APP_OWNER_NETWORK,  APP_FAILURE_RETRYABLE, APP_STATE_NETWORK,           false },
    { APP_STATE_NETWORK,       APP_EVENT_DISCONNECTED,      APP_STATE_SAFE_OFF,      APP_OWNER_MQTT,          APP_FAILURE_RETRYABLE, APP_STATE_NETWORK,           false },
    { APP_STATE_TLS,           APP_EVENT_TLS_CONNECTED,     APP_STATE_SYNC,          APP_OWNER_MQTT,          APP_FAILURE_RETRYABLE, APP_STATE_NONE_SAFE,          false },
    { APP_STATE_TLS,           APP_EVENT_TLS_FAILED,        APP_STATE_SAFE_OFF,      APP_OWNER_MQTT,          APP_FAILURE_RETRYABLE, APP_STATE_TLS,               false },
    { APP_STATE_TLS,           APP_EVENT_DISCONNECTED,      APP_STATE_SAFE_OFF,      APP_OWNER_MQTT,          APP_FAILURE_RETRYABLE, APP_STATE_NETWORK,           false },
    { APP_STATE_SYNC,          APP_EVENT_SYNC_COMPLETE,     APP_STATE_ONLINE,        APP_OWNER_THINGSBOARD,   APP_FAILURE_RETRYABLE, APP_STATE_NONE_SAFE,          false },
    { APP_STATE_SYNC,          APP_EVENT_SYNC_FAILED,       APP_STATE_SAFE_OFF,      APP_OWNER_THINGSBOARD,   APP_FAILURE_RETRYABLE, APP_STATE_SYNC,              false },
    { APP_STATE_SYNC,          APP_EVENT_INVALID_STATE,     APP_STATE_SAFE_OFF,      APP_OWNER_THINGSBOARD,   APP_FAILURE_RETRYABLE, APP_STATE_SYNC,              false },
    { APP_STATE_SYNC,          APP_EVENT_DISCONNECTED,      APP_STATE_SAFE_OFF,      APP_OWNER_MQTT,          APP_FAILURE_RETRYABLE, APP_STATE_NETWORK,           false },
    { APP_STATE_ONLINE,        APP_EVENT_DISCONNECTED,      APP_STATE_SAFE_OFF,      APP_OWNER_MQTT,          APP_FAILURE_RETRYABLE, APP_STATE_NETWORK,           false },
    { APP_STATE_ONLINE,        APP_EVENT_INVALID_STATE,     APP_STATE_SAFE_OFF,      APP_OWNER_THINGSBOARD,   APP_FAILURE_RETRYABLE, APP_STATE_SYNC,              false },
    { APP_STATE_ONLINE,        APP_EVENT_OTA_BEGIN,         APP_STATE_OTA,           APP_OWNER_OTA,           APP_FAILURE_RETRYABLE, APP_STATE_NONE_SAFE,          true  },
    { APP_STATE_SAFE_OFF,      APP_EVENT_OTA_BEGIN,         APP_STATE_OTA,           APP_OWNER_OTA,           APP_FAILURE_RETRYABLE, APP_STATE_NONE_SAFE,          true  },
    { APP_STATE_OTA,           APP_EVENT_OTA_END,           APP_STATE_BOOT,          APP_OWNER_OTA,           APP_FAILURE_RETRYABLE, APP_STATE_NONE_SAFE,          true  },
    { APP_STATE_OTA,           APP_EVENT_OTA_FAILED,        APP_STATE_SAFE_OFF,      APP_OWNER_OTA,           APP_FAILURE_DEGRADED,  APP_STATE_NONE_SAFE,          true  },
};

/** @brief Resolved module configuration (immutable after init). */
typedef struct app_state_runtime {
    app_state_now_fn_t       now_fn;
    uint32_t                 retry_initial_delay_ms;
    uint32_t                 retry_max_delay_ms;
    uint32_t                 retry_max_attempts;
    uint32_t                 retry_backoff_factor;
    uint32_t                 watchdog_timeout_ms;
    app_state_watchdog_fn_t  on_watchdog_expired;
    app_state_observer_fn_t  observer;
} app_state_runtime_t;

typedef struct app_state_internal {
    /* The module mutex is created lazily and adopted with a CAS (first-use
     * race, exactly like the network adapter's lock): the very first
     * init() publishes it, every later operation loads it atomically. */
    _Atomic(osal_mutex_id_t) lock;
    bool                   initialized;
    app_state_runtime_t    cfg;
    app_state_t            state;
    uint32_t               session;
    app_transition_owner_t last_owner;

    /* Watchdog */
    bool                   wdt_armed;
    uint32_t               deadline_ms;

    /* Retry schedule */
    bool                   retry_pending;
    uint32_t               retry_due_ms;
    uint32_t               retry_delay_ms;
    uint32_t               retry_attempts;
    bool                   retry_exhausted;
    app_state_t            retry_target;

    /* Drop counters */
    uint32_t               stale_dropped;
    uint32_t               invalid_dropped;
} app_state_internal_t;

static app_state_internal_t s_state = {
    .lock        = NULL,
    .initialized = false,
    .state       = APP_STATE_BOOT,
    .last_owner  = APP_OWNER_EXTERNAL,
};

/* --------------------------------------------------------------------- */
/* Small helpers                                                          */
/* --------------------------------------------------------------------- */

/** @brief Wrap-safe "now >= when" comparison (valid within a 2^31 ms
 *         window, i.e. for delays far below the 49.7-day clock wrap). */
static bool app_time_ge(uint32_t now, uint32_t when)
{
    return (int32_t)(now - when) >= 0;
}

static uint32_t app_now(void)
{
    return s_state.cfg.now_fn();
}

static bool app_state_is_disconnect_class(app_state_event_t event)
{
    switch (event)
    {
    case APP_EVENT_FS_FAIL:
    case APP_EVENT_CONFIG_FAIL:
    case APP_EVENT_NETWORK_FAILED:
    case APP_EVENT_PROVISIONING_FAILED:
    case APP_EVENT_TLS_FAILED:
    case APP_EVENT_SYNC_FAILED:
    case APP_EVENT_DISCONNECTED:
    case APP_EVENT_INVALID_STATE:
    case APP_EVENT_OTA_FAILED:
        return true;
    default:
        return false;
    }
}

static const char *app_state_name(app_state_t state)
{
    switch (state)
    {
    case APP_STATE_BOOT:          return "boot";
    case APP_STATE_FILESYSTEM:    return "filesystem";
    case APP_STATE_CONFIGURATION: return "configuration";
    case APP_STATE_NETWORK:       return "network";
    case APP_STATE_PROVISIONING:  return "provisioning";
    case APP_STATE_TLS:           return "tls";
    case APP_STATE_SYNC:          return "sync";
    case APP_STATE_ONLINE:        return "online";
    case APP_STATE_SAFE_OFF:      return "safe-off";
    case APP_STATE_FATAL:         return "fatal";
    case APP_STATE_OTA:           return "ota";
    }
    return "?";
}

static const char *app_event_name(app_state_event_t event)
{
    switch (event)
    {
    case APP_EVENT_START:             return "start";
    case APP_EVENT_FS_OK:             return "fs-ok";
    case APP_EVENT_FS_FAIL:           return "fs-fail";
    case APP_EVENT_CONFIG_OK:         return "config-ok";
    case APP_EVENT_CONFIG_FAIL:       return "config-fail";
    case APP_EVENT_NETWORK_CONNECTED: return "network-connected";
    case APP_EVENT_NETWORK_FAILED:    return "network-failed";
    case APP_EVENT_PROVISIONING_STARTED:   return "provisioning-started";
    case APP_EVENT_PROVISIONING_SUCCEEDED: return "provisioning-succeeded";
    case APP_EVENT_PROVISIONING_FAILED:    return "provisioning-failed";
    case APP_EVENT_TLS_CONNECTED:     return "tls-connected";
    case APP_EVENT_TLS_FAILED:        return "tls-failed";
    case APP_EVENT_SYNC_COMPLETE:     return "sync-complete";
    case APP_EVENT_SYNC_FAILED:       return "sync-failed";
    case APP_EVENT_DISCONNECTED:      return "disconnected";
    case APP_EVENT_INVALID_STATE:     return "invalid-state";
    case APP_EVENT_OTA_BEGIN:         return "ota-begin";
    case APP_EVENT_OTA_END:           return "ota-end";
    case APP_EVENT_OTA_FAILED:        return "ota-failed";
    case APP_EVENT_RETRY_DUE:         return "retry-due";
    case APP_EVENT_RESET:             return "reset";
    case APP_EVENT_FATAL:             return "fatal";
    }
    return "?";
}

static const char *app_owner_name(app_transition_owner_t owner)
{
    switch (owner)
    {
    case APP_OWNER_BOOTSTRAP:    return "bootstrap";
    case APP_OWNER_FILESYSTEM:   return "filesystem";
    case APP_OWNER_CONFIGURATION:return "configuration";
    case APP_OWNER_NETWORK:      return "network";
    case APP_OWNER_MQTT:         return "mqtt";
    case APP_OWNER_THINGSBOARD:  return "thingsboard";
    case APP_OWNER_TIMER:        return "timer";
    case APP_OWNER_OTA:          return "ota";
    case APP_OWNER_WATCHDOG:     return "watchdog";
    case APP_OWNER_EXTERNAL:     return "external";
    }
    return "?";
}

/* --------------------------------------------------------------------- */
/* Fail-off / transition helpers (called with the lock held)              */
/* --------------------------------------------------------------------- */

/**
 * @brief Force the lamp output inactive; logs a safety event on failure.
 *
 * The transition machinery cannot do more than best-effort fail-off:
 * lamp_control itself reports #LAMP_ERR_FAIL_OFF when even the escalated
 * fail-off cannot prove the output off.  We log loudly and keep the state
 * transition so the machine does not wedge.
 */
static void app_force_inactive(void)
{
    lamp_status_t status = lamp_control_force_inactive();
    if (status != LAMP_OK)
    {
        osal_log_error("[app_state] fail-off could not force the output "
                       "inactive: %d", (int)status);
    }
}

static void app_clear_retry(void)
{
    s_state.retry_pending   = false;
    s_state.retry_due_ms    = 0U;
    s_state.retry_delay_ms  = 0U;
    s_state.retry_attempts  = 0U;
    s_state.retry_exhausted = false;
    s_state.retry_target    = APP_STATE_NONE_SAFE;
}

/**
 * @brief Execute one state transition (lock held).
 *
 * Enforces the fail-off invariant (every non-online entry forces the
 * output inactive), refreshes the watchdog deadline on forward progress
 * when armed, records the transition owner and notifies the observer.
 */
static void app_do_transition(app_state_t to, app_state_event_t event,
                              app_transition_owner_t owner)
{
    app_state_t from = s_state.state;

    s_state.state     = to;
    s_state.last_owner = owner;

    if (to != APP_STATE_ONLINE)
    {
        app_force_inactive();
    }

    if (s_state.wdt_armed)
    {
        s_state.deadline_ms = app_now();
    }

    if (s_state.cfg.observer != NULL)
    {
        s_state.cfg.observer(from, to, event, owner, s_state.session);
    }

    osal_log_info("[app_state] %s --%s(%s)--> %s (session %u)",
                  app_state_name(from), app_event_name(event),
                  app_owner_name(owner), app_state_name(to),
                  (unsigned)s_state.session);
}

/**
 * @brief Schedule (or refuse) the bounded backoff retry for a retryable
 *        failure that just parked the machine in SAFE_OFF (lock held).
 *
 * The delay grows per consumed attempt (exponentially, capped); when the
 * per-episode budget is spent the machine parks with no further retry.
 */
static void app_arm_retry(app_state_t target)
{
    if (s_state.retry_attempts >= s_state.cfg.retry_max_attempts)
    {
        /* Budget exhausted: park silently — this is the anti-storm rule.
         * Nothing is scheduled, so the reported delay is cleared too. */
        s_state.retry_pending   = false;
        s_state.retry_due_ms    = 0U;
        s_state.retry_delay_ms  = 0U;
        s_state.retry_exhausted = true;
        return;
    }

    uint32_t delay;
    if (s_state.retry_attempts == 0U)
    {
        delay = s_state.cfg.retry_initial_delay_ms;
    }
    else
    {
        uint64_t grown = (uint64_t)s_state.retry_delay_ms *
                         (uint64_t)s_state.cfg.retry_backoff_factor;
        delay = (grown >= s_state.cfg.retry_max_delay_ms)
                    ? s_state.cfg.retry_max_delay_ms
                    : (uint32_t)grown;
    }

    s_state.retry_delay_ms  = delay;
    s_state.retry_due_ms    = app_now() + delay;
    s_state.retry_pending   = true;
    s_state.retry_exhausted = false;
    s_state.retry_target    = target;
}

/**
 * @brief Execute a failure transition to SAFE_OFF (lock held).
 *
 * @param[in] entry The table entry that fired.
 */
static void app_enter_safe_off(const app_state_transition_t *entry)
{
    if (entry->fcls == APP_FAILURE_RETRYABLE)
    {
        app_arm_retry(entry->retry_target);
    }
    else
    {
        /* Degraded (non-retryable): park without an automatic retry.
         * retry_attempts is deliberately NOT cleared: a degraded park must
         * not refill the retry budget (that is the anti-storm rule — do not
         * "reset the budget on degraded park" without revisiting this). */
        s_state.retry_pending   = false;
        s_state.retry_due_ms    = 0U;
        s_state.retry_exhausted = false;
        s_state.retry_target    = APP_STATE_NONE_SAFE;
    }
    app_do_transition(APP_STATE_SAFE_OFF, entry->event, entry->owner);
}

/* --------------------------------------------------------------------- */
/* Table lookup                                                           */
/* --------------------------------------------------------------------- */

static const app_state_transition_t *app_lookup(app_state_t state,
                                                app_state_event_t event)
{
    size_t i;

    for (i = 0; i < (sizeof(APP_STATE_TRANSITIONS) /
                     sizeof(APP_STATE_TRANSITIONS[0])); ++i)
    {
        if ((APP_STATE_TRANSITIONS[i].from == state) &&
            (APP_STATE_TRANSITIONS[i].event == event))
        {
            return &APP_STATE_TRANSITIONS[i];
        }
    }
    return NULL;
}

/**
 * @brief Owner acceptance rule (lock held).
 *
 * The DISCONNECTED event may be reported by either transport context —
 * the Wi-Fi adapter (APP_OWNER_NETWORK) or the MQTT/TLS layer
 * (APP_OWNER_MQTT) — both are legitimate owners of "transport down".
 */
static bool app_owner_ok(const app_state_transition_t *entry,
                         app_transition_owner_t owner)
{
    if (entry->owner == owner)
    {
        return true;
    }
    return (entry->event == APP_EVENT_DISCONNECTED) &&
           ((owner == APP_OWNER_NETWORK) || (owner == APP_OWNER_MQTT));
}

/* --------------------------------------------------------------------- */
/* Locking                                                                */
/* --------------------------------------------------------------------- */

static bool app_lock(void)
{
    osal_mutex_id_t lock =
        atomic_load_explicit(&s_state.lock, memory_order_acquire);

    if ((lock == NULL) || (osal_mutex_take(lock) != OSAL_SUCCESS))
    {
        osal_log_error("[app_state] lock unavailable");
        return false;
    }
    return true;
}

static void app_unlock(void)
{
    osal_mutex_id_t lock =
        atomic_load_explicit(&s_state.lock, memory_order_acquire);

    if (lock != NULL)
    {
        (void)osal_mutex_give(lock);
    }
}

/* --------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* --------------------------------------------------------------------- */

app_state_status_t app_state_init(const app_state_config_t *config)
{
    osal_mutex_id_t created;
    osal_mutex_id_t expected;

    if (config == NULL)
    {
        return APP_STATE_ERR_INVALID_ARGUMENT;
    }

    /* Create the module mutex and CAS-adopt it into the published slot
     * (first-use race, see the lock comment above).  Only one init can win
     * the adoption for a given lifecycle generation. */
    created = NULL;
    if (osal_mutex_create(&created, "app_state") != OSAL_SUCCESS)
    {
        return APP_STATE_ERR_NOT_INITIALIZED;
    }

    expected = NULL;
    if (!atomic_compare_exchange_strong(&s_state.lock, &expected, created))
    {
        /* Another thread already published a lock: the module is (or was,
         * pending deinit) initialized.  We never call back into the winner
         * under any lock here - the caller-visible guarantee is "already
         * initialized". */
        (void)osal_mutex_delete(created);
        return APP_STATE_ERR_ALREADY_INITIALIZED;
    }

    /* We own the fresh lock; nobody can be inside the module yet, so no
     * take() is needed while we fill the state.  The lock is released
     * (given back) at the end for the first real user. */
    s_state.cfg.now_fn = config->now_ms;
    if (s_state.cfg.now_fn == NULL)
    {
        s_state.cfg.now_fn = osal_task_get_time_ms;
    }
    s_state.cfg.retry_initial_delay_ms =
        (config->retry_initial_delay_ms == 0U)
            ? APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS
            : config->retry_initial_delay_ms;
    if (s_state.cfg.retry_initial_delay_ms < APP_STATE_RETRY_DELAY_MIN_MS)
    {
        s_state.cfg.retry_initial_delay_ms = APP_STATE_RETRY_DELAY_MIN_MS;
    }
    if (s_state.cfg.retry_initial_delay_ms > APP_STATE_RETRY_DELAY_MAX_MS)
    {
        s_state.cfg.retry_initial_delay_ms = APP_STATE_RETRY_DELAY_MAX_MS;
    }
    s_state.cfg.retry_max_delay_ms =
        (config->retry_max_delay_ms == 0U)
            ? APP_STATE_RETRY_MAX_DELAY_DEFAULT_MS
            : config->retry_max_delay_ms;
    if (s_state.cfg.retry_max_delay_ms < APP_STATE_RETRY_DELAY_MIN_MS)
    {
        s_state.cfg.retry_max_delay_ms = APP_STATE_RETRY_DELAY_MIN_MS;
    }
    if (s_state.cfg.retry_max_delay_ms > APP_STATE_RETRY_DELAY_MAX_MS)
    {
        s_state.cfg.retry_max_delay_ms = APP_STATE_RETRY_DELAY_MAX_MS;
    }
    if (s_state.cfg.retry_max_delay_ms <
        s_state.cfg.retry_initial_delay_ms)
    {
        /* Keep the schedule sane: cap >= initial. */
        s_state.cfg.retry_max_delay_ms = s_state.cfg.retry_initial_delay_ms;
    }
    s_state.cfg.retry_max_attempts =
        (config->retry_max_attempts == 0U)
            ? APP_STATE_RETRY_MAX_ATTEMPTS_DEFAULT
            : config->retry_max_attempts;
    if (s_state.cfg.retry_max_attempts == 0U)
    {
        /* Clamp keeps at least one retry possible. */
        s_state.cfg.retry_max_attempts = 1U;
    }
    s_state.cfg.retry_backoff_factor =
        (config->retry_backoff_factor == 0U)
            ? APP_STATE_RETRY_BACKOFF_FACTOR_DEFAULT
            : config->retry_backoff_factor;
    if (s_state.cfg.retry_backoff_factor < 1U)
    {
        s_state.cfg.retry_backoff_factor = 1U;
    }
    if (s_state.cfg.retry_backoff_factor > 10U)
    {
        s_state.cfg.retry_backoff_factor = 10U;
    }
    s_state.cfg.watchdog_timeout_ms =
        (config->watchdog_timeout_ms == 0U)
            ? APP_STATE_WATCHDOG_TIMEOUT_DEFAULT_MS
            : config->watchdog_timeout_ms;
    if (s_state.cfg.watchdog_timeout_ms < APP_STATE_WATCHDOG_TIMEOUT_MIN_MS)
    {
        s_state.cfg.watchdog_timeout_ms = APP_STATE_WATCHDOG_TIMEOUT_MIN_MS;
    }
    if (s_state.cfg.watchdog_timeout_ms > APP_STATE_WATCHDOG_TIMEOUT_MAX_MS)
    {
        s_state.cfg.watchdog_timeout_ms = APP_STATE_WATCHDOG_TIMEOUT_MAX_MS;
    }
    s_state.cfg.on_watchdog_expired = config->on_watchdog_expired;
    s_state.cfg.observer            = config->observer;

    /* Fresh machine: Boot, session 0, watchdog armed but parked until
     * start() provides the first deadline. */
    s_state.state         = APP_STATE_BOOT;
    s_state.session       = 0U;
    s_state.last_owner    = APP_OWNER_EXTERNAL;
    s_state.wdt_armed     = true;
    s_state.deadline_ms   = s_state.cfg.now_fn();
    app_clear_retry();
    s_state.stale_dropped   = 0U;
    s_state.invalid_dropped = 0U;
    s_state.initialized     = true;

    osal_log_info("[app_state] initialized (watchdog %u ms, retry "
                  "initial %u ms / cap %u ms / %u attempts)",
                  (unsigned)s_state.cfg.watchdog_timeout_ms,
                  (unsigned)s_state.cfg.retry_initial_delay_ms,
                  (unsigned)s_state.cfg.retry_max_delay_ms,
                  (unsigned)s_state.cfg.retry_max_attempts);

    /* Release the fresh lock for the first real user, then report
     * success.  The mutex give cannot fail on the freshly adopted lock. */
    (void)osal_mutex_give(s_state.lock);
    return APP_STATE_OK;
}

void app_state_deinit(void)
{
    osal_mutex_id_t lock;

    if (!app_lock())
    {
        return;
    }

    if (s_state.initialized)
    {
        lock = atomic_load_explicit(&s_state.lock, memory_order_acquire);
        osal_log_info("[app_state] deinitialized");
        s_state.initialized = false;
        s_state.state       = APP_STATE_BOOT;
        s_state.session     = 0U;
        s_state.wdt_armed   = false;
        /* Give the module mutex back, unpublish it, THEN delete it: no
         * later operation can observe a lock that is being destroyed. */
        app_unlock();
        atomic_store_explicit(&s_state.lock, NULL, memory_order_release);
        (void)osal_mutex_delete(lock);
    }
    else
    {
        app_unlock();
    }
}

app_state_status_t app_state_start(void)
{
    if (!app_lock())
    {
        return APP_STATE_ERR_NOT_INITIALIZED;
    }

    if (!s_state.initialized)
    {
        app_unlock();
        return APP_STATE_ERR_NOT_INITIALIZED;
    }

    if (s_state.state != APP_STATE_BOOT)
    {
        osal_log_warning("[app_state] start() rejected: not in boot "
                         "(state %s)", app_state_name(s_state.state));
        app_unlock();
        return APP_STATE_ERR_STATE;
    }

    /* New boot episode: fresh session, watchdog (re)armed, output off. */
    s_state.session++;
    s_state.wdt_armed   = true;
    s_state.deadline_ms = app_now();
    app_do_transition(APP_STATE_FILESYSTEM, APP_EVENT_START,
                      APP_OWNER_BOOTSTRAP);
    app_unlock();
    return APP_STATE_OK;
}

/* --------------------------------------------------------------------- */
/* Event delivery                                                         */
/* --------------------------------------------------------------------- */

static app_state_status_t app_state_deliver_locked(app_state_event_t event,
                                                   app_transition_owner_t owner,
                                                   uint32_t session)
{
    /* Fatal reports are always honored: any session, any owner, any state.
     * A device that reaches FATAL stays there until an explicit reset. */
    if (event == APP_EVENT_FATAL)
    {
        if (s_state.state != APP_STATE_FATAL)
        {
            app_clear_retry();
            s_state.session++;
            app_do_transition(APP_STATE_FATAL, event, owner);
        }
        return APP_STATE_OK;
    }

    /* Events that only the machine itself may fire. */
    if ((event == APP_EVENT_START) || (event == APP_EVENT_RETRY_DUE))
    {
        s_state.invalid_dropped++;
        osal_log_warning("[app_state] non-deliverable event %s dropped",
                         app_event_name(event));
        return APP_STATE_ERR_ILLEGAL;
    }

    /* Stale session check: the callback belongs to a previous episode. */
    if (!app_state_is_disconnect_class(event) && (session != s_state.session))
    {
        /* Non-safety events from a stale session carry no obligation. */
        s_state.stale_dropped++;
        osal_log_warning("[app_state] stale event %s (session %u != %u) "
                         "dropped", app_event_name(event), (unsigned)session,
                         (unsigned)s_state.session);
        return APP_STATE_ERR_STALE;
    }
    if (app_state_is_disconnect_class(event) && (session != s_state.session))
    {
        /* Safety-first stale disconnect: fail off BEFORE dropping, exactly
         * like the network adapter's stale-disconnect contract. */
        app_force_inactive();
        s_state.stale_dropped++;
        osal_log_warning("[app_state] stale %s (session %u != %u) dropped "
                         "after fail-off", app_event_name(event),
                         (unsigned)session, (unsigned)s_state.session);
        return APP_STATE_ERR_STALE;
    }

    /* External reset is legal from any state: back to boot, new episode. */
    if (event == APP_EVENT_RESET)
    {
        app_clear_retry();
        s_state.session++;
        s_state.wdt_armed   = true;
        s_state.deadline_ms = app_now();
        app_do_transition(APP_STATE_BOOT, event, APP_OWNER_EXTERNAL);
        return APP_STATE_OK;
    }

    /* Table-driven legal transition + owner validation. */
    const app_state_transition_t *entry = app_lookup(s_state.state, event);
    if ((entry == NULL) || !app_owner_ok(entry, owner))
    {
        /* Illegal event for the current state, or wrong transition owner:
         * disconnect-class events still fail off before the drop. */
        if (app_state_is_disconnect_class(event))
        {
            app_force_inactive();
        }
        s_state.invalid_dropped++;
        osal_log_warning("[app_state] illegal %s from %s owner %s dropped",
                         app_event_name(event), app_state_name(s_state.state),
                         app_owner_name(owner));
        return APP_STATE_ERR_ILLEGAL;
    }

    if (entry->new_episode)
    {
        s_state.session++;
    }

    if (entry->to == APP_STATE_SAFE_OFF)
    {
        app_enter_safe_off(entry);
    }
    else
    {
        app_do_transition(entry->to, entry->event, entry->owner);
        if (entry->to == APP_STATE_ONLINE)
        {
            /* Arriving online resets the retry budget: a healed link may
             * consume a full fresh budget for the next outage. */
            app_clear_retry();
        }
    }

    return APP_STATE_OK;
}

app_state_status_t app_state_deliver(app_state_event_t event,
                                     app_transition_owner_t owner)
{
    app_state_status_t status;

    if (!app_lock())
    {
        return APP_STATE_ERR_NOT_INITIALIZED;
    }
    if (!s_state.initialized)
    {
        app_unlock();
        return APP_STATE_ERR_NOT_INITIALIZED;
    }
    status = app_state_deliver_locked(event, owner, s_state.session);
    app_unlock();
    return status;
}

app_state_status_t app_state_deliver_session(app_state_event_t event,
                                             app_transition_owner_t owner,
                                             uint32_t session)
{
    app_state_status_t status;

    if (!app_lock())
    {
        return APP_STATE_ERR_NOT_INITIALIZED;
    }
    if (!s_state.initialized)
    {
        app_unlock();
        return APP_STATE_ERR_NOT_INITIALIZED;
    }
    status = app_state_deliver_locked(event, owner, session);
    app_unlock();
    return status;
}

/* --------------------------------------------------------------------- */
/* Polling / watchdog / retry timer                                       */
/* --------------------------------------------------------------------- */

app_state_status_t app_state_poll_at(uint32_t now_ms)
{
    uint32_t elapsed;

    if (!app_lock())
    {
        return APP_STATE_ERR_NOT_INITIALIZED;
    }
    if (!s_state.initialized)
    {
        app_unlock();
        return APP_STATE_ERR_NOT_INITIALIZED;
    }

    /* 1. Watchdog check: the deadline was not refreshed within the
     *    timeout (no poll, no successful transition).  The detecting poll
     *    fails the machine, in order: output off (explicit, before the
     *    callback), expiry callback, disarm, clear the pending retry and
     *    transition to FATAL (whose non-online entry enforces fail-off
     *    again through the transition helper). */
    if (s_state.wdt_armed)
    {
        elapsed = (uint32_t)(now_ms - s_state.deadline_ms);
        if (elapsed >= s_state.cfg.watchdog_timeout_ms)
        {
            app_force_inactive();
            s_state.wdt_armed = false;
            app_clear_retry();
            if (s_state.cfg.on_watchdog_expired != NULL)
            {
                s_state.cfg.on_watchdog_expired();
            }
            if (s_state.state != APP_STATE_FATAL)
            {
                s_state.session++;
                app_do_transition(APP_STATE_FATAL, APP_EVENT_FATAL,
                                  APP_OWNER_WATCHDOG);
            }
            app_unlock();
            return APP_STATE_ERR_WATCHDOG;
        }
    }

    /* 2. Feed: the poll itself is the single kept-alive mechanism. */
    if (s_state.wdt_armed)
    {
        s_state.deadline_ms = now_ms;
    }

    /* 3. Retry schedule: consume one attempt and return to the failed
     *    stage when the backoff delay has elapsed. */
    if (s_state.state == APP_STATE_SAFE_OFF && s_state.retry_pending &&
        app_time_ge(now_ms, s_state.retry_due_ms))
    {
        s_state.retry_pending = false;
        s_state.retry_attempts++;
        s_state.session++;
        app_do_transition(s_state.retry_target, APP_EVENT_RETRY_DUE,
                          APP_OWNER_TIMER);
    }

    app_unlock();
    return APP_STATE_OK;
}

app_state_status_t app_state_poll(void)
{
    uint32_t now;

    /* app_now() dereferences the configured clock, which only exists
     * after init — check first, then read the clock. */
    if (!app_lock())
    {
        return APP_STATE_ERR_NOT_INITIALIZED;
    }
    if (!s_state.initialized)
    {
        app_unlock();
        return APP_STATE_ERR_NOT_INITIALIZED;
    }
    now = app_now();
    app_unlock();

    return app_state_poll_at(now);
}

/* --------------------------------------------------------------------- */
/* State queries                                                          */
/* --------------------------------------------------------------------- */

app_state_t app_state_current(void)
{
    app_state_t state;

    if (!app_lock())
    {
        return APP_STATE_BOOT;
    }
    state = s_state.initialized ? s_state.state : APP_STATE_BOOT;
    app_unlock();
    return state;
}

uint32_t app_state_session(void)
{
    uint32_t session;

    if (!app_lock())
    {
        return 0U;
    }
    session = s_state.initialized ? s_state.session : 0U;
    app_unlock();
    return session;
}

app_transition_owner_t app_state_last_owner(void)
{
    app_transition_owner_t owner;

    if (!app_lock())
    {
        return APP_OWNER_EXTERNAL;
    }
    owner = s_state.initialized ? s_state.last_owner : APP_OWNER_EXTERNAL;
    app_unlock();
    return owner;
}

bool app_state_is_online(void)
{
    return app_state_current() == APP_STATE_ONLINE;
}

bool app_state_retry_pending(void)
{
    bool pending;

    if (!app_lock())
    {
        return false;
    }
    pending = s_state.initialized && s_state.retry_pending;
    app_unlock();
    return pending;
}

uint32_t app_state_retry_delay_ms(void)
{
    uint32_t delay;

    if (!app_lock())
    {
        return 0U;
    }
    delay = s_state.initialized ? s_state.retry_delay_ms : 0U;
    app_unlock();
    return delay;
}

uint32_t app_state_retry_attempts_used(void)
{
    uint32_t attempts;

    if (!app_lock())
    {
        return 0U;
    }
    attempts = s_state.initialized ? s_state.retry_attempts : 0U;
    app_unlock();
    return attempts;
}

bool app_state_retry_exhausted(void)
{
    bool exhausted;

    if (!app_lock())
    {
        return false;
    }
    exhausted = s_state.initialized && s_state.retry_exhausted;
    app_unlock();
    return exhausted;
}

uint32_t app_state_stale_dropped(void)
{
    uint32_t count;

    if (!app_lock())
    {
        return 0U;
    }
    count = s_state.initialized ? s_state.stale_dropped : 0U;
    app_unlock();
    return count;
}

uint32_t app_state_invalid_dropped(void)
{
    uint32_t count;

    if (!app_lock())
    {
        return 0U;
    }
    count = s_state.initialized ? s_state.invalid_dropped : 0U;
    app_unlock();
    return count;
}