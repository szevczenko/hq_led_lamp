/**
 * @file app_state.h
 * @brief Application state machine and watchdog policy (TASK-115)
 *
 * Normative public API of the product-owned application state owner.
 *
 * This component turns the boot sequence "boot -> safe-off -> filesystem ->
 * configuration -> Wi-Fi -> provisioning (only when no saved station
 * credential exists) -> verified MQTT/TLS -> state sync -> online" and
 * every error path into ONE explicit state machine, and attaches the
 * application watchdog policy to it.  Events cross the Wi-Fi, MQTT,
 * ThingsBoard, timer, OTA and application contexts; this module is the
 * single owner of the state, of the legal-transition table, of the stale-
 * callback (generation/session) rejection and of the bounded retry/backoff
 * schedule.  Exactly one task — the integrator's supervisor loop, the only
 * caller of app_state_poll() — owns watchdog feeding.
 *
 * State machine (normative)
 * -------------------------
 *   - #APP_STATE_BOOT           — entry state after init/start; output off.
 *   - #APP_STATE_FILESYSTEM     — waiting for the LittleFS bootstrap result.
 *   - #APP_STATE_CONFIGURATION  — waiting for product config validation.
 *   - #APP_STATE_NETWORK        — waiting for the Wi-Fi connection gate;
 *                                 may enter #APP_STATE_PROVISIONING when no
 *                                 saved station credential exists.
 *   - #APP_STATE_PROVISIONING   — Wi-Fi provisioning active (bounded retry
 *                                 on failure); output off; a device with
 *                                 saved credentials never enters it.
 *   - #APP_STATE_TLS            — waiting for verified MQTT/TLS.
 *   - #APP_STATE_SYNC           — waiting for ThingsBoard desired-state sync.
 *   - #APP_STATE_ONLINE         — all gates passed; output may be active.
 *   - #APP_STATE_SAFE_OFF       — degraded; every failure path converges
 *                                 here; output is off; bounded retry only.
 *   - #APP_STATE_FATAL          — unrecoverable; output off; only an external
 *                                 reset returns to boot.
 *   - #APP_STATE_OTA            — OTA in progress; output off; completion
 *                                 returns to boot; failure degrades.
 *
 * The only legal path into #APP_STATE_ONLINE requires a successful
 * filesystem, configuration, Wi-Fi (including Wi-Fi provisioning when no
 * saved station credential exists — a device with saved credentials may
 * pass straight through without entering PROVISIONING), verified TLS AND
 * synchronization — nothing shorter can reach online.  The lamp output is
 * forced inactive on
 * EVERY entry into a non-online state (the module calls
 * lamp_control_force_inactive() itself, see "Fail-off contract"), so the
 * DoD "every failure path forces the output inactive" is an invariant of
 * the transition machinery, not a caller obligation.
 *
 * Legal transitions, events and transition owners
 * -----------------------------------------------
 * Each transition is owned by the context that may lawfully fire its event.
 * The owner is validated per delivery: a callback from a context that does
 * not own the transition is rejected as illegal.  The table (from --
 * event (owner) --> to):
 *
 *   BOOT        -- start (BOOTSTRAP) -------------> FILESYSTEM
 *   FILESYSTEM  -- FS_OK (FILESYSTEM) ------------> CONFIGURATION
 *   FILESYSTEM  -- FS_FAIL (FILESYSTEM) ----------> SAFE_OFF      (degraded)
 *   FILESYSTEM  -- DISCONNECTED (NETWORK/MQTT) ---> SAFE_OFF      (retry NETWORK)
 *   CONFIGURATION-- CONFIG_OK (CONFIGURATION) ----> NETWORK
 *   CONFIGURATION-- CONFIG_FAIL (CONFIGURATION) --> SAFE_OFF      (degraded)
 *   CONFIGURATION-- DISCONNECTED (NETWORK/MQTT) --> SAFE_OFF      (retry NETWORK)
 *   NETWORK     -- NETWORK_CONNECTED (NETWORK) --> TLS
 *   NETWORK     -- NETWORK_FAILED (NETWORK) ------> SAFE_OFF      (retry NETWORK)
 *   NETWORK     -- PROVISIONING_STARTED (NETWORK) -> PROVISIONING (no saved credential)
 *   PROVISIONING-- PROVISIONING_SUCCEEDED (NETWORK) -> NETWORK    (credential saved)
 *   PROVISIONING-- PROVISIONING_FAILED (NETWORK) ----> SAFE_OFF   (retry NETWORK)
 *   NETWORK     -- DISCONNECTED (NETWORK/MQTT) ---> SAFE_OFF      (retry NETWORK)
 *   TLS         -- TLS_CONNECTED (MQTT) ----------> SYNC
 *   TLS         -- TLS_FAILED (MQTT) -------------> SAFE_OFF      (retry TLS)
 *   TLS         -- DISCONNECTED (NETWORK/MQTT) ---> SAFE_OFF      (retry NETWORK)
 *   SYNC        -- SYNC_COMPLETE (THINGSBOARD) ---> ONLINE
 *   SYNC        -- SYNC_FAILED (THINGSBOARD) -----> SAFE_OFF      (retry SYNC)
 *   SYNC        -- INVALID_STATE (THINGSBOARD) ---> SAFE_OFF      (retry SYNC)
 *   SYNC        -- DISCONNECTED (NETWORK/MQTT) ---> SAFE_OFF      (retry NETWORK)
 *   ONLINE      -- DISCONNECTED (NETWORK/MQTT) ---> SAFE_OFF      (retry NETWORK)
 *   ONLINE      -- INVALID_STATE (THINGSBOARD) ---> SAFE_OFF      (retry SYNC)
 *   ONLINE      -- OTA_BEGIN (OTA) ---------------> OTA
 *   SAFE_OFF    -- OTA_BEGIN (OTA) ---------------> OTA
 *   OTA         -- OTA_END (OTA) -----------------> BOOT
 *   OTA         -- OTA_FAILED (OTA) --------------> SAFE_OFF      (degraded)
 *   SAFE_OFF    -- RETRY_DUE (TIMER, internal) ---> NETWORK|TLS|SYNC (see Retry)
 *   any         -- RESET (EXTERNAL) --------------> BOOT
 *   any         -- FATAL (any owner) -------------> FATAL
 *
 * The DISCONNECTED rows above are ENCODED in the table with owner
 * APP_OWNER_MQTT, but the runtime app_owner_ok() deliberately widens every
 * DISCONNECT-class row to ALSO accept APP_OWNER_NETWORK — both the Wi-Fi
 * adapter (APP_OWNER_NETWORK) and the MQTT/TLS layer (APP_OWNER_MQTT) are
 * legitimate reporters of "transport down".  The "(NETWORK/MQTT)" owner
 * shown on those rows is that widened set: a row labeled MQTT therefore
 * accepts NETWORK for DISCONNECT-class events (no behavior difference —
 * this just makes the already-widened owner set explicit here and in the
 * encoded table).
 *
 * A successful transition back to #APP_STATE_SAFE_OFF/... is produced by
 * the events above; the module enforces them and drops everything else as
 * illegal (see "Stale and illegal callbacks").
 *
 * Retry and bounded backoff
 * -------------------------
 * Recoverable failures (PROVISIONING_FAILED, NETWORK_FAILED, TLS_FAILED,
 * SYNC_FAILED, INVALID_STATE, DISCONNECTED) park the machine in
 * #APP_STATE_SAFE_OFF and schedule ONE retry after a bounded,
 * exponentially-growing delay (2 ^ attempt, capped): default 2000 ms
 * first, 30000 ms cap, at most #APP_STATE_RETRY_MAX_ATTEMPTS_DEFAULT (5)
 * retry transitions per recovery episode.  The retry returns to the exact
 * stage that failed (network — including after a provisioning failure,
 * because the provisioning adapter is owned by the network stage — TLS or
 * sync) so a stage that already passed is never repeated
 * (no filesystem re-mount storms, no Wi-Fi re-scan storms).  Non-retryable
 * failures (FS_FAIL, CONFIG_FAIL, OTA_FAILED) park degraded WITHOUT an
 * automatic retry: recovery is an explicit RESET/provisioning/OTA action.
 * When the retry budget is exhausted the machine parks in SAFE_OFF and
 * stays silent — no retry storm is possible.  The budget resets to zero on
 * every successful arrival at #APP_STATE_ONLINE, so a healed link recovers
 * the full retry budget for the next outage.  The backoff schedule is
 * clock-driven by app_state_poll() using the configured clock, which makes
 * the whole policy deterministic in host tests.
 *
 * Stale and illegal callbacks (generation / session identity)
 * -----------------------------------------------------------
 * Every boot/reconnect/retry episode carries a session identity
 * (app_state_session()).  Cross-context callbacks MUST capture the session
 * at arm time (app_state_session()) and deliver it back with
 * app_state_deliver_session().  The module rejects:
 *
 *   - stale callbacks — event delivered with a session different from the
 *     current one (a late network callback from a previous reconnect
 *     episode, a dropped attempt response, ...).  Stale events are dropped
 *     and counted (app_state_stale_dropped()),
 *   - illegal callbacks — events that are not legal for the current state
 *     or arrive from a context that does not own the transition (wrong
 *     owner).  They are dropped and counted (app_state_invalid_dropped()).
 *
 * Safety-first exception: DISCONNECT-class events (DISCONNECTED,
 * *_FAILED, PROVISIONING_FAILED, INVALID_STATE, FS_FAIL, CONFIG_FAIL,
 * OTA_FAILED) ALWAYS force the lamp output inactive BEFORE the drop,
 * mirroring the network
 * adapter's stale-disconnect contract: a late disconnect must never leave
 * the output enabled.  #APP_EVENT_FATAL is always honored (any session,
 * any owner): a fatal failure report is never treated as stale.
 *
 * Watchdog policy (owner, feed, timeout, blocking constraints)
 * -----------------------------------------------------------
 * The application watchdog is owned by EXACTLY ONE task: the integrator's
 * supervisor loop, the only caller of app_state_poll().  All other
 * contexts (Wi-Fi worker, MQTT/TLS connect, ThingsBoard callbacks, timer,
 * OTA) deliver events only and never poll/feed.
 *
 *   - Feed points: every app_state_poll() call is a feed; every successful
 *     state transition also refreshes the deadline (forward progress).  A
 *     connected device that stops producing transitions still MUST keep
 *     polling within each watchdog window.
 *   - Timeout behavior: when a poll observes that the deadline has not been
 *     refreshed for more than the configured watchdog timeout, the module
 *     forces the output inactive, fires the configured
 *     on_watchdog_expired callback, disarms the watchdog, clears any
 *     pending retry and transitions to #APP_STATE_FATAL (owner
 *     #APP_OWNER_WATCHDOG).  The poll that detects the expiry returns
 *     #APP_STATE_ERR_WATCHDOG.
 *   - Recovery: a watchdog expiry (state #APP_STATE_FATAL) is recovered
 *     only by an explicit external reset (APP_EVENT_RESET) or OTA — the
 *     device does not spontaneously resume from a watchdog fault.  After
 *     RESET the machine returns to #APP_STATE_BOOT and a fresh
 *     app_state_start() re-runs the full boot sequence.
 *   - Blocking constraints (normative):
 *       1. The owning task must not block in a worker callback: poll() is
 *          only ever called from the owner's loop and from bounded wait
 *          loops the owner itself runs (feeding from a second context
 *          would let two tasks mask each other's stalls).
 *       2. Every blocking call the owner makes in its loop must be bounded
 *          BELOW the watchdog timeout, and any wait loop that can run
 *          longer than one poll interval must call app_state_poll() at
 *          each iteration (same task — allowed, and required).
 *       3. poll() must never be called from an ISR context.
 *       4. on_watchdog_expired and the optional observer run with the
 *          module lock held: they must be short, non-blocking and must
 *          NOT call back into this module's API.
 *
 * Fail-off contract
 * -----------------
 * The module calls lamp_control_force_inactive() on every entry into a
 * non-online state and on every rejected DISCONNECT-class event.  It never
 * applies a lamp state and never releases the fail-off barrier — those
 * transitions are owned by mqtt_cfg (verified-TLS release) and
 * tb_application (apply after complete valid sync), exactly as before.  If
 * a force-inactive ever fails, the module logs the safety event and keeps
 * the state transition (the state machine must not wedge), so the highest
 * available fail-off responsibility stays with the transition machinery.
 *
 * Host testability
 * ----------------
 * The module speaks only the portable OSAL clock/mutex/log surface and
 * lamp_control.  Host tests inject the clock (app_state_config_t::now_ms),
 * observe transitions (app_state_config_t::observer) and drive a
 * lamp-control double, giving deterministic fault-injection tests for
 * every failure transition, the reconnect loop, timeouts, watchdog
 * expiry/recovery and OTA entry/exit.
 */

