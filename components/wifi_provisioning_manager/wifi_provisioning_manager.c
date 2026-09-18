/**
 * @file wifi_provisioning_manager.c
 * @brief Product-owned Wi-Fi provisioning adapter (TASK-126)
 *
 * Implementation of the documented wifi_provisioning_manager.h product
 * contract on top of the platform provisioning application
 * (wifi_http_provisioning.h), the shared Mongoose process
 * (mongoose_process.h) and the platform Wi-Fi manager saved-credentials
 * query (wifi_managment.h).
 *
 * Design rules enforced here:
 *
 *   1. Narrow surface: the ONLY platform headers included are
 *      wifi_http_provisioning.h, mongoose_process.h and wifi_managment.h,
 *      and only the operations the product contract needs are used
 *      (start/stop/get_state, the runtime URL overrides under the
 *      test-only guard, MongooseProcess_IsRunning, wifi_mgmt_is_read_data).
 *      None of these platform types or include paths appear in the public
 *      header (include/wifi_provisioning_manager.h).
 *
 *   2. Secrecy: nothing that could contain an SSID, a password, a token or
 *      a provisioning URL with embedded credentials is ever accepted,
 *      stored or logged here.  The adapter logs state transitions, listener
 *      bind outcomes and error codes only.
 *
 *   3. Mongoose ownership: the portal rides the shared Mongoose process
 *      that also hosts MQTT/TLS.  start() checks MongooseProcess_IsRunning()
 *      up front and fails cleanly when the process is missing, and this
 *      module never calls MongooseProcess_Init()/Deinit() — stop() and
 *      deinit() close only the portal's own listeners and never tear the
 *      shared process down.
 *
 *   4. Serialization: init/deinit/start/stop are serialized by one OSAL
 *      mutex, so concurrent lifecycle calls can never interleave on the
 *      portal state or the test-only URL override buffers.  The adapter
 *      never executes its own logic on the Mongoose poll thread; the
 *      platform start/stop dispatch their listener work to the poll thread
 *      via MongooseProcess_Invoke() and block this CALLER until the poll
 *      thread completes it (the poll thread itself never blocks on the
 *      adapter).
 *
 *   5. Controller notification translation (TASK-132): init() registers a
 *      product notification handler with the platform automatic fallback
 *      controller (wifi_provisioning_controller_init_with_config(), the
 *      platform fallback-budget default from TASK-131 is left untouched).
 *      The handler honors the controller's session/generation token (a
 *      notification captured in a superseded lifecycle - after adapter
 *      deinit/re-init - is discarded) and translates transitions with a
 *      small adapter-private table into pending product events
 *      (STARTED/SUCCEEDED/FAILED) stored under the SAME adapter mutex; the
 *      supervisor consumes them via wifi_provisioning_manager_poll_event()
 *      (TASK-133).  The notification handler never calls back into the
 *      controller, never blocks on anything but the short adapter lock and
 *      never logs a credential, SSID, token or URL - only state codes.
 */

#include "wifi_provisioning_manager.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "mongoose_process.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "wifi_http_provisioning.h"
#include "wifi_managment.h"
#include "wifi_provisioning_controller.h"

/* --------------------------------------------------------------------- */
/* Configuration                                                          */
/* --------------------------------------------------------------------- */

/** @brief Maximum length (incl. terminator) of a test-only URL override. */
#define WIFI_PROVISIONING_MANAGER_URL_MAX_LEN 128u

/** @brief Bounded FIFO capacity for pending product provisioning events. */
#define WIFI_PROVISIONING_MANAGER_EVENT_QUEUE_LEN 8u

/* --------------------------------------------------------------------- */
/* Internal state                                                          */
/* --------------------------------------------------------------------- */

