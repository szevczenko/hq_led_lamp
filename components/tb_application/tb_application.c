/**
 * @file tb_application.c
 * @brief ThingsBoard desired-state synchronization implementation (TASK-112)
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
 *   - lock discipline: all module state is guarded by one OSAL mutex.
 *     Transport calls (subscribe/request) are always issued with the lock
 *     released, because the platform can invoke the response callback
 *     synchronously on an error path; the callbacks take the lock
 *     themselves, so no re-entrant deadlock can form.  lamp_control never
 *     calls back into this module, so holding the module lock while calling
 *     lamp_control is safe.
 */

#include "tb_application.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "lamp_control.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "tb_attributes.h"
#include "tb_client.h"

/* --------------------------------------------------------------------- */
/* Internal constants                                                     */
/* --------------------------------------------------------------------- */

/** @brief Shared-attribute keys requested as one synchronization operation. */
#define TB_APPLICATION_KEY_POWER "power"
#define TB_APPLICATION_KEY_BRIGHTNESS "brightness"

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

    if (!tb_app_parse_state(json_response, &desired))
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
    osal_mutex_give(s_tb.lock);

    tb_app_run_transport();
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