#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Constants                                                              */
/* --------------------------------------------------------------------- */

/** @brief First retry delay after a recoverable failure (ms). */
#define APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS 2000u

/** @brief Backoff delay cap (ms). */
#define APP_STATE_RETRY_MAX_DELAY_DEFAULT_MS 30000u

/** @brief Default maximum retry transitions per recovery episode. */
#define APP_STATE_RETRY_MAX_ATTEMPTS_DEFAULT 5u

/** @brief Default backoff growth factor per failed attempt. */
#define APP_STATE_RETRY_BACKOFF_FACTOR_DEFAULT 2u

/** @brief Bounded retry delay limits (ms). */
#define APP_STATE_RETRY_DELAY_MIN_MS 100u
#define APP_STATE_RETRY_DELAY_MAX_MS 600000u

/** @brief Default application watchdog timeout (ms). */
#define APP_STATE_WATCHDOG_TIMEOUT_DEFAULT_MS 30000u

/** @brief Bounded watchdog timeout limits (ms). */
#define APP_STATE_WATCHDOG_TIMEOUT_MIN_MS 1000u
#define APP_STATE_WATCHDOG_TIMEOUT_MAX_MS 3600000u

/* --------------------------------------------------------------------- */
/* Status type                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Result type for the application state machine.
 *
 * #APP_STATE_OK (0) is the only success code.
 */
