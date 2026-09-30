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
 *   - telemetry and health reporting (TASK-114): the documented six-field
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
#include "tb_state_sync.h"
#include "tb_telemetry.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifndef CONFIG_KLC_LAMP_TYPE_RGB
#define CONFIG_KLC_LAMP_TYPE_RGB 0
#endif

#ifdef CONFIG_KLC_DEFAULT_BRIGHTNESS_PERCENT
#define TB_APPLICATION_DEFAULT_BRIGHTNESS ((uint8_t)CONFIG_KLC_DEFAULT_BRIGHTNESS_PERCENT)
#else
#define TB_APPLICATION_DEFAULT_BRIGHTNESS 0u
#endif

/* --------------------------------------------------------------------- */
/* Internal constants                                                     */
/* --------------------------------------------------------------------- */

/** @brief Shared-attribute keys requested as one synchronization operation. */
#define TB_APPLICATION_KEY_POWER "power"
#define TB_APPLICATION_KEY_BRIGHTNESS "brightness"
#define TB_APPLICATION_KEY_RED "red"
#define TB_APPLICATION_KEY_GREEN "green"
#define TB_APPLICATION_KEY_BLUE "blue"

/** @brief Documented server-side RPC method names (TASK-113). */
#define TB_APPLICATION_RPC_METHOD_SET_POWER "setPower"
#define TB_APPLICATION_RPC_METHOD_SET_BRIGHTNESS "setBrightness"
#define TB_APPLICATION_RPC_METHOD_SET_STATE "setState"
#define TB_APPLICATION_RPC_METHOD_GET_STATE "getState"
#define TB_APPLICATION_RPC_METHOD_SET_COLOR "setColor"

/* --------------------------------------------------------------------- */
/* Internal module state                                                  */
/* --------------------------------------------------------------------- */

typedef struct tb_app_module {
    bool               initialized;
    bool               connected;
    bool               output_suspended;
    tb_client_t       *client;
    tb_application_rssi_fn_t rssi_fn;
    osal_mutex_id_t    lock;

    /* Last applied complete valid state (for telemetry/reporting). */
    lamp_state_t       applied;
    bool               has_applied;
    bool               rpc_state_changed;
    lamp_state_t       rpc_state_pending;

    /* Telemetry / health reporting (TASK-114). */
    uint32_t           telemetry_period_ms; /**< Bounded periodic interval (0 = default). */
    char               hardware[TB_APPLICATION_HARDWARE_MAX_LEN + 1u];     /**< Safe-charset copy. */

    /* Firmware update cooperation. */
    bool               firmware_hint;      /**< fw_* shared attributes changed. */
} tb_app_module_t;

static tb_app_module_t s_tb;

static bool tb_app_is_connected(void);

static void tb_app_publish_applied_attributes(const void *state,
                                              void *user_data)
{
    const lamp_state_t *applied = (const lamp_state_t *)state;
    char json[160];
    int written;

    (void)user_data;
    if (applied == NULL || !s_tb.connected || s_tb.client == NULL ||
        !tb_client_is_connected(s_tb.client)) {
        return;
    }
#if CONFIG_KLC_LAMP_TYPE_RGB
    written = snprintf(json, sizeof(json),
                       "{\"power\":%s,\"brightness\":%u,\"red\":%u,"
                       "\"green\":%u,\"blue\":%u}",
                       applied->power ? "true" : "false",
                       (unsigned)applied->brightness_percent,
                       (unsigned)applied->red,
                       (unsigned)applied->green,
                       (unsigned)applied->blue);
#else
    written = snprintf(json, sizeof(json), "{\"power\":%s,\"brightness\":%u}",
                       applied->power ? "true" : "false",
                       (unsigned)applied->brightness_percent);
#endif
    if (written > 0 && (size_t)written < sizeof(json)) {
        (void)tb_attributes_send_json(s_tb.client, json);
    }
}

/* --------------------------------------------------------------------- */
/* Helpers (all internal helpers expect the lock held unless noted)        */
/* --------------------------------------------------------------------- */