typedef struct wifi_provisioning_manager_ctx
{
  _Atomic(osal_mutex_id_t) lock;   /**< Published atomically (CAS-adopted). */
  atomic_bool initialized;         /**< init() completed, deinit() cleared. */

  /* Controller notification translation (TASK-132).  Everything below is
   * protected by @c lock. */
  uint32_t controller_session;     /**< Session/generation token of the
                                        current controller lifecycle,
                                        adopted from the first notification
                                        seen after init(). */
  bool     controller_session_set; /**< true once @c controller_session has
                                        been adopted. */
  bool     success_pending;        /**< true between a GRACE entry and the
                                        ONLINE retire: guards SUCCEEDED so a
                                        saved-credential connect
                                        (AWAITING_CONNECT -> RETIRING_AP ->
                                        ONLINE) is never mis-reported as a
                                        provisioning success. */
  wifi_provisioning_manager_event_t pending[WIFI_PROVISIONING_MANAGER_EVENT_QUEUE_LEN];
                                   /**< Pending product events, FIFO. */
  unsigned pending_head;           /**< Index of the oldest pending event. */
  unsigned pending_count;          /**< Number of pending events. */
#ifdef WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY
  /* Test/host-build-only listen-URL overrides, applied at the next start().
   * Protected by @c lock; never logged. */
  char http_url[WIFI_PROVISIONING_MANAGER_URL_MAX_LEN];
  bool http_url_set;
  char dns_url[WIFI_PROVISIONING_MANAGER_URL_MAX_LEN];
  bool dns_url_set;
#endif
} wifi_provisioning_manager_ctx_t;

static wifi_provisioning_manager_ctx_t s_ctx = {
    .lock        = NULL,
    .initialized = false,
};

/* --------------------------------------------------------------------- */
/* Lock helpers (lazy, race-free publication)                             */
/* --------------------------------------------------------------------- */

/**
 * @brief Create the adapter mutex before concurrent use is possible.
 *
 * The adapter owns no constructor, so the mutex is created lazily — but the
 * creation itself is race-free: an atomic compare-and-swap adopts exactly one
 * winner's mutex, and every loser destroys its duplicate before anyone can
 * observe it.  Once published the handle stays valid for the process lifetime
 * (it is never deleted, matching the network adapter precedent).
 *
 * @return true when the adapter lock is available.
 */
