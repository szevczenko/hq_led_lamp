/**
 * @file network_manager.h
 * @brief Product-owned network adapter around the platform Wi-Fi manager
 *        (TASK-109)
 *
 * Normative public API for the network adapter.  This is the ONLY network
 * header product-domain modules may include.
 *
 * Purpose
 * -------
 * The platform Wi-Fi manager (hq_platform `wifi` component) is treated as an
 * external dependency with a rich, manager-internal API: mode selection,
 * scans, credentials, persisted configuration schema, IP snapshots.  Product
 * code must not see any of that.  This adapter narrows the platform manager
 * down to the documented product contract:
 *
 *   @code
 *   typedef struct {
 *       void (*on_connected)(void *context);
 *       void (*on_disconnected)(void *context);
 *       void *context;
 *   } network_callbacks_t;
 *
 *   int  network_manager_start(const network_callbacks_t *callbacks);
 *   bool network_manager_is_connected(void);
 *   @endcode
 *
 * plus a blocking connection gate (network_manager_wait_connected()) that
 * orders Wi-Fi onboarding before ThingsBoard initialization: the ThingsBoard
 * client may only connect after this gate reports an established network
 * connection.
 *
 * Safety contract (fail-off)
 * --------------------------
 * The adapter forces the lamp output inactive synchronously, before the
 * application state machine is informed, on every disconnect-class event
 * (connection lost or connect attempt failed).  A Wi-Fi loss therefore drives
 * the electrical output off within application latency even if the
 * application callback is slow or never registered.
 *
 * Secrecy contract
 * ----------------
 * Wi-Fi credentials and the Wi-Fi manager persistence schema (wifi_ap.json,
 * credential lists, SSID/password buffers) are internal to the platform
 * manager.  This API never accepts, returns, stores or logs them; the adapter
 * logs transition states only.  Product modules must not include platform
 * Wi-Fi headers.
 *
 * Threading and ownership contract
 * --------------------------------
 *   - network_manager_start() / network_manager_stop() are serialized with
 *     each other and with callback delivery by an internal OSAL mutex.  They
 *     are intended to be called from a single lifecycle owner (app_main).
 *   - Callbacks run in the Wi-Fi manager worker task context.  They must be
 *     short and non-blocking and must NOT call network_manager_start() or
 *     network_manager_stop() (they would deadlock on the adapter lock).
 *     Calling network_manager_is_connected() from a callback is allowed and
 *     lock-free.
 *   - The adapter copies the function pointers but does NOT own @c context.
 *     Ownership stays with the integrator: @c context must remain valid from
 *     a successful network_manager_start() until network_manager_stop()
 *     returns.
 *   - Late-callback behavior: the Wi-Fi worker snapshots subscriptions before
 *     dispatching, so an event that raced with network_manager_stop() may
 *     still arrive afterwards.  Every subscription carries a per-start
 *     generation token (passed through the manager's user_data); the handler
 *     re-validates registration AND token under the adapter lock and drops
 *     every stale event — including an event snapshot taken before a
 *     stop()/start() cycle, which carries the previous session's token and is
 *     never delivered to the new session's callbacks/context.  After stop()
 *     returns, no application callback runs again, so @c context is safe to
 *     release.  A stale disconnect-class event still performs the (idempotent,
 *     safe) lamp fail-off before it is dropped.
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Configuration constants                                                */
/* --------------------------------------------------------------------- */

/** @brief Maximum time [ms] network_manager_start() waits for Wi-Fi startup. */
#define NETWORK_START_TIMEOUT_MS   10000u

/**
 * @brief Default maximum time [ms] network_manager_wait_connected() blocks
 *        for the first (or next) connection when the integrator does not
 *        override the timeout.
 */
#define NETWORK_CONNECT_TIMEOUT_MS 30000u

/** @brief Poll interval [ms] between connected-state checks while waiting. */
#define NETWORK_WAIT_POLL_INTERVAL_MS 50u

/* --------------------------------------------------------------------- */
/* Result codes                                                           */
/* --------------------------------------------------------------------- */

/**
 * @brief Portable result/error type returned by the start operation.
 *
 * #NETWORK_OK (0) signals success; every other value is an error code.
 * Values are stable: callers compare against the symbolic names.
 */