static bool tb_app_is_connected(void)
{
    return s_tb.connected && (s_tb.client != NULL) &&
           tb_client_is_connected(s_tb.client);
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
 * Called with the module lock held.  Serializes the six documented fields
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
 *   - `hardware` is the safe-charset, bounded config copy,
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
static void tb_app_publish_telemetry(void *user_data)
{
    lamp_applied_state_t applied;
    lamp_duty_t duty = LAMP_DUTY_MIN;
    char buf[TB_APPLICATION_TELEMETRY_MAX_BYTES];
    int n;

    /* Suppress while disconnected: never queue, never publish. */
    (void)user_data;
    if (!s_tb.initialized || !s_tb.connected || s_tb.client == NULL ||
        !tb_client_is_connected(s_tb.client))
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

    int rssi = 0;
    const bool have_rssi = s_tb.rssi_fn != NULL && s_tb.rssi_fn(&rssi) == 0;
    char rssi_suffix[24] = "";
    if (have_rssi) {
        (void)snprintf(rssi_suffix, sizeof(rssi_suffix), ",\"rssi\":%d", rssi);
    }

    n = snprintf(buf, sizeof(buf),
#if CONFIG_KLC_LAMP_TYPE_RGB
                 "{\"power\":%s,\"brightness\":%u,\"red\":%u,"
                 "\"green\":%u,\"blue\":%u,\"pwm_duty\":%u,"
#else
                 "{\"power\":%s,\"brightness\":%u,\"pwm_duty\":%u,"
#endif
                 "\"connection_state\":\"%s\","
                 "\"hardware\":\"%s\",\"uptime_ms\":%u%s}",
                 applied.power ? "true" : "false",
                 (unsigned)applied.brightness_percent,
#if CONFIG_KLC_LAMP_TYPE_RGB
                 (unsigned)applied.red,
                 (unsigned)applied.green,
                 (unsigned)applied.blue,
#endif
                 (unsigned)duty,
                 TB_APPLICATION_CONNECTION_STATE_ONLINE,
                 s_tb.hardware,
                 (unsigned)osal_task_get_time_ms(), rssi_suffix);
    if ((n < 0) || ((size_t)n >= sizeof(buf)))
    {
        osal_log_error("[tb_app] telemetry serialization overflow; "
                       "publish suppressed");
        return;
    }

    if (tb_telemetry_send_json(s_tb.client, buf) != 0)
    {
        /* Best-effort: report and keep the periodic cadence. */
        osal_log_warning("[tb_app] telemetry publish failed");
    }
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
static void tb_app_fail_off(void *user_data)
{
    (void)user_data;
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
static bool tb_app_apply_state(const void *state, bool output_suspended,
                               void *user_data)
{
    const lamp_state_t *desired = (const lamp_state_t *)state;
    lamp_status_t status;

    (void)user_data;
    if (desired == NULL) {
        return false;
    }
    if (output_suspended)
    {
        osal_mutex_take(s_tb.lock);
        s_tb.applied = *desired;
        s_tb.has_applied = true;
        osal_mutex_give(s_tb.lock);
        osal_log_info("[tb_app] desired state stored while output is "
                      "suspended: power=%s brightness=%u",
                      desired->power ? "on" : "off",
                      (unsigned)desired->brightness_percent);
        return true;
    }

    (void)lamp_control_release_fail_off();
    status = lamp_control_apply_state(desired, NULL);
    if (status == LAMP_OK)
    {
        osal_mutex_take(s_tb.lock);
        s_tb.applied = *desired;
        s_tb.has_applied = true;
        osal_mutex_give(s_tb.lock);
        osal_log_info("[tb_app] applied complete valid desired state: "
                      "power=%s brightness=%u",
                      desired->power ? "on" : "off",
                      (unsigned)desired->brightness_percent);
    }
    else
    {
        osal_log_error("[tb_app] lamp apply failed: %d (fail-off)",
                       (int)status);
        tb_app_fail_off(NULL);
    }
    return status == LAMP_OK;
}

/* --------------------------------------------------------------------- */
/* Desired-state validation                                               */
/* --------------------------------------------------------------------- */

#if CONFIG_KLC_LAMP_TYPE_RGB
static bool tb_app_parse_rgb_channel(const cJSON *attrs, const char *key,
                                     uint8_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(attrs, key);

    if (!cJSON_IsNumber(item) || item->valueint < 0 || item->valueint > 255 ||
        (double)item->valueint != item->valuedouble) {
        return false;
    }
    *out = (uint8_t)item->valueint;
    return true;
}
#endif

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
static bool tb_app_parse_state_scope(const char *json, const char *scope,
                                     lamp_state_t *out)
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

    /* Responses wrap values under a requested scope; updates arrive flat. */
    if (scope != NULL) {
        attrs = cJSON_GetObjectItemCaseSensitive(root, scope);
        if (!cJSON_IsObject(attrs)) {
            cJSON_Delete(root);
            return false;
        }
    } else {
        attrs = cJSON_GetObjectItemCaseSensitive(root, "shared");
        if (!cJSON_IsObject(attrs)) {
            attrs = root;
        }
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
#if CONFIG_KLC_LAMP_TYPE_RGB
    {
        const bool has_red = cJSON_GetObjectItemCaseSensitive(
            attrs, TB_APPLICATION_KEY_RED) != NULL;
        const bool has_green = cJSON_GetObjectItemCaseSensitive(
            attrs, TB_APPLICATION_KEY_GREEN) != NULL;
        const bool has_blue = cJSON_GetObjectItemCaseSensitive(
            attrs, TB_APPLICATION_KEY_BLUE) != NULL;
        if (!has_red && !has_green && !has_blue) {
            out->red = 255u;
            out->green = 255u;
            out->blue = 255u;
        } else if (!has_red || !has_green || !has_blue ||
                   !tb_app_parse_rgb_channel(attrs, TB_APPLICATION_KEY_RED, &out->red) ||
                   !tb_app_parse_rgb_channel(attrs, TB_APPLICATION_KEY_GREEN, &out->green) ||
                   !tb_app_parse_rgb_channel(attrs, TB_APPLICATION_KEY_BLUE, &out->blue)) {
            goto done;
        }
    }
#else
    out->red = 255u;
    out->green = 255u;
    out->blue = 255u;
#endif
    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}

static bool tb_app_parse_state(const char *json, lamp_state_t *out)
{
    return tb_app_parse_state_scope(json, NULL, out);
}

static bool tb_app_is_ota_key(const char *key, bool *is_firmware)
{
    if ((key != NULL) && (strncmp(key, "fw_", 3) == 0))
    {
        *is_firmware = true;
        return true;
    }
    return (key != NULL) && (strncmp(key, "sw_", 3) == 0);
}

/**
 * @brief Does a shared update carry ONLY ThingsBoard OTA keys?
 *
 * Called with the lock held.  ThingsBoard pushes `fw_*`/`sw_*` (and
 * `{"deleted":[...]}` on unassignment) through the same shared-attribute
 * channel; those must never fail the lamp synchronization.  Any `fw_*`
 * key raises the firmware hint, even when lamp keys are present too.
 */
static bool tb_app_is_ota_only_update(const char *json)
{
    cJSON *root;
    cJSON *attrs;
    cJSON *item;
    bool is_firmware = false;
    bool only_ota = true;
    bool any = false;

    if (strlen(json) > TB_APPLICATION_MAX_PAYLOAD_BYTES)
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
    if (!cJSON_IsObject(attrs))
    {
        attrs = root;
    }

    cJSON_ArrayForEach(item, attrs)
    {
        any = true;
        if ((item->string != NULL) && (strcmp(item->string, "deleted") == 0) &&
            cJSON_IsArray(item))
        {
            const cJSON *deleted;
            cJSON_ArrayForEach(deleted, item)
            {
                if (!cJSON_IsString(deleted) ||
                    !tb_app_is_ota_key(deleted->valuestring, &is_firmware))
                {
                    only_ota = false;
                }
            }
        }
        else if (!tb_app_is_ota_key(item->string, &is_firmware))
        {
            only_ota = false;
        }
    }
    cJSON_Delete(root);

    if (is_firmware)
    {
        osal_mutex_take(s_tb.lock);
        s_tb.firmware_hint = true;
        osal_mutex_give(s_tb.lock);
        osal_log_info("[tb_app] firmware attributes changed; firmware "
                      "check requested");
    }
    return any && only_ota;
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
    if (attrs != NULL || cJSON_GetObjectItemCaseSensitive(root, "client") != NULL) {
        const cJSON *client = cJSON_GetObjectItemCaseSensitive(root, "client");
        if ((attrs != NULL && (!cJSON_IsObject(attrs) || cJSON_GetArraySize(attrs) != 0)) ||
            (client != NULL && (!cJSON_IsObject(client) || cJSON_GetArraySize(client) != 0))) {
            cJSON_Delete(root);
            return false;
        }
    } else if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 0) {
        cJSON_Delete(root);
        return false;
    }

    out->power = false;
    out->brightness_percent = TB_APPLICATION_DEFAULT_BRIGHTNESS;
    out->red = 255u;
    out->green = 255u;
    out->blue = 255u;
    cJSON_Delete(root);
    return true;
}

static tb_state_sync_parse_result_t tb_app_parse_state_callback(
    const char *json, tb_state_sync_source_t source, void *state_out,
    void *user_data)
{
    bool valid = false;
    (void)user_data;
    if (source == TB_STATE_SYNC_SOURCE_CLIENT) {
        valid = tb_app_parse_state_scope(json, "client", state_out);
        if (valid) {
            osal_log_info("[tb_app] sync source=client");
        }
    } else if (source == TB_STATE_SYNC_SOURCE_SHARED) {
        valid = tb_app_parse_state_scope(json, "shared", state_out);
        if (valid) {
            osal_log_info("[tb_app] sync source=shared");
        }
    } else if (source == TB_STATE_SYNC_SOURCE_DEFAULT) {
        valid = tb_app_parse_initial_state(json, state_out);
        if (valid) {
            osal_log_info("[tb_app] sync source=default");
        }
    } else {
        if (json != NULL && tb_app_is_ota_only_update(json)) {
            return TB_STATE_SYNC_PARSE_IGNORED;
        }
        valid = tb_app_parse_state(json, state_out);
    }
    return valid ? TB_STATE_SYNC_PARSE_VALID : TB_STATE_SYNC_PARSE_INVALID;
}

static bool tb_app_states_equal(const void *left, const void *right,
                                void *user_data)
{
    const lamp_state_t *left_state = (const lamp_state_t *)left;
    const lamp_state_t *right_state = (const lamp_state_t *)right;
    (void)user_data;
    return left_state->power == right_state->power &&
           left_state->brightness_percent == right_state->brightness_percent &&
           left_state->red == right_state->red &&
           left_state->green == right_state->green &&
           left_state->blue == right_state->blue;
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
#if CONFIG_KLC_LAMP_TYPE_RGB
            cJSON_AddNumberToObject(desired_obj, "red", (double)desired->red);
            cJSON_AddNumberToObject(desired_obj, "green", (double)desired->green);
            cJSON_AddNumberToObject(desired_obj, "blue", (double)desired->blue);
#endif
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
#if CONFIG_KLC_LAMP_TYPE_RGB
            cJSON_AddNumberToObject(applied_obj, "red", (double)applied->red);
            cJSON_AddNumberToObject(applied_obj, "green", (double)applied->green);
            cJSON_AddNumberToObject(applied_obj, "blue", (double)applied->blue);
#endif
            cJSON_AddItemToObject(root, "applied", applied_obj);
        }
    }

    tb_app_rpc_respond(request_id, root);
    cJSON_Delete(root);
}