typedef enum app_state_status {
    APP_STATE_OK                   = 0,  /**< Accepted / transitioned. */
    APP_STATE_ERR_INVALID_ARGUMENT = -1, /**< NULL/invalid argument. */
    APP_STATE_ERR_NOT_INITIALIZED  = -2, /**< Operation before init / after deinit. */
    APP_STATE_ERR_ALREADY_INITIALIZED = -3, /**< init() while active. */
    APP_STATE_ERR_STATE            = -4, /**< Op not legal in the current state (start() from non-BOOT). */
    APP_STATE_ERR_STALE            = -5, /**< Stale session callback dropped (counted). */
    APP_STATE_ERR_ILLEGAL          = -6, /**< Illegal event/owner for the state, dropped (counted). */
    APP_STATE_ERR_WATCHDOG         = -7  /**< Poll detected a watchdog expiry; the machine is FATAL. */
} app_state_status_t;

/* --------------------------------------------------------------------- */
/* States                                                                 */
/* --------------------------------------------------------------------- */

/**
 * @brief Application states (see the header contract for the transition
 *        table, the owners and the fail-off invariant).
 */
typedef enum app_state {
    APP_STATE_BOOT = 0,          /**< Entry; not yet started; output off. */
    APP_STATE_FILESYSTEM,        /**< Waiting for filesystem bootstrap result. */
    APP_STATE_CONFIGURATION,     /**< Waiting for product configuration validation. */
    APP_STATE_NETWORK,           /**< Waiting for the Wi-Fi connection gate. */
    APP_STATE_PROVISIONING,      /**< Wi-Fi provisioning active (no saved
                                      station credential); output off. */
    APP_STATE_TLS,               /**< Waiting for verified MQTT/TLS. */
    APP_STATE_SYNC,              /**< Waiting for ThingsBoard desired-state sync. */
    APP_STATE_ONLINE,            /**< All gates passed; output may be active. */
    APP_STATE_SAFE_OFF,          /**< Degraded; output off; bounded retry only. */
    APP_STATE_FATAL,             /**< Unrecoverable; output off; reset required. */
    APP_STATE_OTA                /**< OTA in progress; output off. */
} app_state_t;

