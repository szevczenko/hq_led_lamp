/**
 * @file wifi_provisioning_manager.h
 * @brief Product-owned Wi-Fi provisioning adapter (TASK-126)
 *
 * Normative public API of the product-owned provisioning adapter.  This is
 * the ONLY provisioning header product-domain modules may include.
 *
 * Purpose
 * -------
 * The platform provisioning application (hq_platform `wifi_provisioning`
 * component: `wifi_http_provisioning_start/stop/get_state`, captive DNS,
 * HTTP portal on the shared Mongoose process) is treated as an external
 * dependency with a platform-internal API (platform lifecycle states,
 * Mongoose listen URLs, listener internals).  Product code must not see any
 * of that.  This adapter narrows the platform provisioning application down
 * to the documented product contract:
 *
 *   @code
 *   wifi_provisioning_manager_init/deinit();   // adapter lifecycle
 *   wifi_provisioning_manager_start/stop();    // portal lifecycle
 *   wifi_provisioning_manager_get_state();     // portal state query
 *   wifi_provisioning_manager_is_active();     // is-portal-active query
 *   wifi_provisioning_manager_has_saved_credentials();  // provisioning decision
 *   @endcode
 *
 * No Wi-Fi, Mongoose or provisioning platform type appears in this header:
 * the platform include paths (and with them the platform state types, the
 * Mongoose listen-URL machinery and the Wi-Fi manager credential/schema
 * internals) never propagate to product-domain dependents.  The adapter
 * never accepts, stores or logs an SSID, a password, a token or a
 * provisioning URL.
 *
 * Product decision ownership
 * --------------------------
 * Since TASK-131 the platform's automatic fallback controller
 * (CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK=y with a bounded
 * CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS budget, see
 * sdkconfig.defaults) is COMPILED IN.  Since TASK-132 the adapter registers
 * its product notification handler with the controller
 * (wifi_provisioning_controller_init_with_config()) and translates the
 * controller's state-change notifications into product provisioning events
 * (STARTED/SUCCEEDED/FAILED) consumed through
 * wifi_provisioning_manager_poll_event(); the supervisor wiring that acts on
 * those events is TASK-133.  The adapter still exposes
 * wifi_provisioning_manager_has_saved_credentials() so the start-provisioning
 * decision remains explicit at the product level.
 *
 * Mongoose process ownership
 * --------------------------
 * The provisioning portal rides the shared Mongoose process that also hosts
 * MQTT/TLS.  start() REQUIRES the shared process to be running
 * (MongooseProcess_IsRunning) and fails cleanly otherwise; the adapter never
 * initializes or deinitializes the shared process.  stop() (and deinit())
 * only close the listeners owned by the provisioning portal and never tear
 * the shared process down.
 *
 * Threading contract
 * ------------------
 * init/deinit/start/stop are serialized against each other by an internal
 * OSAL mutex and are idempotent.  The adapter never runs its own logic on
 * the Mongoose poll thread and never blocks the poll thread on a caller
 * context: the platform's start/stop dispatch their listener operations to
 * the poll thread via MongooseProcess_Invoke() and block the CALLER until
 * the poll thread completes them.
 */

#ifndef WIFI_PROVISIONING_MANAGER_H
#define WIFI_PROVISIONING_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Result codes                                                           */
/* --------------------------------------------------------------------- */

/**
 * @brief Portable result/error type returned by the adapter operations.
 *
 * #WIFI_PROVISIONING_MANAGER_OK (0) signals success; every other value is
 * an error code.  Values are stable: callers compare against the symbolic
 * names.
 */
typedef enum wifi_provisioning_manager_status
{
    WIFI_PROVISIONING_MANAGER_OK                      = 0,
    /**< Operation succeeded (including idempotent repeats). */
    WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED     = -1,
    /**< Adapter used before init() or after deinit(). */
    WIFI_PROVISIONING_MANAGER_ERR_INVALID_ARGUMENT    = -2,
    /**< NULL/empty/over-long argument to a (test-only) setter. */
    WIFI_PROVISIONING_MANAGER_ERR_MONGOOSE_NOT_RUNNING = -3,
    /**< start() requires the shared Mongoose process to be running. */
    WIFI_PROVISIONING_MANAGER_ERR_START_FAILED        = -4,
    /**< The platform provisioning application refused to start. */
    WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED         = -5
    /**< The platform provisioning application refused to stop. */
} wifi_provisioning_manager_status_t;

/* --------------------------------------------------------------------- */
/* Portal state (product-owned mirror)                                     */
/* --------------------------------------------------------------------- */

/**
 * @brief Provisioning portal lifecycle state, as reported by the adapter.
 *
 * Product-owned mirror of the platform lifecycle; no platform type is
 * exposed.  The portal reaches #WIFI_PROVISIONING_MANAGER_RUNNING only after
 * both the HTTP portal listener and the captive DNS listener are bound.
 */
