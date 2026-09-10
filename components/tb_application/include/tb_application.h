/**
 * @file tb_application.h
 * @brief ThingsBoard application logic: desired-state synchronization
 *        (TASK-112), server-side RPC control (TASK-113) and telemetry /
 *        health reporting (TASK-114)
 *
 * Normative public API of the product-owned ThingsBoard application
 * component.  Shared attributes `power` and `brightness` are authoritative
 * for the lamp's desired state; this module turns a fresh MQTT/TLS
 * connection into a complete, validated, session-consistent desired state
 * applied to the lamp.  On top of that synchronization contract the module
 * also owns the documented server-side RPC control surface (setPower /
 * setBrightness / setState / getState), which is the transient
 * service/test control channel described below.
 *
 * Behavioral reference: the platform ThingsBoard RGB lamp example
 * (platform/hq_platform/examples/common/thingboard_rgb_lamp.c), the
 * platform ThingsBoard attributes primitives (tb_attributes.h) and the
 * platform server-side RPC primitives (tb_rpc.h).  This component reuses
 * those transport primitives unchanged and owns the product policy around
 * them.
 *
 * Synchronization contract (per successful connection)
 * ----------------------------------------------------
 *   1. subscribe to shared-attribute updates,
 *   2. request `power` and `brightness` as ONE shared-attribute request,
 *   3. keep the lamp output off while waiting,
 *   4. validate a complete response (required fields, JSON types, value
 *      ranges, bounded payload),
 *   5. apply the state only after a complete valid response (or a complete
 *      valid update) arrives for the current synchronization session,
 *   6. on sync timeout remain off and retry after bounded backoff,
 *   7. restart synchronization after every reconnect; disconnect always
 *      forces the lamp output inactive (fail-off).
 *
 * The module deliberately mirrors the other product-owned components'
 * fail-off philosophy (mqtt_cfg / network_manager / device_identity):
 * every rejection path — invalid types/ranges, partial data, timeout,
 * transport loss, duplicate data and stale callbacks — leaves the lamp
 * output off.  No output is ever enabled before a complete valid state has
 * been synchronized for the current connection session.
 *
 * Session identity
 * ----------------
 * Every subscribe/request cycle carries a per-session token.  Responses and
 * updates whose token does not match the current synchronization session
 * are stale and dropped; each synchronization attempt has a unique attempt
 * number so a duplicated or late response for an already-consumed attempt
 * is rejected instead of being applied twice.  Duplicate, stale and late
 * callbacks therefore never change the output state.
 *
 * Threading and lifecycle
 * -----------------------
 *   - tb_application_init() must be called once after the ThingsBoard
 *     client exists and before any connect; deinit() is the inverse.
 *   - The module locks internally; every public operation is thread-safe.
 *   - The connect/disconnect glue (supplied by the integrator) MUST invoke
 *     tb_application_on_connected() / tb_application_on_disconnected() from
 *     the ThingsBoard client's connection-state callbacks, exactly:
 *
 *         tb_client_config_t tc = { .on_connect    = glue_connected,
 *                                   .on_disconnect = glue_disconnected };
 *         static void glue_connected(tb_client_t *c, void *ud) {
 *             (void)ud; tb_application_on_connected(c);
 *         }
 *         static void glue_disconnected(tb_client_t *c,
 *                                      tb_client_disconnect_reason_t r,
 *                                      void *ud) {
 *             (void)r; (void)ud; tb_application_on_disconnected(c);
 *         }
 *
 *     The ThingsBoard transport auto-reconnects; every successful connect
 *     event restarts the synchronization (the subscribe/request cycle) so
 *     ThingsBoard remains authoritative after reconnect.
 *   - tb_application_poll() drives the sync window deadline and the bounded
 *     retry/backoff state and must be called periodically (10..100 ms) by
 *     the application loop while the client is connected.
 *   - Callbacks (attribute responses / shared updates) run on the
 *     transport thread and take the module lock themselves; the transport
 *     calls issued from on_connected()/poll() are performed with the
 *     module lock released so a synchronous transport callback cannot
 *     deadlock on the module lock.
 *
 * Desired-state authority
 * -----------------------
 *   - Until a complete valid state is applied for the current session,
 *     tb_application_is_synchronized() returns false and the lamp output
 *     stays off.
 *   - After synchronization, complete valid shared-attribute updates are
 *     applied immediately (authoritative changes); identical retransmitted
 *     updates are duplicates and ignored; invalid or partial updates force
 *     the output off again and re-enter synchronization with bounded
 *     backoff.
 *
 * Server-side RPC control (TASK-113)
 * ----------------------------------
 * Every successful connection also (re-)arms the server-side RPC control
 * surface through the platform tb_rpc primitives.  RPC is the *transient*
 * service/test control channel: it drives the lamp hardware directly and
 * never writes shared attributes (dashboards keep writing shared
 * attributes; a server rule chain may synchronize RPC results into shared
 * attributes explicitly).  Exactly four methods are supported:
 *
 *   - `setPower`      `{"power": <boolean>}`                         — required,
 *   - `setBrightness` `{"brightness": <integer 0..100>}`             — required,
 *   - `setState`      `{"power": <boolean>, "brightness": <0..100>}` — both required,
 *   - `getState`      `{}`                                           — returns state.
 *
 * Validation is strict and happens before any hardware action:
 *   - method name and params payload are length-bounded before parsing
 *     (#TB_APPLICATION_RPC_METHOD_MAX_LEN /
 *     #TB_APPLICATION_RPC_PARAMS_MAX_LEN),
 *   - params must be a JSON object; required fields must be present with
 *     exactly the documented JSON types (power: boolean; brightness:
 *     integer, never fractional),
 *   - brightness must be in the closed interval 0..100 (never clamped),
 *   - any other method name is rejected as unknown.
 *
 * Ordering and reporting:
 *   - the validated state is applied to the hardware BEFORE the success
 *     response is published — a response carrying `"success":true` is only
 *     ever published after lamp_control_apply_state() returned #LAMP_OK,
 *   - `power`/`brightness` telemetry is published ONLY after a successful
 *     hardware change (never for invalid requests, failures or getState),
 *   - responses are structured JSON: success responses carry the resulting
 *     `desired` and `applied` states; errors carry `"success":false` with
 *     `"error"` set to one of `unknown method` / `invalid payload` /
 *     `hardware failure` (plus a `reason` and/or `method` detail).
 *
 * An invalid RPC never modifies the applied state.  A valid RPC applies
 * through lamp_control_apply_state() like a synchronized state, so the
 * module's last-applied desired state (reported by getState and used for
 * duplicate detection) stays consistent across both control channels; the
 * next shared-attribute update remains authoritative afterwards.
 *
 * Telemetry and health reporting (TASK-114)
 * ------------------------------------------
 * The module publishes the documented health telemetry record
 * (production plan section 8.3) on three triggers:
 *
 *   1. every successful connection (immediately from on_connected()),
 *   2. every successful state change (synchronization apply, shared-attribute
 *      update apply, or a valid server-RPC set that changed the hardware),
 *   3. periodically while connected, rate-limited to at most one publish per
 *      #TB_APPLICATION_TELEMETRY_PERIOD_DEFAULT_MS-like interval
 *      (`telemetry_period_ms`; CONFIG_KLC_TELEMETRY_PERIOD_MS).
 *
 * Every record carries exactly the documented fields:
 *
 *     {
 *       "power": <bool>,              // applied power
 *       "brightness": <0..100>,       // applied brightness percent
 *       "pwm_duty": <0..10000>,       // applied PWM duty (LAMP_DUTY_SCALE units)
 *       "connection_state": "online",
 *       "fw_version": "<bounded>",    // config::fw_version (safe charset)
 *       "hardware": "<bounded>",      // config::hardware (safe charset)
 *       "uptime_ms": <uint32>         // OSAL monotonic uptime at publish
 *     }
 *
 * `pwm_duty` is derived from the state actually applied to the PWM output
 * (lamp_control_get_applied_state()), never from the requested brightness
 * alone.  All records are serialized with a bounded stack buffer
 * (#TB_APPLICATION_TELEMETRY_MAX_BYTES); a serialization overflow suppresses
 * the publish instead of emitting truncated JSON.
 *
 * Secrecy and disconnection:
 *   - telemetry NEVER carries tokens, passwords, provisioning secrets,
 *     certificates, keys, or file paths.  The module only publishes the
 *     seven fields above; the two config strings are truncated to their
 *     documented bounds and filtered to a conservative `[A-Za-z0-9._+-]`
 *     charset before being copied into module storage, so a misconfigured
 *     value containing `/`, quotes, whitespace or control characters cannot
 *     leak a path or break the JSON framing,
 *   - while the transport is disconnected every publication is suppressed
 *     at the source: no telemetry is queued, buffered or retried, so a
 *     disconnect can never build an unbounded publish queue,
 *   - periodic telemetry is additionally rate-limited by
 *     `telemetry_period_ms`: repeated poll() calls (and change/connect
 *     events) cannot cause more than one publish per period.
 *
 * Host testability
 * ----------------
 * The module speaks only the portable OSAL clock/mutex/log surface, cJSON,
 * the platform ThingsBoard attributes/client primitives and lamp_control.
 * Host tests compile it against the REAL platform tb_attributes/tb_client
 * sources with the platform's mqtt_app test double (mqtt_app_mock) and a
 * lamp-control double, exactly like the platform's own tb_tests; it never
 * references ESP-IDF VFS, sockets or TLS symbols directly.
 */