typedef enum network_status {
    NETWORK_OK                   = 0,  /**< Network started successfully.      */
    NETWORK_ERR_INVALID_ARGUMENT = -1, /**< NULL callbacks or missing handler. */
    NETWORK_ERR_ALREADY_STARTED  = -2, /**< start() called while already started. */
    NETWORK_ERR_START_FAILED     = -3  /**< Wi-Fi startup or subscribe failed. */
} network_status_t;

/* --------------------------------------------------------------------- */
/* Callback interface (documented product contract)                       */
/* --------------------------------------------------------------------- */

/**
 * @brief Connect/disconnect notification callbacks with an opaque context.
 *
 * Both handlers are mandatory.  @c context is forwarded verbatim to every
 * invocation; the adapter never dereferences or copies it.
 */
typedef struct network_callbacks {
    void (*on_connected)(void *context);    /**< Network connection established. */
    void (*on_disconnected)(void *context); /**< Network lost or connect failed. */
    void *context;                          /**< Opaque user context.            */
} network_callbacks_t;

/* --------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* --------------------------------------------------------------------- */

/**
 * @brief   Start the network: bring up the Wi-Fi manager in the
 *          Kconfig-selected default mode (KLC_WIFI_DEFAULT_MODE, AP+STA by
 *          default) and request a connection with the persisted credentials.
 *
 * @details The start mode is a product policy decision owned by this adapter
 *          (the network stage is the single Wi-Fi owner): AP+STA is the
 *          default so the provisioning portal's soft-AP is available when no
 *          usable credentials exist; station-only is selectable for hardened
 *          builds.  The mode never appears in this header — it is set inside
 *          the adapter before onboarding.
 *
 *          Ordering contract: this operation runs Wi-Fi onboarding to
 *          completion (manager initialized, started, ready) before it
 *          returns, and a ThingsBoard connect may only be attempted after
 *          network_manager_wait_connected() has reported a connection.
 *
 *          The registered callbacks remain armed until network_manager_stop()
 *          completes.  A start failure rolls everything back: subscriptions
 *          are removed, callbacks are disarmed and the adapter is left in the
 *          not-started state, from which a fresh start() is allowed.
 *
 * @param   [in] callbacks - connection callbacks with an opaque context; both
 *                           handlers must be non-NULL, @c context may be NULL.
 *
 * @return  #NETWORK_OK on success,
 *          #NETWORK_ERR_INVALID_ARGUMENT on a NULL/incomplete callback set,
 *          #NETWORK_ERR_ALREADY_STARTED when called while started,
 *          #NETWORK_ERR_START_FAILED when the Wi-Fi manager could not be
 *          brought up (nothing stays registered in that case).
 *
 * @note    Thread-safe against stop() and against callback delivery.
 */
int network_manager_start(const network_callbacks_t *callbacks);

/**
 * @brief   Stop the network adapter and disarm the callbacks.
 *
 * @details Blocks until an in-flight callback has finished, then unsubscribes
 *          from the Wi-Fi manager events, disarms the callbacks and requests
 *          a manager disconnect.  After this function returns no application
 *          callback can run again, so the context may be released safely.
 *          Stop-before-start and repeated stops are no-ops.  Stopping does
 *          not destroy the adapter: a fresh network_manager_start() with new
 *          callbacks is allowed at any time.
 */
void network_manager_stop(void);

/**
 * @brief   Query the current network connection state.
 *
 * @return  true when the adapter is started and the Wi-Fi manager reports an
 *          established station connection; false otherwise (including after
 *          stop() or a failed start()).
 *
 * @note    Lock-free and therefore legal from inside a network callback.
 */
bool network_manager_is_connected(void);

/**
 * @brief   Block until the network is connected or the timeout elapses.
 *
 * @details This is the ThingsBoard ordering gate: a ThingsBoard connect may
 *          only be attempted after this function returned true.
 *
 * @param   [in] timeout_ms - maximum time to wait in milliseconds
 *                            (#NETWORK_CONNECT_TIMEOUT_MS is the product
 *                            default; 0 performs a single immediate check).
 *
 * @return  true once the network is connected; false on timeout or when the
 *          adapter is not started (a stopped adapter can never report a
 *          connection).
 */
bool network_manager_wait_connected(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_MANAGER_H */