static bool wifi_provisioning_manager_ensure_lock(void)
{
    osal_mutex_id_t current =
        atomic_load_explicit(&s_ctx.lock, memory_order_acquire);
    if (current != NULL)
    {
        return true;
    }

    osal_mutex_id_t created = NULL;
    if (osal_mutex_create(&created, "wifi_prov_mgr") != OSAL_SUCCESS)
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
 *         wifi_provisioning_manager_unlock(); false means nothing was taken.
 */
static bool wifi_provisioning_manager_lock(void)
{
    osal_mutex_id_t lock =
        atomic_load_explicit(&s_ctx.lock, memory_order_acquire);
    return (lock != NULL) && (osal_mutex_take(lock) == OSAL_SUCCESS);
}

/** @brief Release the adapter lock after a successful
 *         wifi_provisioning_manager_lock(). */
static void wifi_provisioning_manager_unlock(void)
{
    osal_mutex_id_t lock = atomic_load_explicit(&s_ctx.lock,
                                                memory_order_acquire);
    if (lock != NULL)
    {
        (void)osal_mutex_give(lock);
    }
}

/* --------------------------------------------------------------------- */
/* Platform state mapping                                                 */
/* --------------------------------------------------------------------- */

/**
 * @brief Map the platform provisioning state onto the product state type.
 *
 * Explicit switch so the product enum never aliases a platform type through
 * a cast.  Unknown platform values map to ERROR (defensive default).
 */
static wifi_provisioning_manager_state_t
wifi_provisioning_manager_map_state(wifi_http_provisioning_state_t state)
{
    switch (state)
    {
        case WIFI_PROVISIONING_STOPPED:
            return WIFI_PROVISIONING_MANAGER_STOPPED;
        case WIFI_PROVISIONING_STARTING:
            return WIFI_PROVISIONING_MANAGER_STARTING;
        case WIFI_PROVISIONING_RUNNING:
            return WIFI_PROVISIONING_MANAGER_RUNNING;
        case WIFI_PROVISIONING_STOPPING:
            return WIFI_PROVISIONING_MANAGER_STOPPING;
        case WIFI_PROVISIONING_ERROR:
        default:
            return WIFI_PROVISIONING_MANAGER_ERROR;
    }
}

/* --------------------------------------------------------------------- */
/* Controller notification translation (TASK-132)                          */
/* --------------------------------------------------------------------- */

/**
 * @brief Enqueue a pending product provisioning event (FIFO).
 *
 * The caller must hold the adapter lock.  Bounded queue: on overflow the
 * oldest event is dropped (FIFO discipline) so the ordering of the
 * remaining stream - STARTED before SUCCEEDED - is preserved.
 */
static void wifi_provisioning_manager_enqueue_event(
    wifi_provisioning_manager_event_t event)
{
    unsigned tail;

    if (s_ctx.pending_count >= WIFI_PROVISIONING_MANAGER_EVENT_QUEUE_LEN)
    {
        s_ctx.pending_head = (s_ctx.pending_head + 1u) %
                             WIFI_PROVISIONING_MANAGER_EVENT_QUEUE_LEN;
        --s_ctx.pending_count;
        osal_log_warning("[prov_mgr] pending provisioning event queue "
                         "overflow: oldest event dropped");
    }
    tail = (s_ctx.pending_head + s_ctx.pending_count) %
           WIFI_PROVISIONING_MANAGER_EVENT_QUEUE_LEN;
    s_ctx.pending[tail] = event;
    ++s_ctx.pending_count;
}

/**
 * @brief Adapter-private translation table: controller state transitions to
 *        product provisioning events.
 *
 *   - entry into WIFI_PROVISIONING_CONTROLLER_PROVISIONING (fresh device or
 *     exhausted-credential fallback) -> STARTED,
 *   - WIFI_PROVISIONING_CONTROLLER_ONLINE reached after the retirement
 *     sequence (GRACE/RETIRING_AP) -> SUCCEEDED; the notification handler
 *     additionally guards SUCCEEDED with @c success_pending (set on the
 *     GRACE entry) so a saved-credential connect (which also retires the
 *     startup SoftAP through RETIRING_AP) is never reported as a
 *     provisioning success.
 *
 * Every other transition maps to NONE.  No platform type leaves this
 * function.
 */
static wifi_provisioning_manager_event_t
wifi_provisioning_manager_translate_transition(
    wifi_provisioning_controller_state_t previous,
    wifi_provisioning_controller_state_t current)
{
    switch (current)
    {
        case WIFI_PROVISIONING_CONTROLLER_PROVISIONING:
            return WIFI_PROVISIONING_MANAGER_EVENT_STARTED;

        case WIFI_PROVISIONING_CONTROLLER_ONLINE:
            if (previous == WIFI_PROVISIONING_CONTROLLER_GRACE ||
                previous == WIFI_PROVISIONING_CONTROLLER_RETIRING_AP)
            {
                return WIFI_PROVISIONING_MANAGER_EVENT_SUCCEEDED;
            }
            break;

        default:
            break;
    }
    return WIFI_PROVISIONING_MANAGER_EVENT_NONE;
}

/**
 * @brief Platform fallback-controller state-change notification handler
 *        (TASK-132).
 *
 * Platform contract: must not block and must not call back into the
 * controller.  It only takes the short adapter lock, discards notifications
 * from a stale controller lifecycle (a session/generation token that does
 * not match the current lifecycle, including anything delivered after
 * adapter deinit()), translates the transition with the adapter-private
 * table and stores the pending product event under the lock.  The
 * supervisor consumes the stored outcome with
 * #wifi_provisioning_manager_poll_event() (TASK-133); the adapter itself
 * never touches app_state.  Only non-sensitive state codes are logged.
 */
static void wifi_provisioning_manager_on_controller_state_changed(
    wifi_provisioning_controller_state_t previous,
    wifi_provisioning_controller_state_t current,
    uint32_t session,
    void *user_ctx)
{
    wifi_provisioning_manager_event_t event;
    (void)user_ctx;

    /* The adapter lock is the single store point; if it is unavailable (or
     * was never created) nothing can be stored, so the notification is
     * dropped. */
    if (!wifi_provisioning_manager_lock())
    {
        return;
    }

    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        /* Notification delivered after adapter deinit(): drop it. */
        wifi_provisioning_manager_unlock();
        return;
    }

    if (!s_ctx.controller_session_set)
    {
        /* First notification of the current lifecycle: adopt its token. */
        s_ctx.controller_session     = session;
        s_ctx.controller_session_set = true;
    }
    else if (s_ctx.controller_session != session)
    {
        /* Stale controller lifecycle (deinit/re-init superseded it). */
        wifi_provisioning_manager_unlock();
        return;
    }

    /* Track the provisioning success guard: a GRACE entry means the station
     * connected while the portal was up; leaving the flow (abort, connect
     * loss, explicit stop) clears it. */
    if (current == WIFI_PROVISIONING_CONTROLLER_GRACE)
    {
        s_ctx.success_pending = true;
    }
    else if (current == WIFI_PROVISIONING_CONTROLLER_PROVISIONING ||
             current == WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT ||
             current == WIFI_PROVISIONING_CONTROLLER_DISABLED)
    {
        s_ctx.success_pending = false;
    }

    event = wifi_provisioning_manager_translate_transition(previous, current);
    if ((event == WIFI_PROVISIONING_MANAGER_EVENT_SUCCEEDED) &&
        !s_ctx.success_pending)
    {
        /* ONLINE reached without the success grace window (saved-credential
         * connect): not a provisioning outcome. */
        event = WIFI_PROVISIONING_MANAGER_EVENT_NONE;
    }

    if (event != WIFI_PROVISIONING_MANAGER_EVENT_NONE)
    {
        wifi_provisioning_manager_enqueue_event(event);
        /* State codes only - never an SSID, password, token or URL. */
        osal_log_info("[prov_mgr] controller transition %d -> %d "
                      "(product event %d)",
                      (int)previous, (int)current, (int)event);
    }

    if (current == WIFI_PROVISIONING_CONTROLLER_ONLINE ||
        current == WIFI_PROVISIONING_CONTROLLER_DISABLED)
    {
        s_ctx.success_pending = false;
    }

    wifi_provisioning_manager_unlock();
}