/* --------------------------------------------------------------------- */
/* Events                                                                 */
/* --------------------------------------------------------------------- */

/**
 * @brief Events delivered to the machine (see the transition table).
 */
typedef enum app_state_event {
    APP_EVENT_START = 0,         /**< internal: start() a boot episode. */
    APP_EVENT_FS_OK,             /**< Filesystem bootstrap succeeded. */
    APP_EVENT_FS_FAIL,           /**< Filesystem bootstrap failed (degraded). */
    APP_EVENT_CONFIG_OK,         /**< Product configuration validated. */
    APP_EVENT_CONFIG_FAIL,       /**< Product configuration rejected (degraded). */
    APP_EVENT_NETWORK_CONNECTED,     /**< Wi-Fi connection established. */
    APP_EVENT_NETWORK_FAILED,        /**< Wi-Fi connect failed or timed out (retryable). */
    APP_EVENT_PROVISIONING_STARTED,  /**< Wi-Fi provisioning starts (no saved station credential). */
    APP_EVENT_PROVISIONING_SUCCEEDED,/**< Provisioning credential saved; station may now connect. */
    APP_EVENT_PROVISIONING_FAILED,   /**< Provisioning failed/timed out (retryable). */
    APP_EVENT_TLS_CONNECTED,         /**< Verified MQTT/TLS connection established. */
    APP_EVENT_TLS_FAILED,        /**< Verified TLS connect failed (retryable). */
    APP_EVENT_SYNC_COMPLETE,     /**< Complete valid desired state synchronized. */
    APP_EVENT_SYNC_FAILED,       /**< Synchronization attempt failed (retryable). */
    APP_EVENT_DISCONNECTED,      /**< Transport/network lost (retryable). */
    APP_EVENT_INVALID_STATE,     /**< Invalid/partial desired-state data (retryable). */
    APP_EVENT_OTA_BEGIN,         /**< OTA session starts (online update). */
    APP_EVENT_OTA_END,           /**< OTA completed; reboot back to boot. */
    APP_EVENT_OTA_FAILED,        /**< OTA aborted/failed (degraded). */
    APP_EVENT_RETRY_DUE,         /**< internal: backoff retry fires (poll). */
    APP_EVENT_RESET,             /**< External reset/provisioning/recovery. */
    APP_EVENT_FATAL              /**< Unrecoverable failure report. */
} app_state_event_t;

