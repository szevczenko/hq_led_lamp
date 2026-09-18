/**
 * @file network_manager.c
 * @brief Product-owned network adapter around the platform Wi-Fi manager
 *        (TASK-109)
 *
 * Implementation of the documented network_callbacks_t product contract on
 * top of the platform Wi-Fi manager (wifi_managment.h).
 *
 * Design rules enforced here:
 *
 *   1. Narrow surface: the ONLY platform network header included is
 *      wifi_managment.h, and only the operations the product contract needs
 *      are used (type selection, init/start, event subscribe, connect,
 *      connected query, disconnect, stop).  Credential persistence
 *      (wifi_config.h / wifi_ap.json / saved credential buffers) is never
 *      touched here and therefore never crosses a product API boundary.
 *
 *   2. Secrecy: nothing that could contain an SSID or password is ever
 *      fetched, stored or logged.  The adapter logs state transitions only.
 *
 *   3. Ordering: start() completes Wi-Fi onboarding (init -> subscribe ->
 *      start -> ready) BEFORE it returns and requests the connect, so
 *      ThingsBoard can never begin connecting before the network is up (it
 *      must additionally pass network_manager_wait_connected()).
 *
 *   4. Fail-off: every disconnect-class event (lost connection or exhausted
 *      connect attempts) synchronously forces the lamp output inactive
 *      before the application state machine (on_disconnected) is informed.
 *
 *   5. Thread-safe callback ownership / late callbacks: registration state
 *      and the user callback table live behind one OSAL mutex.  stop() clears
 *      the table under that mutex before unsubscribing, so any late event
 *      that raced the unsubscribe is dropped by re-validation instead of
 *      reaching an unregistered context.  Every subscription carries a
 *      per-start generation token (as the platform's user_data): a late event
 *      from a previous session's dispatch snapshot carries the OLD token and
 *      is dropped even after a fresh start() re-registered new
 *      callbacks/context.  stop() joins an in-flight callback (the handler
 *      holds the same mutex during delivery), so no application
 *      callback can run after stop() returns.
 *
 *   6. Mode policy: the Wi-Fi start mode is a product policy decision that
 *      lives HERE (the network stage is the single Wi-Fi owner, TASK-109),
 *      selected by the KLC_WIFI_DEFAULT_MODE Kconfig choice.  AP+STA
 *      (T_WIFI_TYPE_CLI_SER) is the default and matches the provisioning
 *      demo's ownership order because the provisioning portal needs the AP
 *      interface: a station-only start strands an unprovisioned device, and
 *      the portal's wifi_mgmt_request_mode(T_WIFI_TYPE_CLI_SER) cannot bring
 *      the AP up after a client-only start.  Pure station-only
 *      (T_WIFI_TYPE_CLIENT) remains selectable for hardened production
 *      builds that are provisioned out-of-band and never need the portal.
 *      No platform Wi-Fi type ever appears in network_manager.h.
 */

#include "network_manager.h"

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "lamp_control.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "wifi_managment.h"

/* --------------------------------------------------------------------- */
/* Internal state                                                          */
/* --------------------------------------------------------------------- */

typedef struct network_ctx
{
  _Atomic(osal_mutex_id_t) lock;     /**< Published atomically (CAS-adopted).*/
  network_callbacks_t callbacks;     /**< Valid only while started.         */
  atomic_bool started;               /**< Callbacks armed, API operational. */
  uint32_t active_token;             /**< Token of the armed registration
                                          (valid under @c lock only).       */
  uint32_t generation;               /**< Monotonic per-start token source. */
  uint32_t stop_count;               /**< Number of completed stop()
                                          disarms (valid under @c lock only).
                                          A start() records this value when
                                          it arms its session and re-checks
                                          it after the platform
                                          registration calls: a changed
                                          count means a concurrent stop()
                                          disarmed this session mid-
                                          transaction, so the start must
                                          abort and roll back instead of
                                          returning NETWORK_OK (start/stop
                                          transaction race). */
} network_ctx_t;

static network_ctx_t s_ctx = {
  .lock         = NULL,
  .callbacks    = { NULL, NULL, NULL },
  .started      = false,
  .active_token = 0u,
  .generation   = 0u,
  .stop_count   = 0u,
};

/* --------------------------------------------------------------------- */
/* Small lock helpers                                                     */
/* --------------------------------------------------------------------- */