#ifndef TB_APPLICATION_H
#define TB_APPLICATION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of the platform opaque ThingsBoard client handle
 * (see platform/hq_platform/src/thingsboard/include/tb_client.h).  No
 * platform header is exposed through this product-owned API. */
typedef struct tb_client tb_client_t;

/* --------------------------------------------------------------------- */
/* Configuration constants                                                */
/* --------------------------------------------------------------------- */

/** @brief Default synchronization window per attempt (ms). */
#define TB_APPLICATION_SYNC_TIMEOUT_DEFAULT_MS         10000u

/** @brief Bounded synchronization window limits (ms). */
#define TB_APPLICATION_SYNC_TIMEOUT_MIN_MS            100u
#define TB_APPLICATION_SYNC_TIMEOUT_MAX_MS            60000u

/** @brief Default first retry delay after a failed attempt (ms). */
#define TB_APPLICATION_RETRY_INITIAL_DELAY_DEFAULT_MS 2000u

/** @brief Default maximum retry/backoff delay (ms). */
#define TB_APPLICATION_RETRY_MAX_DELAY_DEFAULT_MS     30000u

/** @brief Bounded retry delay limits (ms). */
#define TB_APPLICATION_RETRY_DELAY_MIN_MS            100u
#define TB_APPLICATION_RETRY_DELAY_MAX_MS            600000u