typedef enum wifi_provisioning_manager_state
{
    WIFI_PROVISIONING_MANAGER_STOPPED  = 0, /**< Portal stopped.           */
    WIFI_PROVISIONING_MANAGER_STARTING = 1, /**< Listeners being opened.   */
    WIFI_PROVISIONING_MANAGER_RUNNING  = 2, /**< HTTP + DNS listeners up.  */
    WIFI_PROVISIONING_MANAGER_STOPPING = 3, /**< Listeners being closed.   */
    WIFI_PROVISIONING_MANAGER_ERROR    = 4  /**< Lifecycle error.          */
} wifi_provisioning_manager_state_t;

/* --------------------------------------------------------------------- */
/* Provisioning outcome events (product-owned mirror, TASK-132)            */
/* --------------------------------------------------------------------- */

/**
 * @brief Provisioning outcome reported by #wifi_provisioning_manager_poll_event.
 *
 * Product-owned mirror of the platform fallback controller's state-change
 * notifications (TASK-132); no controller or platform type is exposed.  The
 * product events are produced by the adapter's private translation table:
 *
 *   - #WIFI_PROVISIONING_MANAGER_EVENT_STARTED — the controller entered the
 *     provisioning flow (fresh device, or the fallback budget was
 *     exhausted by failing saved credentials),
 *   - #WIFI_PROVISIONING_MANAGER_EVENT_SUCCEEDED — the controller reached
 *     ONLINE through the success grace/retire path (the station connected
 *     while the portal was up and the temporary AP was retired),
 *   - #WIFI_PROVISIONING_MANAGER_EVENT_FAILED — a portal start failure
 *     observed by the adapter (the platform provisioning application
 *     refused to open its listeners).
 *
 * Events are consumed one at a time, in the order they were recorded, with
 * #wifi_provisioning_manager_poll_event(); #WIFI_PROVISIONING_MANAGER_EVENT_NONE
 * means "no pending event".
 */
typedef enum wifi_provisioning_manager_event
{
    WIFI_PROVISIONING_MANAGER_EVENT_NONE      = 0, /**< No pending event. */
    WIFI_PROVISIONING_MANAGER_EVENT_STARTED   = 1, /**< Provisioning flow started. */
    WIFI_PROVISIONING_MANAGER_EVENT_SUCCEEDED = 2, /**< Provisioning succeeded. */
    WIFI_PROVISIONING_MANAGER_EVENT_FAILED    = 3  /**< Provisioning failed. */
} wifi_provisioning_manager_event_t;

/* --------------------------------------------------------------------- */
/* Adapter lifecycle                                                      */
/* --------------------------------------------------------------------- */

/**
 * @brief   Initialize the provisioning adapter (idempotent).
 *
 * @details Prepares the adapter's internal synchronization state.  It does
 *          NOT touch the shared Mongoose process, Wi-Fi state or any
 *          listener: the portal only comes up when start() is called.
 *          Calling init() while already initialized is a safe no-op.
 *
 * @return  #WIFI_PROVISIONING_MANAGER_OK on success.
 */
wifi_provisioning_manager_status_t wifi_provisioning_manager_init(void);

/**
 * @brief   Deinitialize the provisioning adapter (idempotent).
 *
 * @details If the provisioning portal is still active it is stopped first
 *          (best-effort, listeners only); the shared Mongoose process is
 *          never deinitialized.  After deinit() every adapter operation
 *          reports #WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED and the
 *          queries return their safe defaults, until init() is called
 *          again.  Calling deinit() while already deinitialized is a safe
 *          no-op.
 *
 * @return  #WIFI_PROVISIONING_MANAGER_OK.
 */
wifi_provisioning_manager_status_t wifi_provisioning_manager_deinit(void);

/* --------------------------------------------------------------------- */
/* Portal lifecycle                                                       */
/* --------------------------------------------------------------------- */

/**
 * @brief   Start the Wi-Fi provisioning portal (idempotent, serialized).
 *
 * @details Requires the adapter to be initialized and the shared Mongoose
 *          process to be running; both are checked under the adapter lock
 *          and the portal is only handed to the platform afterwards.  On
 *          the target the platform's compiled-in default listen URLs are
 *          used; listen-URL overrides are a test/host-build-only feature
 *          (see the WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY section).
 *          Repeated calls while the portal is already running are safe
 *          no-ops.
 *
 * @return  #WIFI_PROVISIONING_MANAGER_OK when the portal is (or already
 *          was) running,
 *          #WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED after deinit(),
 *          #WIFI_PROVISIONING_MANAGER_ERR_MONGOOSE_NOT_RUNNING when the
 *          shared Mongoose process is not running,
 *          #WIFI_PROVISIONING_MANAGER_ERR_START_FAILED when the platform
 *          refused to open the listeners.
 */