/* --------------------------------------------------------------------- */
/* Transition owners                                                      */
/* --------------------------------------------------------------------- */

/**
 * @brief The context that owns a state transition.
 *
 * The integrator reports the owner on every delivery
 * (app_state_deliver() / app_state_deliver_session()); the machine rejects
 * events whose owner does not match the transition table entry.
 */
typedef enum app_transition_owner {
    APP_OWNER_BOOTSTRAP = 0,     /**< The boot sequence / app entry. */
    APP_OWNER_FILESYSTEM,        /**< Filesystem bootstrap context. */
    APP_OWNER_CONFIGURATION,     /**< Configuration loading context. */
    APP_OWNER_NETWORK,           /**< Wi-Fi adapter / provisioning adapter callbacks. */
    APP_OWNER_MQTT,              /**< MQTT/TLS connect/verified-TLS context. */
    APP_OWNER_THINGSBOARD,       /**< ThingsBoard attribute/sync callbacks. */
    APP_OWNER_TIMER,             /**< The machine's own retry timer. */
    APP_OWNER_OTA,               /**< OTA supervisor context. */
    APP_OWNER_WATCHDOG,          /**< The application watchdog. */
    APP_OWNER_EXTERNAL           /**< External/provisioning/reset context. */
} app_transition_owner_t;

/* --------------------------------------------------------------------- */
/* Failure classes                                                        */
/* --------------------------------------------------------------------- */

/**
 * @brief Failure handling class of a failure event.
 *
 * #APP_FAILURE_RETRYABLE schedules a bounded backoff retry;
 * #APP_FAILURE_DEGRADED parks in SAFE_OFF without an automatic retry;
 * #APP_FAILURE_FATAL moves straight to #APP_STATE_FATAL.
 */
typedef enum app_failure_class {
    APP_FAILURE_RETRYABLE = 0,
    APP_FAILURE_DEGRADED,
    APP_FAILURE_FATAL
} app_failure_class_t;

/* --------------------------------------------------------------------- */
/* Configuration                                                          */
/* --------------------------------------------------------------------- */

/**
 * @brief Clock provider (injectable for deterministic host tests).
 *
 * Returns the current time in milliseconds.  @c NULL selects the OSAL
 * monotonic clock (osal_task_get_time_ms()).
 */
typedef uint32_t (*app_state_now_fn_t)(void);

/**
 * @brief Watchdog expiry callback.
 *
 * Invoked (once) when app_state_poll() detects that the deadline was not
 * refreshed within the watchdog timeout.  Runs with the module lock held:
 * must be short, non-blocking and must NOT call back into this module.
 */
typedef void (*app_state_watchdog_fn_t)(void);