/* --------------------------------------------------------------------- */
/* Adapter lifecycle                                                      */
/* --------------------------------------------------------------------- */

wifi_provisioning_manager_status_t wifi_provisioning_manager_init(void)
{
    bool first_init = false;

    if (!wifi_provisioning_manager_ensure_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_START_FAILED;
    }

    if (!wifi_provisioning_manager_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_START_FAILED;
    }

    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        first_init = true;
        atomic_store_explicit(&s_ctx.initialized, true, memory_order_release);
        /* Fresh lifecycle: no trusted controller session yet, empty queue. */
        s_ctx.controller_session     = 0u;
        s_ctx.controller_session_set = false;
        s_ctx.success_pending        = false;
        s_ctx.pending_head           = 0u;
        s_ctx.pending_count          = 0u;
        osal_log_info("[prov_mgr] wifi provisioning adapter initialized");
    }
    wifi_provisioning_manager_unlock();

    if (first_init)
    {
        /* Register the product notification handler (TASK-132).  The
         * controller commits DISABLED -> AWAITING_CONNECT (and, on a fresh
         * device, -> PROVISIONING) synchronously inside this call, so the
         * handler stores those transitions under the adapter lock while the
         * lock is free - do not hold it across the controller call.  The
         * platform fallback-budget default (TASK-131 Kconfig value) is
         * deliberately left untouched: only the callback and its context are
         * set.  The registration is best-effort: if the controller's
         * deferred worker cannot be created, the adapter still operates via
         * start()/stop() but translates no notifications. */
        wifi_provisioning_controller_config_t controller_config;

        memset(&controller_config, 0, sizeof(controller_config));
        controller_config.on_state_changed =
            wifi_provisioning_manager_on_controller_state_changed;
        controller_config.user_ctx = &s_ctx;
        if (!wifi_provisioning_controller_init_with_config(&controller_config))
        {
            osal_log_error("[prov_mgr] controller notification hook "
                           "registration failed");
        }
    }

    return WIFI_PROVISIONING_MANAGER_OK;
}