/**
 * @brief Create the adapter mutex before concurrent use is possible.
 *
 * The adapter owns no constructor, so the mutex is created lazily — but the
 * creation itself is race-free: an atomic compare-and-swap adopts exactly one
 * winner's mutex, and every loser destroys its duplicate before anyone can
 * observe it.  start()/stop() therefore never publish or delete a mutex
 * concurrently, and once published the handle stays valid for the lifetime
 * of the process (it is never deleted).
 *
 * @return true when the adapter lock is available.
 */
static bool network_ensure_lock(void)
{
    osal_mutex_id_t current =
        atomic_load_explicit(&s_ctx.lock, memory_order_acquire);
    if (current != NULL)
    {
        return true;
    }

    osal_mutex_id_t created = NULL;
    if (osal_mutex_create(&created, "net_mgr") != OSAL_SUCCESS)
    {
        return false;
    }

    osal_mutex_id_t expected = NULL;
    if (atomic_compare_exchange_strong_explicit(&s_ctx.lock, &expected,
                                                created,
                                                memory_order_release,
                                                memory_order_acquire))
    {
        /* This thread adopted its mutex; a mutex was published only once. */
        return true;
    }

    /* Another thread won the creation race; drop our duplicate. */
    (void)osal_mutex_delete(created);
    return true;
}

/**
 * @brief Take the adapter lock.
 *
 * @return true when the lock is held and must be released with
 *         network_unlock(); false means nothing was taken.
 */
static bool network_lock(void)
{
    osal_mutex_id_t lock =
        atomic_load_explicit(&s_ctx.lock, memory_order_acquire);
    return (lock != NULL) && (osal_mutex_take(lock) == OSAL_SUCCESS);
}

/** @brief Release the adapter lock after a successful network_lock(). */
static void network_unlock(void)
{
    osal_mutex_id_t lock = atomic_load_explicit(&s_ctx.lock,
                                                memory_order_acquire);
    if (lock != NULL)
    {
        (void)osal_mutex_give(lock);
    }
}

/**
 * @brief Arm a registration: store the callback table, publish a fresh
 *        per-start generation token and mark the adapter started.
 *
 * @note Called with the lock held.
 */
static void network_arm_locked(const network_callbacks_t *callbacks)
{
    s_ctx.callbacks    = *callbacks;
    s_ctx.active_token = ++s_ctx.generation;
    atomic_store_explicit(&s_ctx.started, true, memory_order_release);
}

/**
 * @brief Shared registration/rollback core: clear the callback table so any
 *        in-flight or late event is stale.
 *
 * @note Called with the lock held.  The token of the disarmed registration
 *       stays readable in s_ctx.active_token until the next arm, so the
 *       caller can still unsubscribe with the exact user_data it registered.
 */
static void network_disarm_locked(void)
{
    s_ctx.callbacks.on_connected    = NULL;
    s_ctx.callbacks.on_disconnected = NULL;
    s_ctx.callbacks.context         = NULL;
    atomic_store_explicit(&s_ctx.started, false, memory_order_release);
}

/* --------------------------------------------------------------------- */
/* Event delivery (runs in the Wi-Fi manager worker task)                 */
/* --------------------------------------------------------------------- */

/**
 * @brief Deliver the connected notification to the registered handler.
 *
 * The adapter lock is held across the invocation on purpose: it joins any
 * concurrent stop() and guarantees stop() never returns while a callback is
 * still running (the documented late-callback contract).
 *
 * @param [in] token - per-start generation token this event was subscribed
 *                     with; a token that does not match the armed
 *                     registration identifies a stale event from a previous
 *                     adapter session and is dropped.
 */
static void network_deliver_connected(uint32_t token)
{
    if (!network_lock())
    {
        return;
    }

    if (!atomic_load_explicit(&s_ctx.started, memory_order_acquire) ||
        (token != s_ctx.active_token) ||
        (s_ctx.callbacks.on_connected == NULL))
    {
        /* Late callback racing stop(), or an event snapshot taken by the
         * manager before a stop/restart cycle: the registration is gone or
         * belongs to a different (older) session. */
        network_unlock();
        osal_log_debug("[net] dropped late connected event");
        return;
    }

    network_callbacks_t cb = s_ctx.callbacks;
    osal_log_info("[net] network connected");
    cb.on_connected(cb.context);

    network_unlock();
}