/** @brief Default maximum consecutive failed attempts per session. */
#define TB_APPLICATION_MAX_RETRIES_DEFAULT 5u

/**
 * @brief Maximum accepted desired-state payload size (bytes).
 *
 * Shared-attribute updates and attribute responses larger than this bound
 * are rejected as invalid before parsing (bounded payloads).
 */
#define TB_APPLICATION_MAX_PAYLOAD_BYTES 512u

/**
 * @brief Maximum accepted server-RPC method name length (bytes).
 *
 * Longer method names are rejected as invalid payload before any method
 * comparison or parsing (bounded methods).
 */
#define TB_APPLICATION_RPC_METHOD_MAX_LEN 32u

/**
 * @brief Maximum accepted server-RPC params payload size (bytes).
 *
 * The RPC params object is re-serialized by the transport; payloads longer
 * than this bound are rejected as invalid payload before parsing (bounded
 * payloads).
 */
#define TB_APPLICATION_RPC_PARAMS_MAX_LEN TB_APPLICATION_MAX_PAYLOAD_BYTES

/** @brief Default periodic health telemetry interval (ms); matches the
 *         product Kconfig default (CONFIG_KLC_TELEMETRY_PERIOD_MS). */
#define TB_APPLICATION_TELEMETRY_PERIOD_DEFAULT_MS 30000u

/** @brief Bounded periodic telemetry interval limits (ms). */
#define TB_APPLICATION_TELEMETRY_PERIOD_MIN_MS     1000u
#define TB_APPLICATION_TELEMETRY_PERIOD_MAX_MS     3600000u

/**
 * @brief Maximum firmware/build version string length (excluding the NUL
 *        terminator) accepted in configuration and reported in telemetry.
 */
#define TB_APPLICATION_FW_VERSION_MAX_LEN 32u

/**
 * @brief Maximum hardware-target string length (excluding the NUL
 *        terminator) accepted in configuration and reported in telemetry.
 */
#define TB_APPLICATION_HARDWARE_MAX_LEN 32u

/**
 * @brief Bounded telemetry serialization buffer size (bytes).
 *
 * Every telemetry publish is serialized into a fixed stack buffer of this
 * size (never an unbounded heap print) and truncated-safe: an overflow is
 * reported and the publish is suppressed instead of emitting malformed JSON.
 */
