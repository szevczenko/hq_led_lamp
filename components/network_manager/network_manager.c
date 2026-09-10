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
 *      reaching an unregistered context.  stop() joins an in-flight callback
 *      (the handler holds the same mutex during delivery), so no application
 *      callback can run after stop() returns.
 */

#include "network_manager.h"

#include <stddef.h>

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
  osal_mutex_id_t lock;              /**< Serializes start/stop/delivery.   */
  network_callbacks_t callbacks;     /**< Valid only while started.         */
  bool started;                      /**< Callbacks armed, API operational. */
  uint32_t generation;               /**< Bumped on every stop/failed start. */
} network_ctx_t;

static network_ctx_t s_ctx = {
  .lock       = NULL,
  .callbacks  = { NULL, NULL, NULL },
  .started    = false,
  .generation = 0u,
};

/* --------------------------------------------------------------------- */
/* Small lock helpers                                                     */
/* --------------------------------------------------------------------- */

/**
 * @brief Create the adapter mutex on first use.
 *
 * The adapter owns no constructor, so the mutex is created lazily.  The
 * documented lifecycle contract (a single lifecycle owner serializes
 * start()/stop()) makes this race-free in product use; the adopt-or-delete
 * pattern below additionally keeps a concurrent first start from leaking a
 * duplicate mutex.
 *
 * @return true when the adapter lock is available.
 */
static bool network_ensure_lock(void)
{
    if (s_ctx.lock != NULL)
    {
        return true;
    }

    osal_mutex_id_t created = NULL;
    if (osal_mutex_create(&created, "net_mgr") != OSAL_SUCCESS)
    {
        return false;
    }

    if (s_ctx.lock != NULL)
    {
        /* Another thread won the creation race; drop our duplicate. */
        (void)osal_mutex_delete(created);
        return true;
    }

    s_ctx.lock = created;
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
    return (s_ctx.lock != NULL) && (osal_mutex_take(s_ctx.lock) == OSAL_SUCCESS);
}

/** @brief Release the adapter lock after a successful network_lock(). */
static void network_unlock(void)
{
    (void)osal_mutex_give(s_ctx.lock);
}

/**
 * @brief Shared registration/rollback core: clear the callback table and
 *        bump the generation so any in-flight or late event is stale.
 *
 * @note Called with the lock held.
 */
static void network_disarm_locked(void)
{
    s_ctx.callbacks.on_connected    = NULL;
    s_ctx.callbacks.on_disconnected = NULL;
    s_ctx.callbacks.context         = NULL;
    s_ctx.started    = false;
    s_ctx.generation += 1u;
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
 */
static void network_deliver_connected(void)
{
    if (!network_lock())
    {
        return;
    }

    if (!s_ctx.started || (s_ctx.callbacks.on_connected == NULL))
    {
        /* Late callback racing stop(): the registration is already gone. */
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
 */
static void network_deliver_disconnected(void)
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

    if (!s_ctx.started || (s_ctx.callbacks.on_disconnected == NULL))
    {
        /* Late callback racing stop(): dropped, the lamp is already off. */
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
 */
static void network_wifi_event_cb(wifi_mgmt_event_t event, void *user_data)
{
    (void)user_data;

    switch (event)
    {
        case WIFI_MGMT_EVENT_CONNECTED:
            network_deliver_connected();
            break;

        case WIFI_MGMT_EVENT_DISCONNECTED:
        case WIFI_MGMT_EVENT_CONNECT_FAILED:
            network_deliver_disconnected();
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

    if (s_ctx.started)
    {
        network_unlock();
        return NETWORK_ERR_ALREADY_STARTED;
    }

    s_ctx.callbacks = *callbacks;
    s_ctx.started   = true;
    network_unlock();

    /* Wi-Fi onboarding must complete before anything downstream (ThingsBoard)
     * may begin connecting.  Station (client) mode is the product role; the
     * persistence schema inside the manager is none of this layer's business. */
    wifi_mgmt_set_wifi_type(T_WIFI_TYPE_CLIENT);
    wifi_mgmt_init();

    const bool subscribed =
        wifi_mgmt_subscribe(WIFI_MGMT_EVENT_CONNECTED,
                            network_wifi_event_cb, NULL) &&
        wifi_mgmt_subscribe(WIFI_MGMT_EVENT_DISCONNECTED,
                            network_wifi_event_cb, NULL) &&
        wifi_mgmt_subscribe(WIFI_MGMT_EVENT_CONNECT_FAILED,
                            network_wifi_event_cb, NULL);

    wifi_mgmt_start();
    const bool ready = wifi_mgmt_wait_ready(NETWORK_START_TIMEOUT_MS);

    if (!subscribed || !ready)
    {
        osal_log_error("[net] Wi-Fi startup failed (subscribed=%d ready=%d)",
                       (int)subscribed, (int)ready);

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
                                    network_wifi_event_cb, NULL);
        (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_DISCONNECTED,
                                    network_wifi_event_cb, NULL);
        (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECT_FAILED,
                                    network_wifi_event_cb, NULL);
        (void)wifi_mgmt_stop();

        return NETWORK_ERR_START_FAILED;
    }

    /* Explicit connect request.  The manager also auto-connects when it
     * loaded persisted credentials; requesting here keeps the product
     * behavior independent of that internal detail. */
    (void)wifi_mgmt_connect();

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
     * instead of reaching an unregistered context.  Holding the lock also
     * joins an in-flight callback before we proceed. */
    network_disarm_locked();
    network_unlock();

    (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECTED,
                                network_wifi_event_cb, NULL);
    (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_DISCONNECTED,
                                network_wifi_event_cb, NULL);
    (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECT_FAILED,
                                network_wifi_event_cb, NULL);

    /* Request a manager-side disconnect; the manager state machine owns the
     * actual Wi-Fi teardown. */
    (void)wifi_mgmt_disconnect();

    osal_log_info("[net] network manager stopped");
}

bool network_manager_is_connected(void)
{
    /* Deliberately lock-free so callbacks may call it (the delivery path
     * holds the adapter lock).  s_started is a single flag written only by
     * start/stop; the manager query is its own thread-safe source of truth. */
    if (!s_ctx.started)
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