/**
 * @brief Optional transition observer (tests and diagnostics).
 *
 * Invoked on every successful transition with the from/to states, the
 * event, the transition owner and the session identity of the target
 * episode.  Runs with the module lock held: must be short, non-blocking
 * and must NOT call back into this module.
 */
typedef void (*app_state_observer_fn_t)(app_state_t from, app_state_t to,
                                        app_state_event_t event,
                                        app_transition_owner_t owner,
                                        uint32_t session);

/**
 * @brief Initialization configuration.
 *
 * Zero-valued optional fields select the documented defaults; every value
 * is clamped to the bounded ranges so no caller can program an unbounded
 * retry storm or an unusable watchdog.
 */
typedef struct app_state_config {
    app_state_now_fn_t now_ms;                /**< Clock; NULL = OSAL monotonic clock. */
    uint32_t retry_initial_delay_ms;          /**< First retry delay; 0 = default. */
    uint32_t retry_max_delay_ms;              /**< Backoff cap; 0 = default. */
    uint32_t retry_max_attempts;              /**< Retry transitions/episode; 0 = default. */
    uint32_t retry_backoff_factor;            /**< Backoff growth; 0 = default (2x). */
    uint32_t watchdog_timeout_ms;             /**< Watchdog window; 0 = default. */
    app_state_watchdog_fn_t on_watchdog_expired; /**< May be NULL. */
    app_state_observer_fn_t observer;         /**< May be NULL. */
} app_state_config_t;

/* --------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* --------------------------------------------------------------------- */

/**
 * @brief Initialize the application state machine.
 *
 * The machine starts in #APP_STATE_BOOT with a zero session and an armed
 * watchdog.  No lamp call is made from init(); the first
 * app_state_start() forces the output inactive (BOOT -> FILESYSTEM entry).
 *
 * @param[in] config Non-NULL configuration.
 *
 * @return #APP_STATE_OK on success,
 *         #APP_STATE_ERR_INVALID_ARGUMENT on a NULL config,
 *         #APP_STATE_ERR_ALREADY_INITIALIZED while already active.
 */
app_state_status_t app_state_init(const app_state_config_t *config);

/**
 * @brief Deinitialize the state machine.
 *
 * Idempotent.  Afterwards every operation returns
 * #APP_STATE_ERR_NOT_INITIALIZED until the next init().  A system reboot
 * (or a later task's OTA path) owns the actual reset; deinit leaves the
 * lamp output wherever it currently is (a preceding failure/fail-off
 * transition already performed the safety action).
 */
void app_state_deinit(void);

/**
 * @brief Begin a boot episode: #APP_STATE_BOOT -> #APP_STATE_FILESYSTEM.
 *
 * Forces the lamp output inactive, refreshes the watchdog deadline, starts
 * a fresh session (generation) and arms the watchdog.  Recovery from
 * #APP_STATE_FATAL, OTA completion and explicit RESET all land in
 * #APP_STATE_BOOT and re-enter the sequence through this call.
 *
 * @return #APP_STATE_OK on success,
 *         #APP_STATE_ERR_NOT_INITIALIZED before init,
 *         #APP_STATE_ERR_STATE when not in #APP_STATE_BOOT.
 */
app_state_status_t app_state_start(void);

/* --------------------------------------------------------------------- */
/* Event delivery                                                         */
/* --------------------------------------------------------------------- */

/**
 * @brief Deliver an event from the current (synchronous, same-context)
 *        caller, stamped with the CURRENT session identity.
 *
 * Use this for events produced on the same context that owns the machine
 * (filesystem result, configuration result, the supervisor's own connect
 * results).  Cross-context callbacks must use
 * app_state_deliver_session() with the session captured at arm time.
 *
 * @param[in] event The event.
 * @param[in] owner The transition owner reporting it.
 *
 * @return #APP_STATE_OK when the event was accepted (transition executed),
 *         #APP_STATE_ERR_STALE / #APP_STATE_ERR_ILLEGAL when it was
 *         dropped (see the header contract; DISCONNECT-class drops still
 *         perform the lamp fail-off),
 *         #APP_STATE_ERR_NOT_INITIALIZED before init.
 */
app_state_status_t app_state_deliver(app_state_event_t event,
                                     app_transition_owner_t owner);