/**
 * @brief Deliver a disconnect-class notification (lost connection or a
 *        failed connect attempt).
 *
 * The lamp output is forced inactive BEFORE the application state machine is
 * informed, synchronously on this path, so Wi-Fi loss drives fail-off within
 * application latency even without any registered callback.
 *
 * @param [in] token - per-start generation token (see
 *                     network_deliver_connected()).
 */
static void network_deliver_disconnected(uint32_t token)
{
    /* Immediate fail-off: the electrical safety action must not wait for the
     * application callback.  Force-inactive is idempotent, so this is safe
     * even if the event turns out to be a stale, already-dropped delivery. */
    lamp_status_t lamp_status = lamp_control_force_inactive();
    if (lamp_status != LAMP_OK)
    {
        osal_log_error("[net] fail-off on network loss failed: %d",
                       (int)lamp_status);
    }

    if (!network_lock())
    {
        return;
    }

    if (!atomic_load_explicit(&s_ctx.started, memory_order_acquire) ||
        (token != s_ctx.active_token) ||
        (s_ctx.callbacks.on_disconnected == NULL))
    {
        /* Late callback racing stop(), or an event from a previous session:
         * dropped, the lamp is already off. */
        network_unlock();
        osal_log_debug("[net] dropped late disconnected event");
        return;
    }

    network_callbacks_t cb = s_ctx.callbacks;
    osal_log_warning("[net] network disconnected (lamp forced off)");
    cb.on_disconnected(cb.context);

    network_unlock();
}

/**
 * @brief Wi-Fi manager event hook: map manager events onto the product
 *        contract.  Everything outside the two product events is ignored.
 *
 * @c user_data carries the per-start generation token the adapter subscribed
 * with, so a stale event from a manager dispatch snapshot taken before a
 * stop/restart cycle can never be delivered to the new session's callbacks.
 */
static void network_wifi_event_cb(wifi_mgmt_event_t event, void *user_data)
{
    const uint32_t token = (uint32_t)(uintptr_t)user_data;

    switch (event)
    {
        case WIFI_MGMT_EVENT_CONNECTED:
            network_deliver_connected(token);
            break;

        case WIFI_MGMT_EVENT_DISCONNECTED:
        case WIFI_MGMT_EVENT_CONNECT_FAILED:
            network_deliver_disconnected(token);
            break;

        default:
            /* SCAN_COMPLETED / MODE_CHANGED are provisioning-internal. */
            break;
    }
}

/* --------------------------------------------------------------------- */
/* Public API                                                             */
/* --------------------------------------------------------------------- */