wifi_provisioning_manager_status_t wifi_provisioning_manager_deinit(void)
{
    bool was_initialized;

    if (!wifi_provisioning_manager_ensure_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED;
    }

    if (!wifi_provisioning_manager_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED;
    }

    was_initialized =
        atomic_load_explicit(&s_ctx.initialized, memory_order_acquire);

    /* Best-effort teardown: if the provisioning portal is (or was) active,
     * close only its own listeners.  The shared Mongoose process is never
     * deinitialized here.  A failed stop is logged but does not block the
     * adapter deinitialization. */
    if (wifi_http_provisioning_get_state() != WIFI_PROVISIONING_STOPPED)
    {
        if (wifi_http_provisioning_stop())
        {
            osal_log_info("[prov_mgr] provisioning portal stopped "
                          "(adapter deinit)");
        }
        else
        {
            osal_log_error("[prov_mgr] provisioning portal stop failed "
                           "during adapter deinit (state=%d)",
                           (int)wifi_http_provisioning_get_state());
        }
    }

    atomic_store_explicit(&s_ctx.initialized, false, memory_order_release);

    /* Invalidate the trusted controller session and drop any pending product
     * events: a late notification from the ended lifecycle is discarded and
     * no stale event survives into the next adapter lifecycle. */
    s_ctx.controller_session_set = false;
    s_ctx.success_pending        = false;
    s_ctx.pending_head           = 0u;
    s_ctx.pending_count          = 0u;

#ifdef WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY
    s_ctx.http_url_set = false;
    s_ctx.dns_url_set  = false;
#endif

    wifi_provisioning_manager_unlock();

    if (was_initialized)
    {
        /* End the platform controller lifecycle OUTSIDE the adapter lock:
         * controller_deinit() commits -> DISABLED and delivers its final
         * notification synchronously, and the handler takes the adapter
         * lock (it sees initialized==false and discards the notification).
         * Holding the adapter lock across that call would deadlock the
         * notification path. */
        wifi_provisioning_controller_deinit();
    }

    osal_log_info("[prov_mgr] wifi provisioning adapter deinitialized");
    return WIFI_PROVISIONING_MANAGER_OK;
}

/* --------------------------------------------------------------------- */
/* Portal lifecycle                                                       */
/* --------------------------------------------------------------------- */

wifi_provisioning_manager_status_t wifi_provisioning_manager_start(void)
{
    bool platform_ok;

    if (!wifi_provisioning_manager_ensure_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_START_FAILED;
    }

    if (!wifi_provisioning_manager_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_START_FAILED;
    }

    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        wifi_provisioning_manager_unlock();
        osal_log_error("[prov_mgr] start before init()");
        return WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED;
    }

    /* The portal rides the shared Mongoose process (which also hosts
     * MQTT/TLS).  Require it up front; never initialize or deinitialize it
     * from the adapter. */
    if (!MongooseProcess_IsRunning())
    {
        wifi_provisioning_manager_unlock();
        osal_log_error("[prov_mgr] provisioning start blocked: shared "
                       "Mongoose process not running");
        return WIFI_PROVISIONING_MANAGER_ERR_MONGOOSE_NOT_RUNNING;
    }