wifi_provisioning_manager_status_t wifi_provisioning_manager_start(void);

/**
 * @brief   Stop the Wi-Fi provisioning portal (idempotent, serialized).
 *
 * @details Closes only the listeners owned by the provisioning portal and
 *          never tears down the shared Mongoose process (MQTT/TLS stay
 *          untouched).  Requires the adapter to be initialized; a stop
 *          with the portal already stopped is a safe no-op.
 *
 * @return  #WIFI_PROVISIONING_MANAGER_OK when the portal is (or already
 *          was) stopped,
 *          #WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED after deinit(),
 *          #WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED when a listener
 *          could not be closed while the shared process is still running.
 */
wifi_provisioning_manager_status_t wifi_provisioning_manager_stop(void);

/* --------------------------------------------------------------------- */
/* Queries                                                                */
/* --------------------------------------------------------------------- */

/**
 * @brief   Query the current provisioning portal state.
 *
 * @return  The mapped product state; #WIFI_PROVISIONING_MANAGER_STOPPED
 *          when the adapter is not initialized (safe default).
 *
 * @note    Lock-free; safe to call from any context.
 */
wifi_provisioning_manager_state_t wifi_provisioning_manager_get_state(void);

/**
 * @brief   Query whether the provisioning portal is active (running).
 *
 * @return  true exactly when the portal is in the RUNNING state
 *          (HTTP portal + captive DNS listeners bound); false otherwise,
 *          including after stop() and after deinit().
 *
 * @note    Lock-free; safe to call from any context.
 */
bool wifi_provisioning_manager_is_active(void);

/**
 * @brief   Query whether the station has a saved credential.
 *
 * @details Product decision hook: app_main uses this to decide when to
 *          start provisioning (a station with a saved credential must
 *          never enter the provisioning flow).  Wraps the platform Wi-Fi
 *          manager's saved-credentials query; no credential content ever
 *          crosses this API.
 *
 * @return  true when a saved station credential was loaded from persistent
 *          storage; otherwise false (including when the adapter is not
 *          initialized — safe default).
 *
 * @note    Lock-free; safe to call from any context.
 */
bool wifi_provisioning_manager_has_saved_credentials(void);

/**
 * @brief   Poll the next pending provisioning outcome event.
 *
 * @details Consumes the product events produced from the platform fallback
 *          controller notifications (TASK-132) one at a time, in the order
 *          they were recorded: a provisioning flow is reported as
 *          #WIFI_PROVISIONING_MANAGER_EVENT_STARTED before
 *          #WIFI_PROVISIONING_MANAGER_EVENT_SUCCEEDED — never reordered.
 *          Notifications from a stale controller lifecycle (delivered after
 *          adapter deinit()/re-init()) are discarded by the adapter and are
 *          never returned here.  The supervisor (TASK-133) is the intended
 *          consumer; the adapter itself never touches app_state.
 *
 * @return  The next pending event, or #WIFI_PROVISIONING_MANAGER_EVENT_NONE
 *          when no event is pending (including after deinit() — safe
 *          default).
 */
wifi_provisioning_manager_event_t wifi_provisioning_manager_poll_event(void);

#ifdef WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY
/* --------------------------------------------------------------------- */
/* Test/host-build-only listen-URL overrides                              */
/* --------------------------------------------------------------------- */

/**
 * @brief   Test-only: override the HTTP listen URL used by the next
 *          start().
 *
 * @details Compiled only into tests/host builds (guarded by
 *          WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY): the production
 *          target always uses the compiled-in default listener URLs.  The
 *          URL is applied (via the platform runtime override) at the next
 *          start(); setting it never logs the value.  The adapter never
 *          accepts credential-bearing URLs on the target build because the
 *          whole setter is compiled out there.
 *
 * @param[in] url Non-NULL, non-empty Mongoose HTTP listen URL (e.g.
 *                "http://127.0.0.1:8080").
 * @return true when accepted and applied at the next start(); false when
 *         NULL/empty/over-long.
 */
bool wifi_provisioning_manager_set_http_url(const char *url);

/**
 * @brief   Test-only: override the captive DNS listen URL used by the next
 *          start().
 *
 * @details See wifi_provisioning_manager_set_http_url().  The URL must be a
 *          Mongoose UDP listen URL (e.g. "udp://127.0.0.1:10053").
 *
 * @param[in] url Non-NULL, non-empty Mongoose UDP listen URL.
 * @return true when accepted and applied at the next start(); false when
 *         NULL/empty/over-long.
 */
bool wifi_provisioning_manager_set_dns_url(const char *url);
#endif /* WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY */

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PROVISIONING_MANAGER_H */