int network_manager_start(const network_callbacks_t *callbacks)
{
    if ((callbacks == NULL) ||
        (callbacks->on_connected == NULL) ||
        (callbacks->on_disconnected == NULL))
    {
        return NETWORK_ERR_INVALID_ARGUMENT;
    }

    if (!network_ensure_lock() || !network_lock())
    {
        return NETWORK_ERR_START_FAILED;
    }

    if (atomic_load_explicit(&s_ctx.started, memory_order_acquire))
    {
        network_unlock();
        return NETWORK_ERR_ALREADY_STARTED;
    }

    network_arm_locked(callbacks);
    const uint32_t token = s_ctx.active_token;
    /* (review finding: start/stop transaction race) Snapshot the number of
     * completed stop() disarms: if it changes while this start is inside
     * its platform registration transaction, a concurrent stop() disarmed
     * this session and the start must abort and roll everything back. */
    const uint32_t stop_count_before = s_ctx.stop_count;
    network_unlock();

    /* Wi-Fi onboarding must complete before anything downstream (ThingsBoard)
     * may begin connecting.  The start mode is a product policy decision
     * (KLC_WIFI_DEFAULT_MODE): by default the manager starts in AP+STA so the
     * provisioning portal's soft-AP is available when no usable credentials
     * exist yet; the hardened station-only mode is selectable for builds
     * provisioned out-of-band.  Keep the demo's ownership order:
     * set_wifi_type -> init -> start. */
#if defined(CONFIG_KLC_WIFI_DEFAULT_MODE_APSTA)
    wifi_mgmt_set_wifi_type(T_WIFI_TYPE_CLI_SER);
#elif defined(CONFIG_KLC_WIFI_DEFAULT_MODE_CLIENT)
    wifi_mgmt_set_wifi_type(T_WIFI_TYPE_CLIENT);
#else
#error "KLC_WIFI_DEFAULT_MODE must resolve to APSTA or CLIENT"
#endif
    wifi_mgmt_init();

    /* The token travels as user_data: every event the manager delivers to
     * this session's subscriptions carries it, and the handler drops any
     * event whose token no longer matches (late delivery from a previous
     * session's dispatch snapshot). */
    const bool subscribed =
        wifi_mgmt_subscribe(WIFI_MGMT_EVENT_CONNECTED,
                            network_wifi_event_cb,
                            (void *)(uintptr_t)token) &&
        wifi_mgmt_subscribe(WIFI_MGMT_EVENT_DISCONNECTED,
                            network_wifi_event_cb,
                            (void *)(uintptr_t)token) &&
        wifi_mgmt_subscribe(WIFI_MGMT_EVENT_CONNECT_FAILED,
                            network_wifi_event_cb,
                            (void *)(uintptr_t)token);

    wifi_mgmt_start();

    /* (review finding: start/stop transaction race) A stop() that ran while
     * the registration/start calls above were in flight disarmed this
     * session under the adapter lock (started -> false, stop_count ++).
     * This start must then abort and roll back instead of returning
     * NETWORK_OK for an adapter that a completed stop() already owns — the
     * subscription set after a completed stop() is always empty. */
    bool stopped_concurrently = false;
    if (network_lock())
    {
        if (s_ctx.stop_count != stop_count_before)
        {
            stopped_concurrently = true;
            network_disarm_locked();
        }
        network_unlock();
    }

    const bool ready = stopped_concurrently
                           ? false
                           : wifi_mgmt_wait_ready(NETWORK_START_TIMEOUT_MS);

    /* Re-check after the (possibly blocking) wait: the stop may have landed
     * during the wait instead of during the registration. */
    if (!stopped_concurrently && network_lock())
    {
        if (s_ctx.stop_count != stop_count_before)
        {
            stopped_concurrently = true;
            network_disarm_locked();
        }
        network_unlock();
    }

    if (!subscribed || !ready || stopped_concurrently)
    {
        osal_log_error("[net] Wi-Fi startup failed (subscribed=%d ready=%d "
                       "stopped=%d)",
                       (int)subscribed, (int)ready,
                       (int)stopped_concurrently);

        /* Roll back so nothing stays registered and no late event can reach
         * the (about to be released) context.  Re-take the lock for the
         * disarm: the registration was published under it and a late event
         * may already be delivering against it. */
        if (network_lock())
        {
            network_disarm_locked();
            network_unlock();
        }

        (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECTED,
                                    network_wifi_event_cb,
                                    (void *)(uintptr_t)token);
        (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_DISCONNECTED,
                                    network_wifi_event_cb,
                                    (void *)(uintptr_t)token);
        (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECT_FAILED,
                                    network_wifi_event_cb,
                                    (void *)(uintptr_t)token);
        (void)wifi_mgmt_stop();

        return NETWORK_ERR_START_FAILED;
    }

    /* Explicit connect request.  The manager also auto-connects when it
     * loaded persisted credentials; requesting here keeps the product
     * behavior independent of that internal detail.  A synchronously
     * rejected request is a startup failure: the same rollback/error path
     * applies, plus the disconnect-class transition (fail-off first, then
     * the application is informed) while the callbacks are still armed. */
    if (!wifi_mgmt_connect())
    {
        osal_log_error("[net] Wi-Fi connect request rejected");

        /* Disconnect-class transition while armed: forces the lamp output
         * inactive and informs the application state machine. */
        network_deliver_disconnected(token);

        /* Roll back exactly like a startup failure: nothing stays
         * registered, the adapter is left in the not-started state. */
        if (network_lock())
        {
            network_disarm_locked();
            network_unlock();
        }

        (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECTED,
                                    network_wifi_event_cb,
                                    (void *)(uintptr_t)token);
        (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_DISCONNECTED,
                                    network_wifi_event_cb,
                                    (void *)(uintptr_t)token);
        (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECT_FAILED,
                                    network_wifi_event_cb,
                                    (void *)(uintptr_t)token);
        (void)wifi_mgmt_stop();

        return NETWORK_ERR_START_FAILED;
    }

    /* (review finding: start/stop transaction race) The post-wait_ready
     * re-check above covers only the registration/wait_ready window: a
     * concurrent stop() can also complete AFTER it, exactly between the
     * connect request and this final return.  Re-validate under the lock: a
     * changed stop_count means a completed stop() disarmed this session and
     * already owns the stopped state, so this start must roll back (same
     * startup-failure path: disarm, unsubscribe with this session's token,
     * wifi_mgmt_stop()) instead of returning NETWORK_OK with subscriptions
     * still published for a stopped adapter. */
    bool stopped_in_connect_window = false;
    if (network_lock())
    {
        if (s_ctx.stop_count != stop_count_before)
        {
            stopped_in_connect_window = true;
            network_disarm_locked();
        }
        network_unlock();
    }

    if (stopped_in_connect_window)
    {
        osal_log_error("[net] Wi-Fi startup aborted by concurrent stop");

        (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECTED,
                                    network_wifi_event_cb,
                                    (void *)(uintptr_t)token);
        (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_DISCONNECTED,
                                    network_wifi_event_cb,
                                    (void *)(uintptr_t)token);
        (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECT_FAILED,
                                    network_wifi_event_cb,
                                    (void *)(uintptr_t)token);
        (void)wifi_mgmt_stop();

        return NETWORK_ERR_START_FAILED;
    }

    osal_log_info("[net] network manager started");
    return NETWORK_OK;
}