/**
 * @brief Publish a structured error response (shared platform format).
 *
 * @p error is one of the documented classes ("invalid payload",
 * "hardware failure"); @p reason carries the validation detail.
 */
static void tb_app_rpc_respond_error(uint32_t request_id, const char *error,
                                     const char *reason, const char *method)
{
    (void)tb_state_sync_respond_rpc_error(s_tb.client, request_id, error,
                                          reason, method);
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
    if (s_tb.output_suspended)
    {
        s_tb.applied = *desired;
        s_tb.has_applied = true;
        s_tb.rpc_state_pending = *desired;
        s_tb.rpc_state_changed = true;
        tb_app_publish_applied_attributes(desired, NULL);
        (void)lamp_control_get_applied_state(&applied);
        tb_app_rpc_respond_success(request_id, desired, &applied);
        return;
    }
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
    s_tb.rpc_state_pending = *desired;
    s_tb.rpc_state_changed = true;

    tb_app_publish_applied_attributes(desired, NULL);
    tb_app_rpc_respond_success(request_id, desired, &applied);
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
    out->red = applied.red;
    out->green = applied.green;
    out->blue = applied.blue;
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

#if CONFIG_KLC_LAMP_TYPE_RGB
static bool tb_app_rpc_parse_rgb(const cJSON *params, lamp_state_t *out)
{
    return tb_app_parse_rgb_channel(params, "red", &out->red) &&
           tb_app_parse_rgb_channel(params, "green", &out->green) &&
           tb_app_parse_rgb_channel(params, "blue", &out->blue);
}

/** @brief Update only the channels present in @p params; at least one is required. */
static bool tb_app_rpc_parse_rgb_partial(const cJSON *params, lamp_state_t *out)
{
    static const char *const keys[] = { "red", "green", "blue" };
    lamp_state_t parsed = *out;
    uint8_t *const channels[] = { &parsed.red, &parsed.green, &parsed.blue };
    bool any = false;

    for (size_t i = 0; i < 3u; ++i) {
        if (!tb_app_rpc_has_field(params, keys[i])) {
            continue;
        }
        if (!tb_app_parse_rgb_channel(params, keys[i], channels[i])) {
            return false;
        }
        any = true;
    }
    if (any) {
        *out = parsed;
    }
    return any;
}

static void tb_app_rpc_handle_set_color(uint32_t request_id,
                                        const cJSON *params)
{
    lamp_state_t desired;

    if (!tb_app_rpc_current_state(&desired)) {
        tb_app_rpc_respond_error(request_id, "hardware failure", NULL, NULL);
        return;
    }
    if (!tb_app_rpc_parse_rgb_partial(params, &desired)) {
        tb_app_rpc_respond_error(request_id, "invalid payload",
                                 "invalid RGB channel", NULL);
        return;
    }
    tb_app_rpc_apply_and_respond(request_id, &desired);
}
#endif

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

    if (!tb_app_rpc_current_state(&desired)) {
        memset(&desired, 0, sizeof(desired));
        desired.red = 255u;
        desired.green = 255u;
        desired.blue = 255u;
    }
    desired.power = power;
    desired.brightness_percent = brightness;
#if CONFIG_KLC_LAMP_TYPE_RGB
    if (tb_app_rpc_has_field(params, "red") ||
        tb_app_rpc_has_field(params, "green") ||
        tb_app_rpc_has_field(params, "blue")) {
        if (!tb_app_rpc_has_field(params, "red") ||
            !tb_app_rpc_has_field(params, "green") ||
            !tb_app_rpc_has_field(params, "blue") ||
            !tb_app_rpc_parse_rgb(params, &desired)) {
            tb_app_rpc_respond_error(request_id, "invalid payload",
                                     "invalid RGB channel", NULL);
            return;
        }
    }
#endif
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

/* The engine enforces the documented limits before calling handlers. */
#if (TB_APPLICATION_RPC_METHOD_MAX_LEN != TB_STATE_SYNC_MAX_RPC_METHOD_LEN) || \
    (TB_APPLICATION_RPC_PARAMS_MAX_LEN != TB_STATE_SYNC_MAX_RPC_PAYLOAD_BYTES)
#error "tb_application RPC limits must match tb_state_sync"
#endif

typedef void (*tb_app_rpc_handler_t)(uint32_t request_id,
                                     const cJSON *params);

typedef struct tb_app_rpc_method {
    const char *name;
    tb_app_rpc_handler_t handler;
} tb_app_rpc_method_t;

static tb_app_rpc_method_t s_rpc_methods[] = {
    { TB_APPLICATION_RPC_METHOD_SET_POWER, tb_app_rpc_handle_set_power },
    { TB_APPLICATION_RPC_METHOD_SET_BRIGHTNESS,
      tb_app_rpc_handle_set_brightness },
    { TB_APPLICATION_RPC_METHOD_SET_STATE, tb_app_rpc_handle_set_state },
#if CONFIG_KLC_LAMP_TYPE_RGB
    { TB_APPLICATION_RPC_METHOD_SET_COLOR, tb_app_rpc_handle_set_color },
#endif
    { TB_APPLICATION_RPC_METHOD_GET_STATE, tb_app_rpc_handle_get_state },
};

/**
 * @brief Server RPC callback registered per method with tb_state_sync.
 *
 * The engine has already bounded the method name and params payload and
 * answered unknown methods.  This callback drops stale requests, requires
 * params to be a JSON object and runs the method handler (which enforces
 * required fields, exact types and ranges) under the module lock.
 */
static void tb_app_on_server_rpc(const char *method, const char *params_json,
                                 uint32_t request_id, void *user_data)
{
    const tb_app_rpc_method_t *entry = user_data;
    cJSON *params = NULL;
    bool state_changed;
    bool publish_telemetry;
    lamp_state_t pending_state;

    (void)method;

    osal_mutex_take(s_tb.lock);
    s_tb.rpc_state_changed = false;
    if (!s_tb.initialized || !tb_app_is_connected())
    {
        /* Stale request after disconnect/teardown: dropped, output and
         * applied state unchanged. */
        osal_log_warning("[tb_app] dropped RPC request while disconnected");
        osal_mutex_give(s_tb.lock);
        return;
    }

    params = cJSON_Parse(params_json);
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

    entry->handler(request_id, params);

    cJSON_Delete(params);
    state_changed = s_tb.rpc_state_changed;
    pending_state = s_tb.rpc_state_pending;
    publish_telemetry = !s_tb.output_suspended;
    s_tb.rpc_state_changed = false;
    osal_mutex_give(s_tb.lock);
    if (state_changed) {
        (void)tb_state_sync_record_state(&pending_state);
        if (publish_telemetry) {
            tb_state_sync_note_state_change();
        }
    }
}

/* --------------------------------------------------------------------- */
/* Public lifecycle                                                       */
/* --------------------------------------------------------------------- */

tb_application_status_t tb_application_init(
    const tb_application_config_t *config)
{
    osal_status_t os_rc;
    tb_state_sync_status_t sync_status;
    tb_state_sync_config_t sync_config = { 0 };
    static const char *const state_keys[] = {
        TB_APPLICATION_KEY_POWER,
        TB_APPLICATION_KEY_BRIGHTNESS,
#if CONFIG_KLC_LAMP_TYPE_RGB
        TB_APPLICATION_KEY_RED,
        TB_APPLICATION_KEY_GREEN,
        TB_APPLICATION_KEY_BLUE,
#endif
    };

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

    s_tb.client = config->client;
    s_tb.rssi_fn = config->rssi_dbm;
    tb_app_copy_telemetry_string(config->hardware, s_tb.hardware,
                                 sizeof(s_tb.hardware));

    s_tb.connected = false;
    s_tb.output_suspended = false;
    memset(&s_tb.applied, 0, sizeof(s_tb.applied));
    s_tb.has_applied = false;
    s_tb.rpc_state_changed = false;
    s_tb.firmware_hint = false;

    s_tb.initialized = true;
    osal_mutex_give(s_tb.lock);

    sync_config.client = config->client;
    sync_config.client_keys = state_keys;
    sync_config.client_key_count = sizeof(state_keys) / sizeof(state_keys[0]);
    sync_config.shared_keys = state_keys;
    sync_config.shared_key_count = sizeof(state_keys) / sizeof(state_keys[0]);
    sync_config.max_payload_bytes = TB_APPLICATION_MAX_PAYLOAD_BYTES;
    sync_config.state_size = sizeof(lamp_state_t);
    sync_config.sync_timeout_ms = config->sync_timeout_ms;
    sync_config.retry_initial_delay_ms = config->retry_initial_delay_ms;
    sync_config.retry_max_delay_ms = config->retry_max_delay_ms;
    sync_config.max_retries = config->max_retries;
    sync_config.telemetry_period_ms = config->telemetry_period_ms;
    sync_config.now_ms = config->now_ms;
    sync_config.parse_state = tb_app_parse_state_callback;
    sync_config.apply_state = tb_app_apply_state;
    sync_config.states_equal = tb_app_states_equal;
    sync_config.force_inactive = tb_app_fail_off;
    sync_config.publish_applied_attributes =
        tb_app_publish_applied_attributes;
    sync_config.publish_telemetry = tb_app_publish_telemetry;
    sync_config.user_data = &s_tb;
    sync_status = tb_state_sync_init(&sync_config);
    for (size_t i = 0u; sync_status == TB_STATE_SYNC_OK &&
                        i < sizeof(s_rpc_methods) / sizeof(s_rpc_methods[0]);
         ++i) {
        if (tb_state_sync_register_rpc_handler(s_rpc_methods[i].name,
                                               tb_app_on_server_rpc,
                                               &s_rpc_methods[i]) != 0) {
            sync_status = TB_STATE_SYNC_ERR_NO_RESOURCE;
        }
    }
    if (sync_status != TB_STATE_SYNC_OK) {
        tb_state_sync_deinit();
        osal_mutex_take(s_tb.lock);
        s_tb.initialized = false;
        s_tb.client = NULL;
        osal_mutex_give(s_tb.lock);
        return sync_status == TB_STATE_SYNC_ERR_ALREADY_INITIALIZED
                   ? TB_APPLICATION_ERR_ALREADY_INITIALIZED
                   : TB_APPLICATION_ERR_NOT_INITIALIZED;
    }
    return TB_APPLICATION_OK;
}

void tb_application_deinit(void)
{
    if (s_tb.lock == NULL) {
        return;
    }
    tb_state_sync_deinit();
    osal_mutex_take(s_tb.lock);
    s_tb.initialized = false;
    s_tb.connected = false;
    s_tb.output_suspended = false;
    s_tb.client = NULL;
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
    if (!s_tb.initialized || client != s_tb.client) {
        osal_mutex_give(s_tb.lock);
        return;
    }
    s_tb.connected = true;
    osal_mutex_give(s_tb.lock);
    tb_state_sync_on_connected(client);
}

void tb_application_on_disconnected(tb_client_t *client)
{
    if (client == NULL)
    {
        return;
    }

    osal_mutex_take(s_tb.lock);
    if (!s_tb.initialized || client != s_tb.client) {
        osal_mutex_give(s_tb.lock);
        return;
    }
    s_tb.connected = false;
    osal_mutex_give(s_tb.lock);
    tb_state_sync_on_disconnected(client);
    osal_log_warning("[tb_app] transport lost; lamp forced off, "
                     "sync session invalidated");
}

void tb_application_poll(tb_client_t *client)
{
    tb_state_sync_poll(client);
}

/* --------------------------------------------------------------------- */
/* Public state query                                                     */
/* --------------------------------------------------------------------- */

bool tb_application_is_synchronized(tb_client_t *client)
{
    return tb_state_sync_is_synchronized(client);
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

bool tb_application_take_firmware_hint(void)
{
    bool hint;

    if (s_tb.lock == NULL)
    {
        return false;
    }
    osal_mutex_take(s_tb.lock);
    hint = s_tb.initialized && s_tb.firmware_hint;
    s_tb.firmware_hint = false;
    osal_mutex_give(s_tb.lock);
    return hint;
}

void tb_application_set_output_suspended(bool suspended)
{
    if (s_tb.lock == NULL) {
        return;
    }
    osal_mutex_take(s_tb.lock);
    if (!s_tb.initialized || s_tb.output_suspended == suspended) {
        osal_mutex_give(s_tb.lock);
        return;
    }
    s_tb.output_suspended = suspended;
    if (suspended)
    {
        osal_log_info("[tb_app] lamp output suspended (OTA indicator)");
    }
    osal_mutex_give(s_tb.lock);
    tb_state_sync_set_output_suspended(suspended);
}