#define TB_APPLICATION_TELEMETRY_MAX_BYTES 512u

/** @brief Documented `connection_state` value of every published telemetry
 *         record (publication is suppressed while disconnected). */
#define TB_APPLICATION_CONNECTION_STATE_ONLINE "online"

/* --------------------------------------------------------------------- */
/* Status type                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Result type for the desired-state synchronization service.
 *
 * #TB_APPLICATION_OK (0) is the only success code.
 */
typedef enum tb_application_status {
    TB_APPLICATION_OK                   = 0,  /**< Success. */
    TB_APPLICATION_ERR_INVALID_ARGUMENT = -1, /**< NULL/invalid argument. */
    TB_APPLICATION_ERR_NOT_INITIALIZED  = -2, /**< Operation before init. */
    TB_APPLICATION_ERR_ALREADY_INITIALIZED = -3, /**< init() while active. */
    TB_APPLICATION_ERR_CLIENT_MISMATCH  = -4  /**< Wrong client handle. */
} tb_application_status_t;

/* --------------------------------------------------------------------- */
/* Configuration                                                          */
/* --------------------------------------------------------------------- */

/**
 * @brief Clock provider (injectable for deterministic host tests).
 *
 * Returns the current time in milliseconds.  @c NULL selects the OSAL
 * monotonic clock (osal_task_get_time_ms()).
 */
typedef uint32_t (*tb_application_now_fn_t)(void);

/**
 * @brief Initialization configuration.
 *
 * Zero-valued optional timing fields select the documented defaults; every
 * value is additionally clamped to the bounded range so no caller can
 * program an unbounded wait or an unbounded retry storm.
 */
typedef struct tb_application_config {
    tb_client_t *client;                          /**< ThingsBoard client (required). */
    uint32_t     sync_timeout_ms;                 /**< Sync window per attempt; 0 = default. */
    uint32_t     retry_initial_delay_ms;          /**< First retry delay; 0 = default. */
    uint32_t     retry_max_delay_ms;              /**< Backoff cap; 0 = default. */
    uint32_t     max_retries;                     /**< Consecutive failed attempts per session; 0 = default. */
    tb_application_now_fn_t now_ms;               /**< Clock provider; NULL = OSAL monotonic clock. */

    /**
     * @brief Periodic health telemetry interval (ms); 0 = default
     *        (#TB_APPLICATION_TELEMETRY_PERIOD_DEFAULT_MS, matching
     *        CONFIG_KLC_TELEMETRY_PERIOD_MS).  Bounded to
     *        #TB_APPLICATION_TELEMETRY_PERIOD_MIN_MS ..
     *        #TB_APPLICATION_TELEMETRY_PERIOD_MAX_MS.
     */
    uint32_t     telemetry_period_ms;
    /**
     * @brief Firmware/build version reported in telemetry as `fw_version`
     *        (may be NULL/empty).  Copied bounded to
     *        #TB_APPLICATION_FW_VERSION_MAX_LEN characters and filtered to
     *        the safe telemetry charset, so it can never smuggle JSON,
     *        paths or secrets into a publish.
     */
    const char  *fw_version;
    /**
     * @brief Hardware target reported in telemetry as `hardware` (may be
     *        NULL/empty).  Copied bounded to
     *        #TB_APPLICATION_HARDWARE_MAX_LEN characters and filtered to the
     *        safe telemetry charset, exactly like @p fw_version.
     */
    const char  *hardware;
} tb_application_config_t;

/* --------------------------------------------------------------------- */
/* Desired state                                                          */
/* --------------------------------------------------------------------- */

/**
 * @brief Complete, validated desired state applied to the lamp.
 *
 * Mirrors lamp_state_t's product semantics: brightness is the closed
 * interval 0..100 and `power == false` (or brightness 0) is electrically
 * off.  This is the authoritative ThingsBoard state; the raw JSON it came
 * from is never retained.
 */
typedef struct tb_application_desired_state {
    bool    power;              /**< Authoritative power (true = on). */
    uint8_t brightness_percent; /**< Authoritative brightness, 0..100. */
} tb_application_desired_state_t;

/* --------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* --------------------------------------------------------------------- */

/**
 * @brief Initialize the desired-state synchronizer.
 *
 * Called once, after the ThingsBoard client exists and before any connect
 * attempt.  The module starts in the inactive (output off) state; the
 * first tb_application_on_connected() begins the synchronization cycle.
 *
 * @param[in] config Non-NULL configuration; @c config->client must be
 *                   non-NULL.
 *
 * @return #TB_APPLICATION_OK on success,
 *         #TB_APPLICATION_ERR_INVALID_ARGUMENT on a NULL config/client,
 *         #TB_APPLICATION_ERR_ALREADY_INITIALIZED while already active.
 */
tb_application_status_t tb_application_init(const tb_application_config_t *config);

/**
 * @brief Deinitialize the synchronizer.
 *
 * Idempotent.  Afterwards the module ignores every operation until the
 * next successful init().  The lamp output is left in its current state (a
 * disconnect fail-off already happened before deinit would be reached).
 */
void tb_application_deinit(void);

/* --------------------------------------------------------------------- */
/* Connection glue (MUST be wired to the ThingsBoard client callbacks)    */
/* --------------------------------------------------------------------- */

/**
 * @brief Start synchronization after a successful MQTT/TLS connection.
 *
 * Restarts the session (invalidating every in-flight token of the previous
 * session), subscribes to shared-attribute updates and requests `power`
 * and `brightness` as one operation.  The output stays off until a
 * complete valid state arrives for the new session.
 *
 * Must be called from the ThingsBoard client's on_connect callback context.
 * Safe to call when already synchronized: it starts a fresh authoritative
 * cycle and re-validates the state against the freshly connected broker,
 * keeping ThingsBoard authoritative after reconnect.
 *
 * @param[in] client The ThingsBoard client that connected.
 */
void tb_application_on_connected(tb_client_t *client);

/**
 * @brief Handle transport loss (MQTT/TLS disconnect or connect failure).
 *
 * Forces the lamp output inactive (fail-off) and invalidates the current
 * synchronization session so every stale/late callback is dropped.  The
 * next connect (auto-reconnect or app-driven) restarts synchronization.
 * Disconnect ALWAYS performs the lamp fail-off, regardless of synchroniza-
 * tion progress.
 *
 * Must be called from the ThingsBoard client's on_disconnect callback
 * context (or, for connect failures never followed by connect, from the
 * application loop after the transport is confirmed down).
 *
 * @param[in] client The ThingsBoard client that disconnected.
 */
void tb_application_on_disconnected(tb_client_t *client);

/**
 * @brief Drive the synchronization window deadline and bounded retry/backoff.
 *
 * Called periodically by the application loop (10..100 ms).  While a sync
 * attempt is in flight it enforces the bounded sync window (timeout leaves
 * the output off); after a failed attempt it enforces the bounded,
 * exponentially-growing retry delay (capped and attempt-limited); it also
 * restarts attempts that become due after a backoff period.  No-op when
 * inactive or disconnected.
 *
 * @param[in] client The ThingsBoard client; must match init()'s handle.
 */
void tb_application_poll(tb_client_t *client);

/* --------------------------------------------------------------------- */
/* State query                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Is a complete valid desired state applied for the current session?
 *
 * @param[in] client The ThingsBoard client (may be NULL).
 * @return true only when the module is initialized, the client matches,
 *         the transport is connected AND a complete valid state has been
 *         applied for the current synchronization session.  Anything else
 *         (connecting, syncing, backing off, disconnected, invalid data
 *         received) returns false.
 */
bool tb_application_is_synchronized(tb_client_t *client);

/**
 * @brief Get the last applied complete valid desired state.
 *
 * @param[in]  client    The ThingsBoard client (may be NULL).
 * @param[out] has_state Receives true once at least one complete valid
 *                       state was applied since init (may be NULL).
 * @param[out] out       Receives the last applied state on success
 *                       (may be NULL for a pure has-state query).
 *
 * @return #TB_APPLICATION_OK on success,
 *         #TB_APPLICATION_ERR_NOT_INITIALIZED before init (or after
 *         deinit),
 *         #TB_APPLICATION_ERR_CLIENT_MISMATCH when @p client is non-NULL
 *         and differs from the configured handle,
 *         #TB_APPLICATION_ERR_INVALID_ARGUMENT when both @p has_state and
 *         @p out are NULL.
 */
tb_application_status_t tb_application_get_desired_state(
    tb_client_t *client, bool *has_state,
    tb_application_desired_state_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TB_APPLICATION_H */