void network_manager_stop(void)
{
    if (!network_ensure_lock() || !network_lock())
    {
        return;
    }

    if (!s_ctx.started)
    {
        /* Stop-before-start / repeated stop: idempotent no-op. */
        network_unlock();
        return;
    }

    /* Disarm first (under the lock): any late Wi-Fi worker event that already
     * passed the manager's subscription snapshot is then dropped on delivery
     * instead of reaching an unregistered context — both by the started-flag
     * check and by the session-token check.  Holding the lock also joins an
     * in-flight callback before we proceed.  The token stays readable so the
     * unsubscribe below removes exactly this session's subscriptions. */

    /* (review finding: start/stop transaction race) A start() that is still
     * inside its platform registration transaction observes the bumped
     * stop_count under the lock after its wait/registration calls return,
     * then disarms its own session and rolls everything back — so a
     * completed stop() can never leave a live subscription behind and no
     * start can return NETWORK_OK for an already-stopped adapter. */
    s_ctx.stop_count++;
    const uint32_t token = s_ctx.active_token;
    network_disarm_locked();
    network_unlock();

    (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECTED,
                                network_wifi_event_cb,
                                (void *)(uintptr_t)token);
    (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_DISCONNECTED,
                                network_wifi_event_cb,
                                (void *)(uintptr_t)token);
    (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECT_FAILED,
                                network_wifi_event_cb,
                                (void *)(uintptr_t)token);

    /* Request a manager-side disconnect; the manager state machine owns the
     * actual Wi-Fi teardown. */
    (void)wifi_mgmt_disconnect();

    osal_log_info("[net] network manager stopped");
}

osal_mutex_id_t network_manager_test_get_lock(void)
{
    /* Test-only peek at the published adapter mutex (atomic acquire load,
     * same as network_lock()): NULL while the lock was never created. */
    return atomic_load_explicit(&s_ctx.lock, memory_order_acquire);
}

bool network_manager_is_connected(void)
{
    /* Deliberately lock-free so callbacks may call it (the delivery path
     * holds the adapter lock) and so a concurrent stop() can never be joined
     * by this query.  The lifecycle flag is an atomic read with acquire
     * ordering: it is written only by start/stop (release stores under the
     * adapter mutex), so the read is well-defined and the manager query
     * below remains the thread-safe source of truth for the actual
     * connection state.  In the window where a stop() has disarmed the
     * adapter but a start() is still inside its (aborting) transaction, the
     * armed flag is already false, so the query never reports a connection
     * for an adapter that is being torn down. */
    if (!atomic_load_explicit(&s_ctx.started, memory_order_acquire))
    {
        return false;
    }

    return wifi_mgmt_is_connected();
}

bool network_manager_wait_connected(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0u;

    while (!network_manager_is_connected())
    {
        if (timeout_ms == 0u)
        {
            /* Single immediate check requested. */
            return false;
        }

        if ((timeout_ms - waited_ms) < NETWORK_WAIT_POLL_INTERVAL_MS)
        {
            /* Final partial interval, then the deadline is exhausted. */
            (void)osal_task_delay_ms(timeout_ms - waited_ms);
            return network_manager_is_connected();
        }

        (void)osal_task_delay_ms(NETWORK_WAIT_POLL_INTERVAL_MS);
        waited_ms += NETWORK_WAIT_POLL_INTERVAL_MS;
    }

    return true;
}