#ifdef WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY
    /* Test/host-build-only: hand the overrides to the platform so tests can
     * bind non-privileged high ports.  On the target build this block is
     * compiled out and the platform uses its compiled-in default listen
     * URLs.  The setter results are intentionally ignored: the platform
     * refuses a URL change while the portal is already running, which only
     * happens on an (idempotent) repeat start. */
    if (s_ctx.http_url_set)
    {
        (void)wifi_http_provisioning_set_http_url(s_ctx.http_url);
    }
    if (s_ctx.dns_url_set)
    {
        (void)wifi_http_provisioning_set_dns_url(s_ctx.dns_url);
    }
#endif

    platform_ok = wifi_http_provisioning_start();
    if (!platform_ok)
    {
        const wifi_http_provisioning_state_t platform_state =
            wifi_http_provisioning_get_state();
        /* A portal start failure observed by the adapter is a
         * provisioning-failed outcome for the supervisor (TASK-133); the
         * adapter stores it, it never acts on it directly. */
        wifi_provisioning_manager_enqueue_event(
            WIFI_PROVISIONING_MANAGER_EVENT_FAILED);
        wifi_provisioning_manager_unlock();
        /* Error code only — never the listen URLs. */
        osal_log_error("[prov_mgr] provisioning portal start failed "
                       "(platform_state=%d)",
                       (int)platform_state);
        return WIFI_PROVISIONING_MANAGER_ERR_START_FAILED;
    }

    wifi_provisioning_manager_unlock();
    /* Reached RUNNING: both listeners (HTTP portal + captive DNS) bound. */
    osal_log_info("[prov_mgr] provisioning portal running "
                  "(listeners bound)");
    return WIFI_PROVISIONING_MANAGER_OK;
}

wifi_provisioning_manager_status_t wifi_provisioning_manager_stop(void)
{
    bool controller_ok;
    bool platform_ok;

    if (!wifi_provisioning_manager_ensure_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED;
    }

    if (!wifi_provisioning_manager_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED;
    }

    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        wifi_provisioning_manager_unlock();
        osal_log_error("[prov_mgr] stop before init()");
        return WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED;
    }

    wifi_provisioning_manager_unlock();

    /* End the controller's portal lifecycle OUTSIDE the adapter lock
     * (TASK-133): wifi_provisioning_controller_stop() cancels any pending
     * success-grace timer, stops the HTTP/DNS listeners and requests the
     * STA-only temporary-AP retirement, and it delivers its final
     * state-change notification synchronously on this thread — the
     * notification handler takes the adapter lock, so holding it across the
     * call would deadlock (the same pattern as deinit()/controller_deinit()).
     * A controller that is already disabled (e.g. a credentialed device with
     * no active portal) reports false as a safe no-op, which is not an
     * adapter failure; a real retirement failure leaves the controller
     * recoverable and is reported by the listener close below. */
    controller_ok = wifi_provisioning_controller_stop();
    if (!controller_ok)
    {
        osal_log_warning("[prov_mgr] controller stop reported the portal "
                         "still recoverable (state=%d); closing listeners",
                         (int)wifi_provisioning_controller_get_state());
    }

    /* Belt-and-braces listener close (idempotent): guarantees no
     * provisioning listener is left on the shared Mongoose process even if
     * the controller path could not complete.  It runs UNDER the adapter
     * lock again so the platform start/stop transactions stay serialized
     * (the controller stop above is outside the lock by contract).  The
     * shared process (and with it MQTT/TLS) is never deinitialized. */
    if (!wifi_provisioning_manager_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED;
    }
    platform_ok = wifi_http_provisioning_stop();
    if (!platform_ok)
    {
        const wifi_http_provisioning_state_t platform_state =
            wifi_http_provisioning_get_state();
        wifi_provisioning_manager_unlock();
        osal_log_error("[prov_mgr] provisioning portal stop failed "
                       "(platform_state=%d)",
                       (int)platform_state);
        return WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED;
    }
    wifi_provisioning_manager_unlock();

    osal_log_info("[prov_mgr] provisioning portal stopped "
                  "(controller lifecycle ended)");
    return WIFI_PROVISIONING_MANAGER_OK;
}

