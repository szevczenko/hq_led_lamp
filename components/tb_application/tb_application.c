/**
 * @file tb_application.c
 * @brief ThingsBoard application logic: desired-state synchronization
 *        (TASK-112), server-side RPC control (TASK-113) and telemetry /
 *        health reporting (TASK-114)
 *
 * See tb_application.h for the normative contract.  Implementation notes:
 *
 *   - the module owns a small session state machine (INACTIVE / SYNCING /
 *     BACKOFF / SYNCED).  Every connection starts a fresh session; every
 *     disconnect invalidates the session and forces the lamp output
 *     inactive (fail-off), so stale or late callbacks can never re-enable
 *     the output,
 *   - the transport surface is the existing ThingsBoard primitives
 *     (tb_attributes_subscribe() + tb_attributes_request_shared()) — the
 *     exact reuse the RGB lamp example demonstrates.  The module never
 *     talks to MQTT/TLS itself; the platform request/response machinery
 *     carries the transport between broker and this module,
 *   - payloads are bounded (TB_APPLICATION_MAX_PAYLOAD_BYTES) and parsed
 *     with cJSON; a desired state is complete and valid only when `power`
 *     is a JSON boolean and `brightness` is an integer JSON number in
 *     0..100.  Anything else (malformed JSON, missing/extra-typed fields,
 *     out-of-range or fractional brightness, oversized payload) is invalid
 *     and fails the attempt: the output is forced inactive and a bounded
 *     backoff retry is scheduled,
 *   - session identity: every subscribe/request cycle carries a session
 *     token; responses and updates are dropped when the token does not
 *     match the current session, and each attempt has a unique number so a
 *     duplicated or late response for an already-consumed attempt is
 *     rejected instead of being applied twice,
 *   - server-side RPC control (TASK-113): every successful connection also
 *     (re-)arms tb_rpc_subscribe_server().  The handler validates the four
 *     documented methods with bounded method/payload lengths and strict
 *     JSON types/ranges, applies the hardware state BEFORE any success
 *     response, publishes telemetry only after a successful change and
 *     returns structured success/error JSON.  RPC never writes shared
 *     attributes (transient control channel),
 *   - telemetry and health reporting (TASK-114): the documented seven-field
 *     record is published on connect, on every successful state change
 *     (synchronization apply, shared update apply, valid RPC set) and
 *     periodically while connected.  Records are serialized into a bounded
 *     stack buffer (TB_APPLICATION_TELEMETRY_MAX_BYTES) with a conservative
 *     safe charset for the two config strings and the PWM duty read from
 *     the applied lamp state.  Every publish is suppressed at the source
 *     while disconnected (no queue, no retry) and the periodic publish is
 *     rate-limited to telemetry_period_ms by a last-publish timestamp,
 *   - lock discipline: all module state is guarded by one OSAL mutex.
 *     Transport calls (subscribe/request) are always issued with the lock
 *     released, because the platform can invoke the response callback
 *     synchronously on an error path; the callbacks take the lock
 *     themselves, so no re-entrant deadlock can form.  lamp_control never
 *     calls back into this module, so holding the module lock while calling
 *     lamp_control is safe.  The RPC handler (transport thread) also holds
 *     the module lock while applying and responding; tb_rpc_respond only
 *     publishes and never calls back into this module.
 */

#include "tb_application.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "lamp_control.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "tb_attributes.h"
#include "tb_client.h"
#include "tb_rpc.h"
#include "tb_telemetry.h"

/* --------------------------------------------------------------------- */
/* Internal constants                                                     */
/* --------------------------------------------------------------------- */

/** @brief Shared-attribute keys requested as one synchronization operation. */
#define TB_APPLICATION_KEY_POWER "power"
#define TB_APPLICATION_KEY_BRIGHTNESS "brightness"

/** @brief Documented server-side RPC method names (TASK-113). */
#define TB_APPLICATION_RPC_METHOD_SET_POWER "setPower"
#define TB_APPLICATION_RPC_METHOD_SET_BRIGHTNESS "setBrightness"
#define TB_APPLICATION_RPC_METHOD_SET_STATE "setState"
#define TB_APPLICATION_RPC_METHOD_GET_STATE "getState"

/* --------------------------------------------------------------------- */
/* Internal module state                                                  */
/* --------------------------------------------------------------------- */

/** @brief Synchronization session state (see header contract). */
typedef enum tb_app_state {
    TB_APP_STATE_INACTIVE = 0, /**< Disconnected or retries exhausted; output off. */
    TB_APP_STATE_SYNCING,      /**< Connected; one subscribe/request attempt in flight. */
    TB_APP_STATE_BACKOFF,      /**< Connected; last attempt failed; bounded delay before retry. */
    TB_APP_STATE_SYNCED        /**< Connected; a complete valid state is applied. */
} tb_app_state_t;

/**
 * @brief Token handed to the in-flight attribute request.
 *
 * Identifies the exact session+attempt a callback belongs to.  The module
 * only ever has one outstanding request, so a single module-owned token is
 * sufficient and stays valid for the whole attempt.
 */
typedef struct tb_app_attempt_token {
    uint32_t session; /**< Session the request was issued in. */
    uint32_t attempt; /**< Attempt number within that session. */
} tb_app_attempt_token_t;

typedef struct tb_app_module {
    bool               initialized;
    bool               connected;
    tb_client_t       *client;
    uint32_t           sync_timeout_ms;
    uint32_t           retry_initial_ms;
    uint32_t           retry_max_ms;
    uint32_t           max_retries;
    tb_application_now_fn_t now_fn;
    osal_mutex_id_t    lock;

    /* Synchronization state machine (guarded by lock). */
    tb_app_state_t     state;
    uint32_t           session;        /**< Monotonic per-connection session id. */
    uint32_t           attempt;        /**< Current attempt number in the session. */
    uint32_t           retries;        /**< Consecutive failed attempts in the session. */
    uint32_t           attempt_deadline_ms; /**< Sync window end of the in-flight attempt. */
    uint32_t           backoff_until_ms;    /**< Earliest time the next retry may start. */
    uint32_t           backoff_delay_ms;    /**< Current backoff delay (doubles per failure). */
    bool               attempt_resolved;    /**< In-flight request already completed (duplicate guard). */
    tb_app_attempt_token_t request_token;   /**< Token of the in-flight request. */

    /* Last applied complete valid state (for telemetry/reporting). */
    lamp_state_t       applied;
    bool               has_applied;

    /* Telemetry / health reporting (TASK-114). */
    uint32_t           telemetry_period_ms; /**< Bounded periodic interval (0 = default). */
    char               fw_version[TB_APPLICATION_FW_VERSION_MAX_LEN + 1u]; /**< Safe-charset copy. */
    char               hardware[TB_APPLICATION_HARDWARE_MAX_LEN + 1u];     /**< Safe-charset copy. */
    uint32_t           last_telemetry_ms;  /**< Last telemetry publish time (rate limit). */
} tb_app_module_t;

static tb_app_module_t s_tb;

/* Forward declarations (defined below; the transport function references
 * them before their definitions). */
static void tb_app_on_shared_update(const char *json_payload,
                                    void *user_data);
static void tb_app_on_attr_response(tb_request_result_t result,
                                    const char *json_response,
                                    void *user_data);

/* --------------------------------------------------------------------- */
/* Helpers (all internal helpers expect the lock held unless noted)        */
/* --------------------------------------------------------------------- */

static uint32_t tb_app_now_ms(void)
{
    if (s_tb.now_fn != NULL)
    {
        return s_tb.now_fn();
    }
    return osal_task_get_time_ms();
}

static bool tb_app_is_connected(void)
{
    return s_tb.connected && (s_tb.client != NULL) &&
           tb_client_is_connected(s_tb.client);
}

static uint32_t tb_app_clamp(uint32_t value, uint32_t min_value,
                             uint32_t max_value)
{
    if (value < min_value)
    {
        return min_value;
    }
    if (value > max_value)
    {
        return max_value;
    }
    return value;
}

/**
 * @brief Is @p c a telemetry-safe character?
 *
 * The conservative `[A-Za-z0-9._+-]` charset is the only text ever allowed
 * into a telemetry publish: it structurally excludes JSON framing characters
 * (`"`, `\`), whitespace/control bytes and path separators (`/`, `:`), so a
 * misconfigured config string can never smuggle a secret path, a quote or a
 * whole token into the JSON payload.
 */
static bool tb_app_telemetry_safe_char(char c)
{
    if (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
        ((c >= '0') && (c <= '9')) || (c == '.') || (c == '-') ||
        (c == '_') || (c == '+'))
    {
        return true;
    }
    return false;
}

/**
 * @brief Copy a configuration string into module storage, bounded and
 *        filtered to the telemetry-safe charset.
 *
 * The copy stops at the first unsafe byte and never exceeds
 * @p dst_size - 1 characters, so the destination is always NUL-terminated
 * and the stored value can be emitted into JSON verbatim.
 *
 * @param[in]  src      Source string (may be NULL = empty).
 * @param[out] dst      Destination buffer (must be non-NULL).
 * @param[in]  dst_size Destination size in bytes (must be > 0).
 */
static void tb_app_copy_telemetry_string(const char *src, char *dst,
                                         size_t dst_size)
{
    size_t i = 0u;

    if ((dst == NULL) || (dst_size == 0u))
    {
        return;
    }
    if (src != NULL)
    {
        while (((i + 1u) < dst_size) && (src[i] != '\0'))
        {
            if (!tb_app_telemetry_safe_char(src[i]))
            {
                break; /* Unsafe byte: drop the rest of the value. */
            }
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/**
 * @brief Publish the documented health telemetry record (TASK-114).
 *
 * Called with the module lock held.  Serializes the seven documented fields
 * into a bounded stack buffer and hands the payload to the platform
 * telemetry transport:
 *
 *   - `power` / `brightness` / `pwm_duty` are read from the state actually
 *     applied to the PWM output (lamp_control_get_applied_state()); the duty
 *     is the applied fixed-point duty in #LAMP_DUTY_SCALE units, never the
 *     requested brightness alone,
 *   - `connection_state` is the documented "online" literal — publication is
 *     suppressed entirely while disconnected, so this record can only ever
 *     be published online,
 *   - `fw_version` / `hardware` are the safe-charset, bounded config copies,
 *   - `uptime_ms` is the OSAL monotonic clock (ms since boot).
 *
 * Suppression contract: while the transport is disconnected (or before
 * init/deinit) the publish returns immediately and nothing is queued,
 * buffered or retried, so a disconnect can never grow an unbounded queue.
 * A serialization overflow (impossible for the documented bounds unless
 * the module state is corrupted) is logged and the publish is suppressed
 * rather than emitting truncated JSON.  On every publish attempt the
 * last-publish timestamp is advanced so the poll()-driven periodic publish
 * stays rate-limited to `telemetry_period_ms`.
 */
static void tb_app_publish_telemetry(void)
{
    lamp_applied_state_t applied;
    lamp_duty_t duty = LAMP_DUTY_MIN;
    char buf[TB_APPLICATION_TELEMETRY_MAX_BYTES];
    int n;
    const uint32_t now = tb_app_now_ms();

    /* Suppress while disconnected: never queue, never publish. */
    if (!s_tb.initialized || !tb_app_is_connected())
    {
        return;
    }

    memset(&applied, 0, sizeof(applied));
    if (lamp_control_get_applied_state(&applied) != LAMP_OK)
    {
        /* No applied state (e.g. lamp not initialized): report the safe
         * electrical-off record (zeros). */
        memset(&applied, 0, sizeof(applied));
        applied.power = false;
        applied.brightness_percent = 0u;
        applied.output_active = false;
    }
    if (applied.output_active)
    {
        (void)lamp_duty_from_brightness(applied.brightness_percent, &duty);
    }

    n = snprintf(buf, sizeof(buf),
                 "{\"power\":%s,\"brightness\":%u,\"pwm_duty\":%u,"
                 "\"connection_state\":\"%s\",\"fw_version\":\"%s\","
                 "\"hardware\":\"%s\",\"uptime_ms\":%u}",
                 applied.power ? "true" : "false",
                 (unsigned)applied.brightness_percent,
                 (unsigned)duty,
                 TB_APPLICATION_CONNECTION_STATE_ONLINE,
                 s_tb.fw_version,
                 s_tb.hardware,
                 (unsigned)osal_task_get_time_ms());
    if ((n < 0) || ((size_t)n >= sizeof(buf)))
    {
        osal_log_error("[tb_app] telemetry serialization overflow; "
                       "publish suppressed");
        s_tb.last_telemetry_ms = now;
        return;
    }

    if (tb_telemetry_send_json(s_tb.client, buf) != 0)
    {
        /* Best-effort: report and keep the periodic cadence. */
        osal_log_warning("[tb_app] telemetry publish failed");
    }
    s_tb.last_telemetry_ms = now;
}

/**
 * @brief Force the lamp output inactive (fail-off).
 *
 * Idempotent and safe on every rejection path.  The lamp-control fail-off
 * barrier latched here is released by the module itself immediately before
 * the next successful apply (see tb_app_apply_state()), so a rejected
 * attempt can never be followed by a re-enabled output until a complete
 * valid state arrives.
 */
static void tb_app_fail_off(void)
{
    lamp_status_t status = lamp_control_force_inactive();
    if (status != LAMP_OK)
    {
        osal_log_error("[tb_app] fail-off could not force output off: %d",
                       (int)status);
    }
}

/**
 * @brief Apply a complete valid desired state.
 *
 * Releases the module-latched fail-off barrier (idempotent when no barrier
 * is latched) and applies the state through lamp_control.  On an apply
 * failure the output is forced inactive and an error is returned; the
 * caller schedules the bounded retry in that case.
 */
static lamp_status_t tb_app_apply_state(const lamp_state_t *desired)
{
    lamp_status_t status;

    (void)lamp_control_release_fail_off();
    status = lamp_control_apply_state(desired, NULL);
    if (status == LAMP_OK)
    {
        s_tb.applied = *desired;
        s_tb.has_applied = true;
        osal_log_info("[tb_app] applied complete valid desired state: "
                      "power=%s brightness=%u",
                      desired->power ? "on" : "off",
                      (unsigned)desired->brightness_percent);
        /* Successful state change: publish the applied-state telemetry.  The
         * lock is held here (both sync callbacks call us under the lock). */
        tb_app_publish_telemetry();
    }
    else
    {
        osal_log_error("[tb_app] lamp apply failed: %d (fail-off)",
                       (int)status);
        tb_app_fail_off();
    }
    return status;
}

/**
 * @brief Mark the current attempt as failed: fail-off + bounded backoff.
 *
 * Called with the lock held.  Consumes the in-flight attempt, forces the
 * output inactive, increments the retry counter and either schedules the
 * next (bounded) retry or — when the retry budget for this session is
 * exhausted — parks the module in INACTIVE until the next connect.
 */
static void tb_app_fail_attempt(void)
{
    tb_app_fail_off();

    s_tb.attempt_resolved = true;

    if (!tb_app_is_connected())
    {
        /* The transport is gone; the disconnect path owns the reset. */
        s_tb.state = TB_APP_STATE_INACTIVE;
        return;
    }

    s_tb.retries++;
    if (s_tb.retries >= s_tb.max_retries)
    {
        /* Bounded retry budget exhausted for this session: the module
         * stops retrying and waits for the next reconnect (which starts a
         * fresh session and a fresh budget).  The output stays off. */
        osal_log_warning("[tb_app] sync retry budget exhausted (%u); "
                         "output stays off until reconnect",
                         (unsigned)s_tb.retries);
        s_tb.state = TB_APP_STATE_INACTIVE;
        return;
    }

    if (s_tb.backoff_delay_ms == 0u)
    {
        s_tb.backoff_delay_ms = s_tb.retry_initial_ms;
    }
    else
    {
        uint64_t doubled = (uint64_t)s_tb.backoff_delay_ms * 2u;
        if (doubled > (uint64_t)s_tb.retry_max_ms)
        {
            doubled = (uint64_t)s_tb.retry_max_ms;
        }
        s_tb.backoff_delay_ms = (uint32_t)doubled;
    }

    s_tb.backoff_until_ms = tb_app_now_ms() + s_tb.backoff_delay_ms;
    s_tb.state = TB_APP_STATE_BACKOFF;
    osal_log_warning("[tb_app] sync attempt failed (retries=%u); "
                     "retry in %u ms",
                     (unsigned)s_tb.retries,
                     (unsigned)s_tb.backoff_delay_ms);
}

/**
 * @brief Arm a fresh synchronization attempt.
 *
 * Called with the lock held.  Increments the attempt number, publishes a
 * fresh request token (session identity), sets the bounded sync window
 * deadline and moves to SYNCING.  The subscribe/request transport calls are
 * performed by the caller with the lock released.
 */
static void tb_app_start_attempt(void)
{
    s_tb.attempt++;
    s_tb.attempt_resolved = false;
    s_tb.request_token.session = s_tb.session;
    s_tb.request_token.attempt = s_tb.attempt;
    s_tb.attempt_deadline_ms = tb_app_now_ms() + s_tb.sync_timeout_ms;
    s_tb.state = TB_APP_STATE_SYNCING;
}

/**
 * @brief Subscribe to shared updates and request both attributes (one op).
 *
 * Must be called with the lock RELEASED: the platform may invoke the
 * response callback synchronously on an error path, and the callbacks take
 * the lock themselves.
 *
 * @return 0 on full success; non-zero when the subscribe or the request
 *         could not be issued (the caller then fails the attempt).
 */
static int tb_app_transport_subscribe_and_request(void)
{
    /* The platform request API takes `const char *keys[]`, so the array
     * element type must not add another const qualifier. */
    static const char *TB_APP_KEYS[] = {
        TB_APPLICATION_KEY_POWER,
        TB_APPLICATION_KEY_BRIGHTNESS,
    };
    const size_t num_keys =
        sizeof(TB_APP_KEYS) / sizeof(TB_APP_KEYS[0]);
    int rc;

    /* Subscribe to updates on every successful connection. */
    rc = tb_attributes_subscribe(s_tb.client,
                                 tb_app_on_shared_update,
                                 (void *)(uintptr_t)s_tb.session);
    if (rc != 0)
    {
        osal_log_error("[tb_app] shared-attribute subscribe failed: %d",
                       rc);
        return rc;
    }

    /* Request both authoritative attributes as one synchronization
     * operation; the transport itself bounds the attempt with its own
     * timeout, and the module additionally enforces the sync window in
     * tb_application_poll(). */
    rc = tb_attributes_request_shared(
        s_tb.client, TB_APP_KEYS, num_keys,
        tb_app_on_attr_response, &s_tb.request_token,
        s_tb.sync_timeout_ms);
    if (rc != 0)
    {
        osal_log_error("[tb_app] shared-attribute request failed: %d", rc);
        return rc;
    }

    return 0;
}

/**
 * @brief Issue the transport calls for the already-armed attempt.
 *
 * The caller arms the attempt under the lock and releases it before this
 * runs (the platform may invoke the response callback synchronously on an
 * error path, and the callbacks take the lock themselves).  On an immediate
 * transport failure the current attempt is failed under the lock again.
 */
static void tb_app_run_transport(void)
{
    int rc = tb_app_transport_subscribe_and_request();

    if (rc != 0)
    {
        osal_mutex_take(s_tb.lock);
        if (s_tb.initialized && (s_tb.state == TB_APP_STATE_SYNCING) &&
            !s_tb.attempt_resolved)
        {
            tb_app_fail_attempt();
        }
        osal_mutex_give(s_tb.lock);
    }
}

/* --------------------------------------------------------------------- */
/* Desired-state validation                                               */
/* --------------------------------------------------------------------- */

/**
 * @brief Parse and validate a complete desired state payload.
 *
 * Bounded payload: any input longer than TB_APPLICATION_MAX_PAYLOAD_BYTES
 * is rejected before parsing.  Accepts both a flat object and the
 * attribute-response wrapper `{"shared":{...}}` (the shape the ThingsBoard
 * request/response machinery uses).  A state is complete and valid only
 * when `power` is a JSON boolean and `brightness` is an integer JSON
 * number in the closed interval 0..100.  Unknown extra members are ignored
 * (they do not make the authoritative fields less authoritative).
 *
 * @param[in]  json NUL-terminated payload (may be NULL).
 * @param[out] out  Receives the validated state on success.
 * @return true when the payload is a complete valid desired state.
 */
static bool tb_app_parse_state(const char *json, lamp_state_t *out)
{
    cJSON *root;
    cJSON *attrs;
    cJSON *power_item;
    cJSON *brightness_item;
    bool ok = false;

    if ((json == NULL) || (out == NULL) ||
        (strlen(json) > TB_APPLICATION_MAX_PAYLOAD_BYTES))
    {
        return false;
    }

    root = cJSON_Parse(json);
    if (!cJSON_IsObject(root))
    {
        if (root != NULL)
        {
            cJSON_Delete(root);
        }
        return false;
    }

    /* Attribute responses wrap the values under "shared"; updates arrive
     * flat.  Accept both, exactly like the platform attributes module. */
    attrs = cJSON_GetObjectItemCaseSensitive(root, "shared");
    if (!cJSON_IsObject(attrs))
    {
        attrs = root;
    }

    /* Required field + JSON type: power must be a JSON boolean. */
    power_item = cJSON_GetObjectItemCaseSensitive(attrs, "power");
    if (!cJSON_IsBool(power_item))
    {
        goto done;
    }

    /* Required field + JSON type + range: brightness must be an integer
     * JSON number in 0..100 (fractional and out-of-range values are
     * rejected, never wrapped or clamped). */
    brightness_item =
        cJSON_GetObjectItemCaseSensitive(attrs, "brightness");
    if (!cJSON_IsNumber(brightness_item))
    {
        goto done;
    }
    if ((brightness_item->valueint < 0) ||
        (brightness_item->valueint > (int)LAMP_BRIGHTNESS_MAX))
    {
        goto done;
    }
    if ((double)brightness_item->valueint != brightness_item->valuedouble)
    {
        goto done;
    }

    out->power = cJSON_IsTrue(power_item);
    out->brightness_percent = (uint8_t)brightness_item->valueint;
    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}

static bool tb_app_parse_initial_state(const char *json, lamp_state_t *out)
{
    cJSON *root;
    cJSON *attrs;

    if ((json == NULL) || (out == NULL))
    {
        return false;
    }

    root = cJSON_Parse(json);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return false;
    }

    attrs = cJSON_GetObjectItemCaseSensitive(root, "shared");
    if (attrs == NULL)
    {
        attrs = root;
    }
    if (!cJSON_IsObject(attrs) || (cJSON_GetArraySize(attrs) != 0))
    {
        cJSON_Delete(root);
        return false;
    }

    out->power = false;
    out->brightness_percent = 0U;
    cJSON_Delete(root);
    return true;
}

/* --------------------------------------------------------------------- */
/* Transport callbacks (run on the transport thread)                      */
/* --------------------------------------------------------------------- */

/**
 * @brief Shared-attribute update callback (from tb_attributes_subscribe).
 *
 * Sessions, duplicates and stale delivery are rejected here:
 *   - a callback from a session that is not the current one, or while the
 *     module/transport is not connected, is stale and dropped (output
 *     unchanged),
 *   - a payload that is not a complete valid desired state is rejected
 *     with fail-off and a bounded backoff retry,
 *   - a complete valid update identical to the last applied state is a
 *     duplicate retransmission and is ignored,
 *   - a complete valid update (current session) is applied immediately;
 *     after synchronization the applied state stays authoritative.
 */
static void tb_app_on_shared_update(const char *json_payload,
                                    void *user_data)
{
    const uint32_t token_session =
        (uint32_t)(uintptr_t)user_data;
    lamp_state_t desired;

    osal_mutex_take(s_tb.lock);

    if (!s_tb.initialized || !tb_app_is_connected() ||
        (s_tb.state == TB_APP_STATE_INACTIVE))
    {
        /* Stale callback (previous session or leftover after disconnect):
         * dropped; the output is already off for this session. */
        osal_mutex_give(s_tb.lock);
        return;
    }

    /* Session identity: the subscription token must match the current
     * session (guards updates from a previous subscribe cycle). */
    if (token_session != s_tb.session)
    {
        osal_log_warning("[tb_app] dropped update from stale session");
        osal_mutex_give(s_tb.lock);
        return;
    }

    if (json_payload == NULL)
    {
        tb_app_fail_attempt();
        osal_mutex_give(s_tb.lock);
        return;
    }

    if (!tb_app_parse_state(json_payload, &desired))
    {
        /* Invalid or partial data: reject and fail safe — output off. */
        osal_log_warning("[tb_app] rejected invalid/partial update");
        tb_app_fail_attempt();
        osal_mutex_give(s_tb.lock);
        return;
    }

    if ((s_tb.state == TB_APP_STATE_SYNCED) && s_tb.has_applied &&
        (desired.power == s_tb.applied.power) &&
        (desired.brightness_percent == s_tb.applied.brightness_percent))
    {
        /* Duplicate retransmission of the already-applied complete state:
         * no-op (duplicates never re-apply and never enable the output). */
        osal_mutex_give(s_tb.lock);
        return;
    }

    /* Complete valid state for the current session: apply it.  When this
     * happens while an attribute request is still in flight, the attempt is
     * considered resolved so the (older snapshot) response cannot overwrite
     * the authoritative update. */
    if (s_tb.state == TB_APP_STATE_SYNCING)
    {
        s_tb.attempt_resolved = true;
    }
    if (tb_app_apply_state(&desired) == LAMP_OK)
    {
        s_tb.retries = 0u;
        s_tb.state = TB_APP_STATE_SYNCED;
    }
    else
    {
        tb_app_fail_attempt();
    }

    osal_mutex_give(s_tb.lock);
}

/**
 * @brief Attribute request response callback (from tb_attributes_request_shared).
 *
 * Applies only the single first complete valid response for the current
 * attempt of the current session:
 *   - a callback whose session/attempt token does not match the current
 *     attempt is stale or late and is dropped,
 *   - a second callback for an already-resolved attempt is a duplicate and
 *     is dropped,
 *   - a timeout/error result, a missing response or a response that is not
 *     a complete valid desired state fails the attempt (fail-off + bounded
 *     backoff retry),
 *   - a CANCELLED result is owned by the disconnect path (the fail-off and
 *     session reset happen in on_disconnected).
 */
static void tb_app_on_attr_response(tb_request_result_t result,
                                    const char *json_response,
                                    void *user_data)
{
    const tb_app_attempt_token_t *token =
        (const tb_app_attempt_token_t *)user_data;
    lamp_state_t desired;

    osal_mutex_take(s_tb.lock);

    if (!s_tb.initialized || (s_tb.state != TB_APP_STATE_SYNCING))
    {
        /* Stale/late callback from a previous session or after the attempt
         * was already failed/consumed: dropped, output unchanged. */
        osal_mutex_give(s_tb.lock);
        return;
    }

    /* Session identity: session + attempt must match the current request. */
    if ((token == NULL) ||
        (token->session != s_tb.session) ||
        (token->attempt != s_tb.attempt))
    {
        osal_log_warning("[tb_app] dropped stale/late response");
        osal_mutex_give(s_tb.lock);
        return;
    }

    /* Duplicate: the current attempt already completed. */
    if (s_tb.attempt_resolved)
    {
        osal_log_warning("[tb_app] dropped duplicate response");
        osal_mutex_give(s_tb.lock);
        return;
    }

    if (result == TB_REQUEST_RESULT_CANCELLED)
    {
        /* The transport is being torn down; on_disconnected owns the
         * fail-off and the session reset. */
        osal_mutex_give(s_tb.lock);
        return;
    }

    if ((result != TB_REQUEST_RESULT_SUCCESS) || (json_response == NULL))
    {
        /* Timeout or transport error: the sync window failed. */
        osal_log_warning("[tb_app] sync timeout/error (result=%d)",
                         (int)result);
        tb_app_fail_attempt();
        osal_mutex_give(s_tb.lock);
        return;
    }

    if (!tb_app_parse_state(json_response, &desired) &&
        !tb_app_parse_initial_state(json_response, &desired))
    {
        osal_log_warning("[tb_app] rejected invalid/partial sync response");
        tb_app_fail_attempt();
        osal_mutex_give(s_tb.lock);
        return;
    }

    s_tb.attempt_resolved = true;
    if (tb_app_apply_state(&desired) == LAMP_OK)
    {
        s_tb.retries = 0u;
        s_tb.state = TB_APP_STATE_SYNCED;
    }
    else
    {
        tb_app_fail_attempt();
    }

    osal_mutex_give(s_tb.lock);
}
/* --------------------------------------------------------------------- */
/* Server-side RPC control (TASK-113)                                     */
/* --------------------------------------------------------------------- */

/**
 * @brief Publish a response to a server RPC request.
 *
 * The response is built as a single structured JSON object by the caller;
 * this helper serializes it and hands it to the platform responder.  The
 * platform responder only publishes; it never calls back into this module,
 * so this is safe with the module lock held.
 */
static void tb_app_rpc_respond(uint32_t request_id, const cJSON *root)
{
    char *json;

    if (root == NULL)
    {
        return;
    }
    json = cJSON_PrintUnformatted(root);
    if (json == NULL)
    {
        osal_log_error("[tb_app] RPC response build failed (id=%u)",
                       (unsigned)request_id);
        return;
    }
    if (tb_rpc_respond(s_tb.client, request_id, json) != 0)
    {
        osal_log_error("[tb_app] RPC response publish failed (id=%u)",
                       (unsigned)request_id);
    }
    cJSON_free(json);
}

/**
 * @brief Build a success response carrying the resulting desired+applied
 *        state, and publish it.
 *
 * Only ever called AFTER lamp_control_apply_state() returned LAMP_OK, so a
 * `"success":true` response never precedes hardware application.
 */
static void tb_app_rpc_respond_success(uint32_t request_id,
                                       const lamp_state_t *desired,
                                       const lamp_applied_state_t *applied)
{
    cJSON *root;
    cJSON *desired_obj;
    cJSON *applied_obj;

    root = cJSON_CreateObject();
    if (root == NULL)
    {
        return;
    }

    cJSON_AddBoolToObject(root, "success", true);

    if (desired != NULL)
    {
        desired_obj = cJSON_CreateObject();
        if (desired_obj != NULL)
        {
            cJSON_AddBoolToObject(desired_obj, "power", desired->power);
            cJSON_AddNumberToObject(desired_obj, "brightness",
                                    (double)desired->brightness_percent);
            cJSON_AddItemToObject(root, "desired", desired_obj);
        }
    }

    if (applied != NULL)
    {
        applied_obj = cJSON_CreateObject();
        if (applied_obj != NULL)
        {
            cJSON_AddBoolToObject(applied_obj, "power", applied->power);
            cJSON_AddNumberToObject(applied_obj, "brightness",
                                    (double)applied->brightness_percent);
            cJSON_AddBoolToObject(applied_obj, "output_active",
                                  applied->output_active);
            cJSON_AddItemToObject(root, "applied", applied_obj);
        }
    }

    tb_app_rpc_respond(request_id, root);
    cJSON_Delete(root);
}

/**
 * @brief Build and publish a structured error response.
 *
 * @p error is one of the documented classes ("unknown method",
 * "invalid payload", "hardware failure"); @p reason carries the validation
 * detail for invalid payloads and @p method echoes the offending method
 * name for unknown methods (bounded before it reaches this point).
 */
static void tb_app_rpc_respond_error(uint32_t request_id, const char *error,
                                     const char *reason, const char *method)
{
    cJSON *root;

    root = cJSON_CreateObject();
    if (root == NULL)
    {
        return;
    }

    cJSON_AddBoolToObject(root, "success", false);
    cJSON_AddStringToObject(root, "error", error);
    if (reason != NULL)
    {
        cJSON_AddStringToObject(root, "reason", reason);
    }
    if (method != NULL)
    {
        cJSON_AddStringToObject(root, "method", method);
    }

    tb_app_rpc_respond(request_id, root);
    cJSON_Delete(root);
}

/**
 * @brief Apply a validated RPC desired state and report the outcome.
 *
 * The hardware is applied BEFORE any response: a success response is only
 * published after lamp_control_apply_state() returned LAMP_OK, and the
 * applied state is only recorded then.  Any apply failure produces a
 * "hardware failure" error response and leaves the applied state (and the
 * module's desired state) untouched.
 *
 * The lamp control fail-off barrier (latched by a disconnect/rejection
 * fail-off) is intentionally NOT released here: RPC is transient control
 * and must never re-enable the output behind the synchronizer's fail-off
 * — while the barrier is latched the apply is rejected with
 * #LAMP_ERR_BLOCKED_BY_FAIL_OFF and reported as a hardware failure.
 *
 * Telemetry (TASK-114): a successful RPC set is a successful state change,
 * so the full documented health record is published after the success
 * response — never for invalid requests, hardware failures or getState.
 * The record is built by tb_app_publish_telemetry() from the applied lamp
 * state and carries `pwm_duty`, identity and uptime like every other
 * publish.
 */
static void tb_app_rpc_apply_and_respond(uint32_t request_id,
                                         const lamp_state_t *desired)
{
    lamp_applied_state_t applied;

    memset(&applied, 0, sizeof(applied));
    if (lamp_control_apply_state(desired, &applied) != LAMP_OK)
    {
        osal_log_warning("[tb_app] RPC apply failed; hardware failure "
                         "response (id=%u)", (unsigned)request_id);
        tb_app_rpc_respond_error(request_id, "hardware failure", NULL, NULL);
        return;
    }

    /* Record the new desired state so getState (and the sync duplicate
     * detection) stay consistent across the sync and RPC channels. */
    s_tb.applied = *desired;
    s_tb.has_applied = true;

    tb_app_rpc_respond_success(request_id, desired, &applied);
    tb_app_publish_telemetry();
}

/**
 * @brief Snapshot the currently applied hardware state as the unchanged
 *        base for single-field set methods.
 *
 * @return true and a valid @p out on success; false (hardware failure)
 *         when the lamp control cannot report an applied state.
 */
static bool tb_app_rpc_current_state(lamp_state_t *out)
{
    lamp_applied_state_t applied;

    if (lamp_control_get_applied_state(&applied) != LAMP_OK)
    {
        return false;
    }
    out->power = applied.power;
    out->brightness_percent = applied.brightness_percent;
    return true;
}

/** @brief Does the params object contain the named field at all? */
static bool tb_app_rpc_has_field(const cJSON *params, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(params, key) != NULL;
}

/**
 * @brief Parse the required boolean `power` field (exact JSON type).
 *
 * The field must be present (checked by the caller) and a JSON boolean;
 * anything else is a wrong type and leaves @p out untouched.
 */
static bool tb_app_rpc_parse_power(const cJSON *params, bool *out)
{
    const cJSON *item =
        cJSON_GetObjectItemCaseSensitive(params, "power");

    if (!cJSON_IsBool(item))
    {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

/**
 * @brief Is the required `brightness` field present and a JSON number?
 *
 * Separates the "wrong type" rejection from the range rejection so the RPC
 * error response can carry the precise reason.
 */
static bool tb_app_rpc_brightness_type_ok(const cJSON *params)
{
    return cJSON_IsNumber(
        cJSON_GetObjectItemCaseSensitive(params, "brightness"));
}

/**
 * @brief Parse the required `brightness` field (exact type + range).
 *
 * The field must be present (checked by the caller) and a JSON number
 * (checked via tb_app_rpc_brightness_type_ok()); this function then
 * enforces the value contract: an integer (fractional values are rejected,
 * never truncated) in the closed interval 0..100 (never clamped).
 */
static bool tb_app_rpc_parse_brightness(const cJSON *params, uint8_t *out)
{
    const cJSON *item =
        cJSON_GetObjectItemCaseSensitive(params, "brightness");

    if (!cJSON_IsNumber(item))
    {
        return false;
    }
    if ((item->valueint < 0) || (item->valueint > (int)LAMP_BRIGHTNESS_MAX))
    {
        return false;
    }
    if ((double)item->valueint != item->valuedouble)
    {
        return false;
    }
    *out = (uint8_t)item->valueint;
    return true;
}

/** @brief `setPower` handler: requires a boolean `power`. */
static void tb_app_rpc_handle_set_power(uint32_t request_id,
                                        const cJSON *params)
{
    lamp_state_t desired;
    bool power;

    if (!tb_app_rpc_has_field(params, "power"))
    {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "missing power", NULL);
        return;
    }
    if (!tb_app_rpc_parse_power(params, &power))
    {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "wrong type", NULL);
        return;
    }
    if (!tb_app_rpc_current_state(&desired))
    {
        tb_app_rpc_respond_error(request_id, "hardware failure", NULL, NULL);
        return;
    }
    desired.power = power;
    tb_app_rpc_apply_and_respond(request_id, &desired);
}

/** @brief `setBrightness` handler: requires an integer brightness 0..100. */
static void tb_app_rpc_handle_set_brightness(uint32_t request_id,
                                             const cJSON *params)
{
    lamp_state_t desired;
    uint8_t brightness;

    if (!tb_app_rpc_has_field(params, "brightness"))
    {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "missing brightness", NULL);
        return;
    }
    if (!tb_app_rpc_brightness_type_ok(params))
    {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "wrong type", NULL);
        return;
    }
    if (!tb_app_rpc_parse_brightness(params, &brightness))
    {
        /* A JSON number outside 0..100 or a fractional percentage: the
         * value contract is violated (never wrapped, clamped or
         * truncated). */
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "out of range", NULL);
        return;
    }
    if (!tb_app_rpc_current_state(&desired))
    {
        tb_app_rpc_respond_error(request_id, "hardware failure", NULL, NULL);
        return;
    }
    desired.brightness_percent = brightness;
    tb_app_rpc_apply_and_respond(request_id, &desired);
}

/** @brief `setState` handler: requires both boolean `power` and 0..100
 *         integer `brightness`. */
static void tb_app_rpc_handle_set_state(uint32_t request_id,
                                        const cJSON *params)
{
    lamp_state_t desired;
    bool power;
    uint8_t brightness;

    if (!tb_app_rpc_has_field(params, "power"))
    {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "missing power", NULL);
        return;
    }
    if (!tb_app_rpc_has_field(params, "brightness"))
    {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "missing brightness", NULL);
        return;
    }
    if (!tb_app_rpc_parse_power(params, &power) ||
        !tb_app_rpc_brightness_type_ok(params))
    {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "wrong type", NULL);
        return;
    }
    if (!tb_app_rpc_parse_brightness(params, &brightness))
    {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "out of range", NULL);
        return;
    }

    desired.power = power;
    desired.brightness_percent = brightness;
    tb_app_rpc_apply_and_respond(request_id, &desired);
}

/**
 * @brief `getState` handler: returns the desired and applied state.
 *
 * Reports the module's last applied complete valid desired state (from
 * synchronization or a previous successful RPC set) and the lamp's applied
 * hardware state.  The params object is accepted (it must be a JSON object,
 * enforced by the dispatcher) and its contents are ignored: getState takes
 * no arguments.  Reading state never changes the hardware, so no telemetry
 * is published.
 */
static void tb_app_rpc_handle_get_state(uint32_t request_id,
                                        const cJSON *params)
{
    lamp_applied_state_t applied;

    (void)params;

    memset(&applied, 0, sizeof(applied));
    if (lamp_control_get_applied_state(&applied) != LAMP_OK)
    {
        tb_app_rpc_respond_error(request_id, "hardware failure", NULL, NULL);
        return;
    }

    tb_app_rpc_respond_success(request_id,
                               s_tb.has_applied ? &s_tb.applied : NULL,
                               &applied);
}

/**
 * @brief Server RPC callback (from tb_rpc_subscribe_server, transport
 *        thread).
 *
 * The complete validation pipeline for the four documented methods:
 *   - requests that arrive while the module/transport is not connected or
 *     after deinit are dropped (nothing is applied or reported),
 *   - the method name and the params payload are length-bounded BEFORE any
 *     parsing or comparison,
 *   - an empty/missing method or an oversized method is an invalid payload,
 *   - any method other than the four documented ones is an unknown method
 *     (its bounded name is echoed in the error response),
 *   - params must parse as a JSON object; per-method handlers then enforce
 *     the required fields, exact JSON types and the brightness range,
 *   - a validated set applies the hardware BEFORE any success response and
 *     publishes telemetry only on success; getState reads state.
 */
static void tb_app_on_server_rpc(const char *method, const char *params_json,
                                 uint32_t request_id, void *user_data)
{
    const char *method_name = (method != NULL) ? method : "";
    size_t method_len;
    cJSON *params = NULL;

    (void)user_data;

    osal_mutex_take(s_tb.lock);
    if (!s_tb.initialized || !tb_app_is_connected())
    {
        /* Stale request after disconnect/teardown: dropped, output and
         * applied state unchanged. */
        osal_log_warning("[tb_app] dropped RPC request while disconnected");
        osal_mutex_give(s_tb.lock);
        return;
    }

    /* Bounded method: reject before any parsing or comparison. */
    method_len = strlen(method_name);
    if (method_len == 0u)
    {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "missing method", NULL);
        osal_mutex_give(s_tb.lock);
        return;
    }
    if (method_len > TB_APPLICATION_RPC_METHOD_MAX_LEN)
    {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "method too long", NULL);
        osal_mutex_give(s_tb.lock);
        return;
    }

    /* Bounded payload: reject before parsing the params object. */
    if ((params_json != NULL) &&
        (strlen(params_json) > TB_APPLICATION_RPC_PARAMS_MAX_LEN))
    {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "payload too long", NULL);
        osal_mutex_give(s_tb.lock);
        return;
    }

    if ((strcmp(method_name, TB_APPLICATION_RPC_METHOD_SET_POWER) != 0) &&
        (strcmp(method_name, TB_APPLICATION_RPC_METHOD_SET_BRIGHTNESS) != 0) &&
        (strcmp(method_name, TB_APPLICATION_RPC_METHOD_SET_STATE) != 0) &&
        (strcmp(method_name, TB_APPLICATION_RPC_METHOD_GET_STATE) != 0))
    {
        tb_app_rpc_respond_error(request_id, "unknown method", NULL,
                                 method_name);
        osal_mutex_give(s_tb.lock);
        return;
    }

    params = cJSON_Parse((params_json != NULL) ? params_json : "{}");
    if (params == NULL)
    {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "malformed json", NULL);
        osal_mutex_give(s_tb.lock);
        return;
    }
    if (!cJSON_IsObject(params))
    {
        cJSON_Delete(params);
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "wrong type", NULL);
        osal_mutex_give(s_tb.lock);
        return;
    }

    if (strcmp(method_name, TB_APPLICATION_RPC_METHOD_SET_POWER) == 0)
    {
        tb_app_rpc_handle_set_power(request_id, params);
    }
    else if (strcmp(method_name,
                    TB_APPLICATION_RPC_METHOD_SET_BRIGHTNESS) == 0)
    {
        tb_app_rpc_handle_set_brightness(request_id, params);
    }
    else if (strcmp(method_name, TB_APPLICATION_RPC_METHOD_SET_STATE) == 0)
    {
        tb_app_rpc_handle_set_state(request_id, params);
    }
    else
    {
        tb_app_rpc_handle_get_state(request_id, params);
    }

    cJSON_Delete(params);
    osal_mutex_give(s_tb.lock);
}

/**
 * @brief (Re-)arm the server-side RPC control subscription.
 *
 * Called after every successful connection with the module lock released.
 * The platform tb_rpc subscription is idempotent (one server callback
 * total), so reconnecting simply keeps the armed callback; a transport
 * that lost its subscriptions gets them re-registered here.
 */
static void tb_app_subscribe_rpc(void)
{
    int rc;

    if (!s_tb.initialized || (s_tb.client == NULL))
    {
        return;
    }

    rc = tb_rpc_subscribe_server(s_tb.client, tb_app_on_server_rpc, NULL);
    if (rc != 0)
    {
        osal_log_warning("[tb_app] server RPC subscribe failed: %d", rc);
    }
}

/* --------------------------------------------------------------------- */
/* Public lifecycle                                                       */
/* --------------------------------------------------------------------- */

tb_application_status_t tb_application_init(
    const tb_application_config_t *config)
{
    osal_status_t os_rc;

    if ((config == NULL) || (config->client == NULL))
    {
        return TB_APPLICATION_ERR_INVALID_ARGUMENT;
    }

    /* The mutex is created lazily on the first init.  The boot-order
     * contract places this before any transport thread exists, so the
     * create-once check cannot race: tests and production both call init()
     * from a single lifecycle owner before connecting. */
    if (s_tb.lock == NULL)
    {
        os_rc = osal_mutex_create(&s_tb.lock, "tb_app");
        if (os_rc != OSAL_SUCCESS)
        {
            return TB_APPLICATION_ERR_NOT_INITIALIZED;
        }
    }

    osal_mutex_take(s_tb.lock);
    if (s_tb.initialized)
    {
        osal_mutex_give(s_tb.lock);
        return TB_APPLICATION_ERR_ALREADY_INITIALIZED;
    }

    /* Bounded configuration: defaults for zeros, clamps for extremes. */
    s_tb.client = config->client;
    s_tb.sync_timeout_ms =
        tb_app_clamp((config->sync_timeout_ms == 0u)
                         ? TB_APPLICATION_SYNC_TIMEOUT_DEFAULT_MS
                         : config->sync_timeout_ms,
                     TB_APPLICATION_SYNC_TIMEOUT_MIN_MS,
                     TB_APPLICATION_SYNC_TIMEOUT_MAX_MS);
    s_tb.retry_initial_ms =
        tb_app_clamp((config->retry_initial_delay_ms == 0u)
                         ? TB_APPLICATION_RETRY_INITIAL_DELAY_DEFAULT_MS
                         : config->retry_initial_delay_ms,
                     TB_APPLICATION_RETRY_DELAY_MIN_MS,
                     TB_APPLICATION_RETRY_DELAY_MAX_MS);
    s_tb.retry_max_ms =
        tb_app_clamp((config->retry_max_delay_ms == 0u)
                         ? TB_APPLICATION_RETRY_MAX_DELAY_DEFAULT_MS
                         : config->retry_max_delay_ms,
                     TB_APPLICATION_RETRY_DELAY_MIN_MS,
                     TB_APPLICATION_RETRY_DELAY_MAX_MS);
    if (s_tb.retry_max_ms < s_tb.retry_initial_ms)
    {
        s_tb.retry_max_ms = s_tb.retry_initial_ms;
    }
    s_tb.max_retries = (config->max_retries == 0u)
                           ? TB_APPLICATION_MAX_RETRIES_DEFAULT
                           : config->max_retries;
    s_tb.now_fn = config->now_ms;
    s_tb.telemetry_period_ms =
        tb_app_clamp((config->telemetry_period_ms == 0u)
                         ? TB_APPLICATION_TELEMETRY_PERIOD_DEFAULT_MS
                         : config->telemetry_period_ms,
                     TB_APPLICATION_TELEMETRY_PERIOD_MIN_MS,
                     TB_APPLICATION_TELEMETRY_PERIOD_MAX_MS);
    tb_app_copy_telemetry_string(config->fw_version, s_tb.fw_version,
                                 sizeof(s_tb.fw_version));
    tb_app_copy_telemetry_string(config->hardware, s_tb.hardware,
                                 sizeof(s_tb.hardware));

    s_tb.connected = false;
    s_tb.state = TB_APP_STATE_INACTIVE;
    s_tb.session = 0u;
    s_tb.attempt = 0u;
    s_tb.retries = 0u;
    s_tb.attempt_deadline_ms = 0u;
    s_tb.backoff_until_ms = 0u;
    s_tb.backoff_delay_ms = 0u;
    s_tb.attempt_resolved = true;
    memset(&s_tb.request_token, 0, sizeof(s_tb.request_token));
    memset(&s_tb.applied, 0, sizeof(s_tb.applied));
    s_tb.has_applied = false;
    s_tb.last_telemetry_ms = 0u;

    s_tb.initialized = true;
    osal_mutex_give(s_tb.lock);
    return TB_APPLICATION_OK;
}

void tb_application_deinit(void)
{
    /* No-op (with a NULL lock) when init() never succeeded. */
    if (s_tb.lock == NULL)
    {
        return;
    }

    osal_mutex_take(s_tb.lock);
    if (s_tb.initialized)
    {
        s_tb.initialized = false;
        s_tb.connected = false;
        s_tb.state = TB_APP_STATE_INACTIVE;
        s_tb.attempt_resolved = true;
    }
    osal_mutex_give(s_tb.lock);
}

/* --------------------------------------------------------------------- */
/* Public connection glue                                                 */
/* --------------------------------------------------------------------- */

void tb_application_on_connected(tb_client_t *client)
{
    if (client == NULL)
    {
        return;
    }

    osal_mutex_take(s_tb.lock);
    if (!s_tb.initialized || (client != s_tb.client))
    {
        osal_mutex_give(s_tb.lock);
        return;
    }

    /* Fresh session: invalidates every token of the previous session, resets
     * the retry budget and re-synchronizes from scratch on every connect.
     * The output stays off until a complete valid state arrives for this
     * new session (no output is enabled before valid synchronization). */
    s_tb.session++;
    s_tb.retries = 0u;
    s_tb.backoff_delay_ms = 0u;
    s_tb.connected = true;
    tb_app_start_attempt();

    /* Connect trigger (TASK-114): publish the health telemetry record right
     * away.  The lamp output is still off (fresh fail-off until a complete
     * valid state arrives), which is exactly what the record reports.  This
     * runs with the lock held like every other telemetry publish. */
    tb_app_publish_telemetry();
    osal_mutex_give(s_tb.lock);

    tb_app_run_transport();

    /* Server-side RPC control (TASK-113): transient service/test control
     * over the same lamp.  The platform subscription is idempotent, so
     * reconnecting simply (re-)arms the single server-RPC callback. */
    tb_app_subscribe_rpc();
}

void tb_application_on_disconnected(tb_client_t *client)
{
    if (client == NULL)
    {
        return;
    }

    osal_mutex_take(s_tb.lock);
    if (!s_tb.initialized || (client != s_tb.client))
    {
        osal_mutex_give(s_tb.lock);
        return;
    }

    /* Transport loss: invalidate the session (all in-flight callbacks
     * become stale), park the machine and force the lamp output inactive.
     * The fail-off ALWAYS happens on disconnect regardless of progress. */
    s_tb.session++;
    s_tb.state = TB_APP_STATE_INACTIVE;
    s_tb.attempt_resolved = true;
    s_tb.connected = false;
    tb_app_fail_off();
    osal_log_warning("[tb_app] transport lost; lamp forced off, "
                     "sync session invalidated");

    osal_mutex_give(s_tb.lock);
}

void tb_application_poll(tb_client_t *client)
{
    bool run_attempt = false;

    if (client == NULL)
    {
        return;
    }

    osal_mutex_take(s_tb.lock);
    if (!s_tb.initialized || (client != s_tb.client) ||
        (s_tb.state == TB_APP_STATE_INACTIVE) || !tb_app_is_connected())
    {
        /* Inactive/disconnected: the disconnect path owns everything; the
         * next connect restarts synchronization. */
        osal_mutex_give(s_tb.lock);
        return;
    }

    const uint32_t now = tb_app_now_ms();

    if ((s_tb.state == TB_APP_STATE_SYNCING) && !s_tb.attempt_resolved &&
        (now >= s_tb.attempt_deadline_ms))
    {
        /* The bounded sync window expired without a complete valid
         * response: fail-off + bounded backoff, then retry. */
        osal_log_warning("[tb_app] sync window expired; staying off");
        tb_app_fail_attempt();
    }

    if ((s_tb.state == TB_APP_STATE_BACKOFF) &&
        (now >= s_tb.backoff_until_ms))
    {
        tb_app_start_attempt();
        run_attempt = true;
    }

    /* Periodic health telemetry (TASK-114): while connected, publish at most
     * once per telemetry_period_ms.  Every successful publish (connect and
     * change triggers included) advances the last-publish timestamp, so a
     * burst of changes or a fast poll loop can never produce more than one
     * periodic publish per period.  Disconnected suppression is enforced by
     * the early return above (state == INACTIVE or transport down). */
    if ((uint32_t)(now - s_tb.last_telemetry_ms) >= s_tb.telemetry_period_ms)
    {
        tb_app_publish_telemetry();
    }
    osal_mutex_give(s_tb.lock);

    if (run_attempt)
    {
        tb_app_run_transport();
    }
}

/* --------------------------------------------------------------------- */
/* Public state query                                                     */
/* --------------------------------------------------------------------- */

bool tb_application_is_synchronized(tb_client_t *client)
{
    bool synced;

    osal_mutex_take(s_tb.lock);
    synced = s_tb.initialized && (s_tb.state == TB_APP_STATE_SYNCED) &&
             ((client == NULL) || (client == s_tb.client)) &&
             tb_app_is_connected();
    osal_mutex_give(s_tb.lock);

    return synced;
}

tb_application_status_t tb_application_get_desired_state(
    tb_client_t *client, bool *has_state,
    tb_application_desired_state_t *out)
{
    tb_application_status_t status = TB_APPLICATION_OK;

    if ((has_state == NULL) && (out == NULL))
    {
        return TB_APPLICATION_ERR_INVALID_ARGUMENT;
    }

    osal_mutex_take(s_tb.lock);
    if (!s_tb.initialized)
    {
        status = TB_APPLICATION_ERR_NOT_INITIALIZED;
    }
    else if ((client != NULL) && (client != s_tb.client))
    {
        status = TB_APPLICATION_ERR_CLIENT_MISMATCH;
    }
    else
    {
        if (has_state != NULL)
        {
            *has_state = s_tb.has_applied;
        }
        if (out != NULL)
        {
            out->power = s_tb.applied.power;
            out->brightness_percent = s_tb.applied.brightness_percent;
        }
    }
    osal_mutex_give(s_tb.lock);

    return status;
}