/**
 * @brief Deliver a cross-context event stamped with the session the caller
 *        captured at arm time (app_state_session()).
 *
 * Stale events (session mismatch) are dropped and counted; illegal events
 * (wrong state, wrong owner) are dropped and counted.  See the header
 * contract for the safety-first DISCONNECT-class exception and the always-
 * honored FATAL event.
 *
 * @param[in] event   The event.
 * @param[in] owner   The transition owner reporting it.
 * @param[in] session The session identity the callback was armed under.
 *
 * @return #APP_STATE_OK when accepted,
 *         #APP_STATE_ERR_STALE / #APP_STATE_ERR_ILLEGAL when dropped,
 *         #APP_STATE_ERR_NOT_INITIALIZED before init.
 */
app_state_status_t app_state_deliver_session(app_state_event_t event,
                                             app_transition_owner_t owner,
                                             uint32_t session);

/* --------------------------------------------------------------------- */
/* Polling / watchdog / retry timer                                       */
/* --------------------------------------------------------------------- */

/**
 * @brief Drive the watchdog and the bounded retry schedule.
 *
 * THE single watchdog feed point, owned by exactly one task (the
 * integrator's supervisor loop — see the watchdog policy in the header
 * contract for the blocking constraints).  Performs, in order:
 *
 *   1. watchdog check — if the deadline was not refreshed within the
 *      watchdog timeout, forces the output inactive, fires the configured
 *      expiry callback, disarms the watchdog, clears any pending retry,
 *      transitions to #APP_STATE_FATAL and returns
 *      #APP_STATE_ERR_WATCHDOG;
 *   2. deadline refresh (feed);
 *   3. retry schedule — if a retry is due, consumes one retry attempt,
 *      starts a fresh session and returns to the failed stage
 *      (#APP_STATE_NETWORK — also after a provisioning failure, since the
 *      provisioning adapter is owned by the network stage — / #APP_STATE_TLS
 *      / #APP_STATE_SYNC).
 *
 * @return #APP_STATE_OK on a normal poll (including a fired retry),
 *         #APP_STATE_ERR_WATCHDOG when this poll detected a watchdog
 *         expiry,
 *         #APP_STATE_ERR_NOT_INITIALIZED before init.
 */
app_state_status_t app_state_poll(void);

/**
 * @brief app_state_poll() with an explicit clock value (host tests).
 *
 * @param[in] now_ms The current time in milliseconds.
 * @return See app_state_poll().
 */
app_state_status_t app_state_poll_at(uint32_t now_ms);

/* --------------------------------------------------------------------- */
/* State queries                                                          */
/* --------------------------------------------------------------------- */

/**
 * @brief Current application state.
 *
 * @return The current state, or #APP_STATE_BOOT before init/deinit.
 */
app_state_t app_state_current(void);

/**
 * @brief Current session identity (generation).
 *
 * Returned so cross-context callbacks can capture the generation at arm
 * time and hand it back with app_state_deliver_session().  The session is
 * bumped on every start()/RESET/retry/OTA boundary.
 *
 * @return The current session, 0 before init/deinit.
 */
uint32_t app_state_session(void);

/**
 * @brief Owner of the last successful transition.
 *
 * @return The owner, or #APP_OWNER_EXTERNAL before any transition.
 */
app_transition_owner_t app_state_last_owner(void);

/**
 * @brief Is the lamp output permitted in the current state?
 *
 * @return true ONLY in #APP_STATE_ONLINE; false in every other (and in
 *         every failure) state.  The machine itself additionally forces
 *         the output inactive on every non-online entry, so "permitted"
 *         and "forced off" stay consistent.
 */
bool app_state_is_online(void);

/* --------------------------------------------------------------------- */
/* Retry diagnostics (tests and observability)                            */
/* --------------------------------------------------------------------- */

/** @brief Is a backoff retry currently scheduled? */
bool app_state_retry_pending(void);

/** @brief Next scheduled retry delay (ms); 0 when none scheduled. */
uint32_t app_state_retry_delay_ms(void);

/**
 * @brief Retry attempts already consumed in the current recovery episode.
 */
uint32_t app_state_retry_attempts_used(void);

/**
 * @brief Has the retry budget been exhausted (machine parked in SAFE_OFF)?
 */
bool app_state_retry_exhausted(void);

/**
 * @brief Number of stale (session-mismatched) callbacks dropped so far.
 */
uint32_t app_state_stale_dropped(void);

/**
 * @brief Number of illegal (wrong state/owner) callbacks dropped so far.
 */
uint32_t app_state_invalid_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATE_H */