/* --------------------------------------------------------------------- */
/* Queries                                                                */
/* --------------------------------------------------------------------- */

wifi_provisioning_manager_state_t wifi_provisioning_manager_get_state(void)
{
    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        return WIFI_PROVISIONING_MANAGER_STOPPED;
    }
    return wifi_provisioning_manager_map_state(
        wifi_http_provisioning_get_state());
}

bool wifi_provisioning_manager_is_active(void)
{
    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        return false;
    }
    return wifi_http_provisioning_get_state() == WIFI_PROVISIONING_RUNNING;
}

bool wifi_provisioning_manager_has_saved_credentials(void)
{
    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        return false;
    }
    return wifi_mgmt_is_read_data();
}

wifi_provisioning_manager_event_t wifi_provisioning_manager_poll_event(void)
{
    wifi_provisioning_manager_event_t event =
        WIFI_PROVISIONING_MANAGER_EVENT_NONE;

    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        /* Safe default after deinit(). */
        return WIFI_PROVISIONING_MANAGER_EVENT_NONE;
    }
    if (!wifi_provisioning_manager_ensure_lock() ||
        !wifi_provisioning_manager_lock())
    {
        return WIFI_PROVISIONING_MANAGER_EVENT_NONE;
    }
    if (s_ctx.pending_count > 0u)
    {
        event = s_ctx.pending[s_ctx.pending_head];
        s_ctx.pending_head = (s_ctx.pending_head + 1u) %
                             WIFI_PROVISIONING_MANAGER_EVENT_QUEUE_LEN;
        --s_ctx.pending_count;
    }
    wifi_provisioning_manager_unlock();
    return event;
}

/* --------------------------------------------------------------------- */
/* Test/host-build-only listen-URL overrides                              */
/* --------------------------------------------------------------------- */

#ifdef WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY

/**
 * @brief Common validation/shared storage for the URL overrides.
 *
 * @param[out] slot  Destination buffer (protected by the adapter lock).
 * @param[out] set   Destination "configured" flag (protected by the lock).
 * @param[in]  url   Candidate URL.
 * @return           true when stored.
 */
static bool wifi_provisioning_manager_store_url(char *slot, bool *set,
                                                const char *url)
{
    size_t len;

    if (url == NULL)
    {
        return false;
    }
    len = strlen(url);
    if ((len == 0U) || (len >= WIFI_PROVISIONING_MANAGER_URL_MAX_LEN))
    {
        return false;
    }

    if (!wifi_provisioning_manager_ensure_lock() ||
        !wifi_provisioning_manager_lock())
    {
        return false;
    }
    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        wifi_provisioning_manager_unlock();
        return false;
    }
    memcpy(slot, url, len);
    slot[len] = '\0';
    *set = true;
    wifi_provisioning_manager_unlock();
    return true;
}

bool wifi_provisioning_manager_set_http_url(const char *url)
{
    bool ok = wifi_provisioning_manager_store_url(
        s_ctx.http_url, &s_ctx.http_url_set, url);
    if (!ok)
    {
        osal_log_warning("[prov_mgr] HTTP listen-URL override rejected "
                         "(invalid or adapter not initialized)");
    }
    /* Never log the URL value itself. */
    return ok;
}

bool wifi_provisioning_manager_set_dns_url(const char *url)
{
    bool ok = wifi_provisioning_manager_store_url(
        s_ctx.dns_url, &s_ctx.dns_url_set, url);
    if (!ok)
    {
        osal_log_warning("[prov_mgr] DNS listen-URL override rejected "
                         "(invalid or adapter not initialized)");
    }
    /* Never log the URL value itself. */
    return ok;
}

#endif /* WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY */