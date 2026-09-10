/**
 * @file tb_application_test.c
 * @brief Host mock tests for the ThingsBoard application logic: desired-
 *        state synchronizer (TASK-112), server-side RPC control (TASK-113)
 *        and telemetry / health reporting (TASK-114)
 *
 * Runs the production components/tb_application code against the REAL
 * platform ThingsBoard client/attributes/RPC sources (tb_client.c,
 * tb_attributes.c, tb_rpc.c, ...) and the platform's own mqtt_app test
 * double (mqtt_app_mock), exactly like the platform's tb_tests, plus a
 * lamp-control double (lamp_control_mock.h).
 *
 * Transport simulation
 * --------------------
 *   - tb_client_connect() -> mqtt_app_init() -> the mock fires the
 *     connect callback -> tb_client's on_connect -> the product glue ->
 *     tb_application_on_connected(): the synchronizer subscribes to shared
 *     updates and requests power+brightness as ONE shared-attribute
 *     request (the RGB lamp example's primitives).
 *   - mqtt_app_mock_deliver_message("v1/devices/me/attributes/response/<id>")
 *     delivers attribute responses through the real tb_attributes
 *     request/response machinery; the synchronizer's response callback
 *     runs synchronously on the test thread.
 *   - mqtt_app_mock_deliver_message("v1/devices/me/attributes") delivers
 *     shared-attribute updates through tb_attributes_subscribe.
 *   - mqtt_app_mock_deliver_message("v1/devices/me/rpc/request/<id>")
 *     delivers server RPC requests through the real tb_rpc machinery; the
 *     RPC handler runs synchronously and publishes the response to
 *     "v1/devices/me/rpc/response/<id>", which the mock records.
 *   - mqtt_app_mock_simulate_remote_disconnect() / _simulate_connect()
 *     drive the disconnect/connect glue.
 *
 * Determinism: tb_application_config_t::now_ms injects a test clock that
 * drives the sync-window deadline and the bounded retry/backoff.  The
 * attribute request's own transport timer runs on the real OSAL clock and
 * never fires within a test (its sweep period is 50 ms and the tests run
 * well below the configured multi-second window), so the module-level
 * deadline is the only timeout under test.
 *
 * Coverage per the TASK-112 definition of done:
 *   - valid complete synchronization (attribute-response and update paths),
 *   - no output is enabled before valid complete synchronization,
 *   - partial/invalid/out-of-range/oversized data rejected with fail-off
 *     and a bounded backoff retry,
 *   - sync timeout fails off and retries with bounded backoff,
 *   - duplicate data (identical updates, repeated responses) never applies
 *     twice,
 *   - stale/late callbacks (response for a consumed attempt, cancelled
 *     callback from a torn-down session) are dropped,
 *   - disconnect always forces the lamp output inactive; reconnect starts a
 *     fresh session and ThingsBoard stays authoritative after reconnect,
 *   - bounded retry budget exhaustion parks the module (output off) until
 *     the next reconnect resets the budget.
 *
 * Coverage per the TASK-113 definition of done (server-side RPC control):
 *   - valid setPower / setBrightness / setState and getState calls apply in
 *     order (hardware first, then success response, then telemetry) and
 *     return the resulting desired+applied state,
 *   - malformed requests (invalid JSON), missing required fields, wrong
 *     JSON types, out-of-range/fractional brightness, oversized method and
 *     payload bounds, and unknown methods are rejected with structured
 *     error JSON,
 *   - invalid RPC never modifies the applied state and never publishes
 *     telemetry,
 *   - a hardware failure (lamp apply error) produces a "hardware failure"
 *     error response — never a success response — and leaves the applied
 *     state unchanged,
 *   - getState reports the desired and applied state.
 *
 * Coverage per the TASK-114 definition of done (telemetry & health):
 *   - one full documented seven-field record is published on connect, on
 *     every successful state change (sync response, shared update, RPC set)
 *     and periodically while connected,
 *   - `pwm_duty` reports the APPLIED duty derived from the applied state
 *     (e.g. 55 % -> 5500/10000, off -> 0), not the requested brightness
 *     alone,
 *   - periodic telemetry is rate-limited to telemetry_period_ms: fast polls
 *     and polls before the boundary never publish, exactly one publish
 *     happens at the boundary and the next one needs another full period,
 *   - telemetry is fully suppressed while disconnected (no queue, no retry,
 *     reconnect publishes exactly the connect record),
 *   - secrets never leak: hostile config values (paths, JSON quotes,
 *     certificates, tokens) are sanitized to the safe charset, the payload
 *     carries exactly the seven documented fields and the JSON stays valid.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mqtt_app_mock.h"
#include "osal_task.h"
#include "tb_attributes.h"
#include "tb_client.h"
#include "unity.h"

#include "lamp_control_mock.h"
#include "tb_application.h"

/* --------------------------------------------------------------------- */
/* Harness                                                                */
/* --------------------------------------------------------------------- */

#define RESPONSE_TOPIC_PREFIX "v1/devices/me/attributes/request/"
#define ATTRIBUTES_TOPIC      "v1/devices/me/attributes"
#define RESPONSE_TOPIC_PREFIX_LEN (sizeof(RESPONSE_TOPIC_PREFIX) - 1u)

static tb_client_t *s_client;
static uint32_t s_now_ms;

/** @brief Injected clock: deterministic sync window / backoff control. */
static uint32_t test_now_ms(void)
{
    return s_now_ms;
}

/** @brief Product connection glue (exactly the documented integration). */
static void glue_connected(tb_client_t *client, void *ud)
{
    (void)ud;
    tb_application_on_connected(client);
}

static void glue_disconnected(tb_client_t *client,
                              tb_client_disconnect_reason_t reason, void *ud)
{
    (void)reason;
    (void)ud;
    tb_application_on_disconnected(client);
}

static void advance_ms(uint32_t delta_ms)
{
    s_now_ms += delta_ms;
}

/** @brief Create a ThingsBoard client with the synchronizer glue wired. */
static void create_client(void)
{
    tb_client_config_t cfg = {
        .server_url = "mqtt://localhost:1883",
        .access_token = "test_token",
        .device_name = "test_device",
        .on_connect = glue_connected,
        .on_disconnect = glue_disconnected,
    };

    TEST_ASSERT_EQUAL_INT(0, tb_client_init(&s_client, &cfg));
    TEST_ASSERT_NOT_NULL(s_client);
}

static tb_application_config_t make_app_config(uint32_t sync_timeout_ms,
                                               uint32_t max_retries)
{
    tb_application_config_t cfg = { 0 };

    cfg.client = s_client;
    cfg.sync_timeout_ms = sync_timeout_ms;
    cfg.retry_initial_delay_ms = 1000u;
    cfg.retry_max_delay_ms = 4000u;
    cfg.max_retries = max_retries;
    cfg.now_ms = test_now_ms;

    /* TASK-114 telemetry & health reporting configuration: a short, fixed
     * periodic interval for deterministic rate-limit tests and the
     * documented identity strings carried in every record. */
    cfg.telemetry_period_ms = 30000u;
    cfg.fw_version = "0.0.0-test";
    cfg.hardware = "esp32-wroom-32d";
    return cfg;
}

/** @brief Initialize the synchronizer with default retry settings. */
static void app_init(uint32_t sync_timeout_ms)
{
    tb_application_config_t cfg = make_app_config(sync_timeout_ms, 5u);

    s_now_ms = 100000u;
    TEST_ASSERT_EQUAL(TB_APPLICATION_OK, tb_application_init(&cfg));
}

/** @brief Initialize the synchronizer with a fully custom configuration. */
static void app_init_cfg(const tb_application_config_t *cfg)
{
    s_now_ms = 100000u;
    TEST_ASSERT_EQUAL(TB_APPLICATION_OK, tb_application_init(cfg));
}

/** @brief Connect the transport; on_connect restarts synchronization. */
static void connect_client(void)
{
    TEST_ASSERT_EQUAL_INT(0, tb_client_connect(s_client));
}

/** @brief Last published shared-attribute request id (0 when none). */
static uint32_t last_request_id(void)
{
    for (int i = mock_publish_count - 1; i >= 0; i--)
    {
        if (strncmp(mock_publishes[i].topic, RESPONSE_TOPIC_PREFIX,
                    RESPONSE_TOPIC_PREFIX_LEN) == 0)
        {
            const char *id = mock_publishes[i].topic +
                             RESPONSE_TOPIC_PREFIX_LEN;
            return (uint32_t)strtoul(id, NULL, 10);
        }
    }
    return 0u;
}

/** @brief Deliver an attribute response for the given request id. */
static void deliver_response(uint32_t request_id, const char *payload)
{
    char topic[128];

    snprintf(topic, sizeof(topic),
             "v1/devices/me/attributes/response/%u", (unsigned)request_id);
    mqtt_app_mock_deliver_message(topic, payload, strlen(payload));
}

/** @brief Deliver a shared-attribute update. */
static void deliver_update(const char *payload)
{
    mqtt_app_mock_deliver_message(ATTRIBUTES_TOPIC, payload,
                                  strlen(payload));
}

/** @brief Was a subscription to the shared-attribute topic recorded? */
static bool subscribed_to_shared_attributes(void)
{
    for (int i = 0; i < mock_subscribe_count; i++)
    {
        if (strcmp(mock_subscribes[i].topic, ATTRIBUTES_TOPIC) == 0)
        {
            return true;
        }
    }
    return false;
}

/** @brief Did the last shared-attribute request carry both keys? */
static bool last_request_has_both_keys(void)
{
    int idx = -1;
    cJSON *root = NULL;
    cJSON *shared_keys = NULL;
    const char *keys = NULL;
    bool ok = false;

    for (int i = mock_publish_count - 1; i >= 0; i--)
    {
        if (strncmp(mock_publishes[i].topic, RESPONSE_TOPIC_PREFIX,
                    RESPONSE_TOPIC_PREFIX_LEN) == 0)
        {
            idx = i;
            break;
        }
    }
    TEST_ASSERT_TRUE(idx >= 0);

    root = cJSON_Parse(mock_publishes[idx].message);
    TEST_ASSERT_NOT_NULL(root);
    if (root != NULL)
    {
        shared_keys = cJSON_GetObjectItemCaseSensitive(root, "sharedKeys");
        if (cJSON_IsString(shared_keys))
        {
            keys = shared_keys->valuestring;
            ok = (keys != NULL) && (strstr(keys, "power") != NULL) &&
                 (strstr(keys, "brightness") != NULL);
        }
        cJSON_Delete(root);
    }
    return ok;
}

/** @brief Number of shared-attribute request publishes so far. */
static int request_publish_count(void)
{
    int count = 0;

    for (int i = 0; i < mock_publish_count; i++)
    {
        if (strncmp(mock_publishes[i].topic, RESPONSE_TOPIC_PREFIX,
                    RESPONSE_TOPIC_PREFIX_LEN) == 0)
        {
            count++;
        }
    }
    return count;
}

/* --------------------------------------------------------------------- */
/* Server-side RPC helpers (TASK-113)                                     */
/* --------------------------------------------------------------------- */

#define RPC_REQUEST_TOPIC_PREFIX  "v1/devices/me/rpc/request/"
#define RPC_RESPONSE_TOPIC_PREFIX "v1/devices/me/rpc/response/"
#define TELEMETRY_TOPIC           "v1/devices/me/telemetry"

/**
 * @brief Telemetry publishes produced by rpc_sync_state() (TASK-114).
 *
 * The shared helper connects (connect trigger -> 1 publish) and applies a
 * complete valid state (successful change -> 1 publish), so exactly two
 * telemetry records exist after it returns.  The RPC-specific assertions
 * below use this constant for "no ADDITIONAL telemetry from the RPC".
 */
#define TELEMETRY_AFTER_SYNC 2

/** @brief Deliver a server RPC request for @p request_id. */
static void deliver_rpc(uint32_t request_id, const char *method,
                        const char *params)
{
    char topic[128];
    char payload[TB_APPLICATION_MAX_PAYLOAD_BYTES + 128];

    snprintf(topic, sizeof(topic), "%s%u", RPC_REQUEST_TOPIC_PREFIX,
             (unsigned)request_id);
    snprintf(payload, sizeof(payload), "{\"method\":\"%s\",\"params\":%s}",
             method, params);
    mqtt_app_mock_deliver_message(topic, payload, strlen(payload));
}

/** @brief Deliver a raw payload on the server-RPC request topic (for
 *         malformed overall JSON, which the platform tb_rpc layer drops). */
static void deliver_rpc_raw(uint32_t request_id, const char *payload)
{
    char topic[128];

    snprintf(topic, sizeof(topic), "%s%u", RPC_REQUEST_TOPIC_PREFIX,
             (unsigned)request_id);
    mqtt_app_mock_deliver_message(topic, payload, strlen(payload));
}

/** @brief Number of RPC response publishes to @p request_id (0 or 1). */
static int rpc_response_count(uint32_t request_id)
{
    char prefix[64];
    int count = 0;

    snprintf(prefix, sizeof(prefix), "%s%u", RPC_RESPONSE_TOPIC_PREFIX,
             (unsigned)request_id);
    for (int i = 0; i < mock_publish_count; i++)
    {
        if (strcmp(mock_publishes[i].topic, prefix) == 0)
        {
            count++;
        }
    }
    return count;
}

/**
 * @brief Return the RPC response message for @p request_id (latest), or
 *        NULL when none was published.
 */
static const char *rpc_response_message(uint32_t request_id)
{
    char prefix[64];

    snprintf(prefix, sizeof(prefix), "%s%u", RPC_RESPONSE_TOPIC_PREFIX,
             (unsigned)request_id);
    for (int i = mock_publish_count - 1; i >= 0; i--)
    {
        if (strcmp(mock_publishes[i].topic, prefix) == 0)
        {
            return mock_publishes[i].message;
        }
    }
    return NULL;
}

/**
 * @brief Parse the RPC response for @p request_id and assert one exists.
 *
 * The caller owns the returned cJSON object and must cJSON_Delete() it.
 */
static cJSON *rpc_parse_response(uint32_t request_id)
{
    const char *msg = rpc_response_message(request_id);

    TEST_ASSERT_NOT_NULL(msg);
    return cJSON_Parse(msg);
}

/** @brief Number of telemetry publishes recorded so far. */
static int telemetry_publish_count(void)
{
    int count = 0;

    for (int i = 0; i < mock_publish_count; i++)
    {
        if (strcmp(mock_publishes[i].topic, TELEMETRY_TOPIC) == 0)
        {
            count++;
        }
    }
    return count;
}

/** @brief Latest telemetry JSON payload, or NULL when none was published. */
static const char *latest_telemetry_message(void)
{
    for (int i = mock_publish_count - 1; i >= 0; i--)
    {
        if (strcmp(mock_publishes[i].topic, TELEMETRY_TOPIC) == 0)
        {
            return mock_publishes[i].message;
        }
    }
    return NULL;
}

/**
 * @brief Assert the RPC response to @p request_id is a generic structured
 *        success and return a parsed copy (caller cJSON_Delete()s it).
 */
static cJSON *rpc_assert_success(uint32_t request_id)
{
    cJSON *response = rpc_parse_response(request_id);

    TEST_ASSERT_NOT_NULL(response);
    TEST_ASSERT_TRUE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(response, "success")));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(response, "error"));
    return response;
}

/**
 * @brief Assert the RPC response to @p request_id is a structured error
 *        with the documented @p error class and optional @p reason.
 */
static void rpc_assert_error(uint32_t request_id, const char *error,
                             const char *reason)
{
    cJSON *response = rpc_parse_response(request_id);
    cJSON *error_item;
    cJSON *reason_item;

    TEST_ASSERT_NOT_NULL(response);
    TEST_ASSERT_TRUE(cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(response, "success")));
    error_item = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_TRUE(cJSON_IsString(error_item));
    TEST_ASSERT_EQUAL_STRING(error, error_item->valuestring);
    reason_item = cJSON_GetObjectItemCaseSensitive(response, "reason");
    if (reason == NULL)
    {
        TEST_ASSERT_NULL(reason_item);
    }
    else
    {
        TEST_ASSERT_TRUE(cJSON_IsString(reason_item));
        TEST_ASSERT_EQUAL_STRING(reason, reason_item->valuestring);
    }
    cJSON_Delete(response);
}

/**
 * @brief Synchronize a complete valid state and enter the SYNCED state.
 *
 * Shared helper for the RPC tests: after this the module has applied
 * @p power/@p brightness exactly once.
 */
static void rpc_sync_state(bool power, uint8_t brightness)
{
    char payload[64];

    app_init(10000u);
    connect_client();
    snprintf(payload, sizeof(payload),
             "{\"shared\":{\"power\":%s,\"brightness\":%u}}",
             power ? "true" : "false", (unsigned)brightness);
    deliver_response(last_request_id(), payload);
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
}

void setUp(void)
{
    lamp_mock_reset();
    mqtt_app_mock_reset();
    s_now_ms = 100000u;
    s_client = NULL;

    create_client();
}

void tearDown(void)
{
    tb_application_deinit();
    if (s_client != NULL)
    {
        tb_client_deinit(s_client);
        s_client = NULL;
    }
}

/* --------------------------------------------------------------------- */
/* Valid complete synchronization                                         */
/* --------------------------------------------------------------------- */

/**
 * Valid path: connect -> one subscribe + one shared-attribute request for
 * both keys -> complete valid response applied; nothing is enabled before
 * the response.
 */
static void test_valid_sync_via_attribute_response(void)
{
    uint32_t request_id;
    bool has_state = false;
    tb_application_desired_state_t state;

    app_init(10000u);
    connect_client();

    /* One shared-attribute request carrying both keys, and a subscription. */
    TEST_ASSERT_TRUE(subscribed_to_shared_attributes());
    TEST_ASSERT_EQUAL_INT(1, request_publish_count());
    TEST_ASSERT_TRUE(last_request_has_both_keys());
    request_id = last_request_id();
    TEST_ASSERT_TRUE(request_id != 0u);

    /* No output is enabled before valid complete synchronization. */
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());
    TEST_ASSERT_FALSE(lamp_mock_state_applied());

    deliver_response(request_id, "{\"shared\":{\"power\":true,\"brightness\":80}}");

    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_state_applied());
    TEST_ASSERT_TRUE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(80u, lamp_mock_applied_brightness());

    TEST_ASSERT_EQUAL(TB_APPLICATION_OK,
                      tb_application_get_desired_state(s_client, &has_state,
                                                       &state));
    TEST_ASSERT_TRUE(has_state);
    TEST_ASSERT_TRUE(state.power);
    TEST_ASSERT_EQUAL_UINT(80u, state.brightness_percent);
}

/**
 * Valid path via a shared-attribute update: a complete valid update applies
 * immediately and the (older) attribute response that arrives afterwards is
 * a late response for the resolved attempt and is dropped.
 */
static void test_valid_sync_via_shared_update(void)
{
    uint32_t request_id;

    app_init(10000u);
    connect_client();
    request_id = last_request_id();
    TEST_ASSERT_TRUE(request_id != 0u);

    deliver_update("{\"power\":true,\"brightness\":60}");

    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(60u, lamp_mock_applied_brightness());

    /* The in-flight attribute response for the same attempt is now late and
     * must not overwrite or re-apply anything. */
    deliver_response(request_id, "{\"shared\":{\"power\":true,\"brightness\":60}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
}

/* --------------------------------------------------------------------- */
/* Rejections: partial / invalid data                                     */
/* --------------------------------------------------------------------- */

/**
 * Partial data (missing required field) is rejected with fail-off; the
 * module enters bounded backoff and a later complete state on the retry
 * succeeds.
 */
static void test_partial_data_rejected_then_retry(void)
{
    uint32_t request_id;

    app_init(10000u);
    connect_client();
    request_id = last_request_id();

    /* Missing brightness: incomplete desired state. */
    deliver_response(request_id, "{\"shared\":{\"power\":true}}");

    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() >= 1u);

    /* Bounded backoff; after the delay the module retries (fresh request). */
    advance_ms(2000u);
    tb_application_poll(s_client);
    TEST_ASSERT_EQUAL_INT(2, request_publish_count());
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());

    request_id = last_request_id();
    deliver_response(request_id, "{\"shared\":{\"power\":true,\"brightness\":50}}");

    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_EQUAL_UINT(50u, lamp_mock_applied_brightness());
}

/** Wrong JSON type for the required power field is rejected with fail-off. */
static void test_invalid_power_type_rejected(void)
{
    uint32_t request_id;

    app_init(10000u);
    connect_client();
    request_id = last_request_id();

    deliver_response(request_id,
                     "{\"shared\":{\"power\":\"on\",\"brightness\":50}}");

    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() >= 1u);
}

/** Wrong JSON type for the required brightness field is rejected. */
static void test_invalid_brightness_type_rejected(void)
{
    uint32_t request_id;

    app_init(10000u);
    connect_client();
    request_id = last_request_id();

    deliver_response(request_id,
                     "{\"shared\":{\"power\":true,\"brightness\":\"high\"}}");

    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() >= 1u);
}

/** Out-of-range brightness (above 100 / below 0) is rejected, never clamped. */
static void test_brightness_out_of_range_rejected(void)
{
    app_init(10000u);
    connect_client();
    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":101}}");
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());

    /* Reconnect for the below-zero case. */
    mqtt_app_mock_simulate_remote_disconnect();
    mqtt_app_mock_simulate_connect();
    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":-1}}");
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() >= 1u);
}

/** Fractional brightness is rejected (only integer percentages are valid). */
static void test_fractional_brightness_rejected(void)
{
    uint32_t request_id;

    app_init(10000u);
    connect_client();
    request_id = last_request_id();

    deliver_response(request_id,
                     "{\"shared\":{\"power\":true,\"brightness\":50.5}}");

    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() >= 1u);
}

/** Payloads above the bounded size are rejected before parsing. */
static void test_oversized_payload_rejected(void)
{
    char oversized[TB_APPLICATION_MAX_PAYLOAD_BYTES + 64];
    uint32_t request_id;

    app_init(10000u);
    connect_client();
    request_id = last_request_id();

    memset(oversized, 'x', sizeof(oversized) - 1u);
    oversized[sizeof(oversized) - 1u] = '\0';
    /* Sanity: the stub must strictly exceed the bound. */
    TEST_ASSERT_TRUE(strlen(oversized) > TB_APPLICATION_MAX_PAYLOAD_BYTES);

    deliver_response(request_id, oversized);

    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() >= 1u);

    /* The module stays functional: a valid state on the retry applies. */
    advance_ms(2000u);
    tb_application_poll(s_client);
    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":false,\"brightness\":10}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_FALSE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(10u, lamp_mock_applied_brightness());
}

/* --------------------------------------------------------------------- */
/* Timeout                                                                */
/* --------------------------------------------------------------------- */

/**
 * Sync window expiry (no complete valid state) fails off and enters bounded
 * backoff; the retry then completes with a valid response.
 */
static void test_sync_timeout_fails_off_then_retry(void)
{
    uint32_t request_id;
    unsigned force_off_before;

    app_init(10000u);
    connect_client();
    request_id = last_request_id();
    TEST_ASSERT_TRUE(request_id != 0u);

    /* Nothing arrived; the sync window (100000 + 10000) expires. */
    advance_ms(20000u);
    tb_application_poll(s_client);

    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() >= 1u);

    /* No retry before the bounded backoff elapses. */
    force_off_before = lamp_mock_force_inactive_calls();
    tb_application_poll(s_client);
    TEST_ASSERT_EQUAL_INT(1, request_publish_count());
    TEST_ASSERT_EQUAL_UINT(force_off_before, lamp_mock_force_inactive_calls());

    /* After the backoff delay the module retries and a valid state applies. */
    advance_ms(2000u);
    tb_application_poll(s_client);
    TEST_ASSERT_EQUAL_INT(2, request_publish_count());
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));

    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":30}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_EQUAL_UINT(30u, lamp_mock_applied_brightness());
}

/* --------------------------------------------------------------------- */
/* Duplicate data                                                         */
/* --------------------------------------------------------------------- */

/** Identical retransmitted updates never re-apply or re-enable the output. */
static void test_duplicate_update_ignored(void)
{
    app_init(10000u);
    connect_client();
    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":80}}");

    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());

    /* Identical update: duplicate retransmission -> no-op. */
    deliver_update("{\"power\":true,\"brightness\":80}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());

    /* A genuinely different complete update is authoritative. */
    deliver_update("{\"power\":false,\"brightness\":20}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(2u, lamp_mock_apply_calls());
    TEST_ASSERT_FALSE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(20u, lamp_mock_applied_brightness());
}

/** A repeated response for a request id that is already consumed never
 * reaches the module callback again (single apply). */
static void test_duplicate_response_dropped(void)
{
    uint32_t request_id;

    app_init(10000u);
    connect_client();
    request_id = last_request_id();

    deliver_response(request_id,
                     "{\"shared\":{\"power\":true,\"brightness\":70}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());

    /* The request slot was consumed; a second identical response is dropped
     * before it can reach the module and can never apply twice. */
    deliver_response(request_id,
                     "{\"shared\":{\"power\":true,\"brightness\":70}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
}

/* --------------------------------------------------------------------- */
/* Stale / late callbacks                                                 */
/* --------------------------------------------------------------------- */

/**
 * A response for an already-consumed (timed-out) attempt is stale/late and
 * is dropped: the state machine is in backoff and no output is enabled.
 */
static void test_late_response_after_timeout_rejected(void)
{
    uint32_t old_request_id;

    app_init(10000u);
    connect_client();
    old_request_id = last_request_id();

    advance_ms(20000u);
    tb_application_poll(s_client);
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));

    /* The transport still pends the old request; a late "complete valid"
     * response for it must be rejected (attempt already consumed). */
    deliver_response(old_request_id,
                     "{\"shared\":{\"power\":true,\"brightness\":99}}");

    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());

    /* The retry still completes normally afterwards. */
    advance_ms(2000u);
    tb_application_poll(s_client);
    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":false,\"brightness\":5}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_EQUAL_UINT(5u, lamp_mock_applied_brightness());
}

/**
 * The disconnect path cancels the in-flight request (stale callback of the
 * old session); the module drops it, fails off and a fresh session then
 * re-synchronizes on reconnect.
 */
static void test_stale_cancelled_dropped_and_reconnect(void)
{
    uint32_t old_request_id;

    app_init(10000u);
    connect_client();
    old_request_id = last_request_id();

    /* Remote disconnect: tb_client attributes handler fires CANCELLED with
     * the old session token before the product disconnect glue runs. */
    mqtt_app_mock_simulate_remote_disconnect();

    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() >= 1u);

    /* Any late response for the old session is dropped. */
    deliver_response(old_request_id,
                     "{\"shared\":{\"power\":true,\"brightness\":99}}");
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());

    /* Reconnect restarts synchronization (fresh session, fresh request). */
    mqtt_app_mock_simulate_connect();
    TEST_ASSERT_TRUE(request_publish_count() >= 2);
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));

    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":45}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_EQUAL_UINT(45u, lamp_mock_applied_brightness());
}

/* --------------------------------------------------------------------- */
/* Reconnect authority and fail-off                                        */
/* --------------------------------------------------------------------- */

/**
 * Disconnect always forces the lamp output inactive (regardless of sync
 * progress); reconnect starts a fresh session and no output is re-enabled
 * until the freshly connected broker supplies a complete valid state;
 * ThingsBoard stays authoritative after reconnect.
 */
static void test_reconnect_authoritative_and_fail_off(void)
{
    bool has_state = false;
    tb_application_desired_state_t state;

    app_init(10000u);
    connect_client();
    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":70}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());

    /* Transport loss: output forced off and the session invalidated. */
    unsigned force_off_calls = lamp_mock_force_inactive_calls();
    mqtt_app_mock_simulate_remote_disconnect();
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > force_off_calls);

    /* Reconnect: fresh session; until a complete valid state arrives the
     * module reports not synchronized and applies nothing. */
    mqtt_app_mock_simulate_connect();
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());

    /* ThingsBoard (the reconnected broker) is authoritative: the new state
     * replaces the old one exactly as delivered. */
    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":false,\"brightness\":15}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(2u, lamp_mock_apply_calls());
    TEST_ASSERT_FALSE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(15u, lamp_mock_applied_brightness());

    TEST_ASSERT_EQUAL(TB_APPLICATION_OK,
                      tb_application_get_desired_state(s_client, &has_state,
                                                       &state));
    TEST_ASSERT_FALSE(state.power);
    TEST_ASSERT_EQUAL_UINT(15u, state.brightness_percent);
}

/** Invalid update during synchronization also fails off and never enables the
 * output; the bounded backoff retry then succeeds with a complete state. */
static void test_invalid_update_fails_off(void)
{
    app_init(10000u);
    connect_client();

    deliver_update("{\"power\":true,\"brightness\":\"bad\"}");

    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() >= 1u);

    /* Retry after the bounded backoff applies only the complete valid state. */
    advance_ms(2000u);
    tb_application_poll(s_client);
    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":25}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_EQUAL_UINT(25u, lamp_mock_applied_brightness());
}

/* --------------------------------------------------------------------- */
/* Bounded retry budget                                                   */
/* --------------------------------------------------------------------- */

/**
 * When the bounded retry budget for a session is exhausted the module parks
 * in the safe inactive state (output off, no further attempts) until the
 * next reconnect gives it a fresh budget.
 */
static void test_retry_budget_exhaustion_then_reconnect(void)
{
    tb_application_config_t cfg = make_app_config(10000u, 2u);
    int requests_before;

    app_init_cfg(&cfg);
    connect_client();

    /* Fail attempt 1 with an invalid response. */
    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":101}}");
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_INT(1, request_publish_count());

    /* Fail attempt 2 with an invalid response. */
    advance_ms(2000u);
    tb_application_poll(s_client);
    TEST_ASSERT_EQUAL_INT(2, request_publish_count());
    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":101}}");
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() >= 1u);

    /* Budget exhausted: no further attempts even after a long wait. */
    requests_before = request_publish_count();
    advance_ms(100000u);
    tb_application_poll(s_client);
    tb_application_poll(s_client);
    TEST_ASSERT_EQUAL_INT(requests_before, request_publish_count());
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_apply_calls());

    /* Reconnect restarts the session with a fresh retry budget. */
    mqtt_app_mock_simulate_remote_disconnect();
    mqtt_app_mock_simulate_connect();
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));

    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":40}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_EQUAL_UINT(40u, lamp_mock_applied_brightness());
}

/* --------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* --------------------------------------------------------------------- */

/** init/deinit argument validation, idempotence and state queries. */
static void test_lifecycle_and_state_queries(void)
{
    tb_application_config_t cfg = make_app_config(10000u, 5u);
    bool has_state = false;
    tb_application_desired_state_t state;

    TEST_ASSERT_EQUAL(TB_APPLICATION_ERR_INVALID_ARGUMENT,
                      tb_application_init(NULL));

    TEST_ASSERT_EQUAL(TB_APPLICATION_OK, tb_application_init(&cfg));
    TEST_ASSERT_EQUAL(TB_APPLICATION_ERR_ALREADY_INITIALIZED,
                      tb_application_init(&cfg));

    /* Bad query arguments. */
    TEST_ASSERT_EQUAL(TB_APPLICATION_ERR_INVALID_ARGUMENT,
                      tb_application_get_desired_state(s_client, NULL, NULL));
    TEST_ASSERT_EQUAL(TB_APPLICATION_ERR_CLIENT_MISMATCH,
                      tb_application_get_desired_state(
                          (tb_client_t *)(uintptr_t)0x1, &has_state, &state));

    /* Not synchronized before any connection; has-state is still false. */
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL(TB_APPLICATION_OK,
                      tb_application_get_desired_state(s_client, &has_state,
                                                       &state));
    TEST_ASSERT_FALSE(has_state);

    tb_application_deinit();
    TEST_ASSERT_EQUAL(TB_APPLICATION_ERR_NOT_INITIALIZED,
                      tb_application_get_desired_state(s_client, &has_state,
                                                       &state));
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    tb_application_deinit(); /* idempotent */
}

/* --------------------------------------------------------------------- */
/* Server-side RPC control (TASK-113)                                     */
/* --------------------------------------------------------------------- */

/**
 * Valid setPower: hardware is applied BEFORE the success response; the
 * success response carries the resulting desired+applied state and telemetry
 * is published only after the successful change.  Reading state and errors
 * never publish telemetry.
 */
static void test_rpc_set_power_valid(void)
{
    cJSON *response;
    cJSON *desired;
    cJSON *applied;
    cJSON *telemetry;
    int telemetry_before;

    rpc_sync_state(true, 80u);
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(80u, lamp_mock_applied_brightness());

    telemetry_before = telemetry_publish_count();
    deliver_rpc(1u, "setPower", "{\"power\":false}");

    /* Hardware applied first: apply count grew and the reported hardware
     * state already reflects the change. */
    TEST_ASSERT_EQUAL_UINT(2u, lamp_mock_apply_calls());
    TEST_ASSERT_FALSE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(80u, lamp_mock_applied_brightness());
    TEST_ASSERT_EQUAL_INT(1, rpc_response_count(1u));
    TEST_ASSERT_EQUAL_INT(telemetry_before + 1, telemetry_publish_count());

    response = rpc_assert_success(1u);
    desired = cJSON_GetObjectItemCaseSensitive(response, "desired");
    TEST_ASSERT_TRUE(cJSON_IsObject(desired));
    TEST_ASSERT_FALSE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(desired, "power")));
    TEST_ASSERT_EQUAL_INT(80, cJSON_GetObjectItemCaseSensitive(
                                  desired, "brightness")->valueint);
    applied = cJSON_GetObjectItemCaseSensitive(response, "applied");
    TEST_ASSERT_TRUE(cJSON_IsObject(applied));
    TEST_ASSERT_FALSE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(applied, "power")));
    TEST_ASSERT_EQUAL_INT(80, cJSON_GetObjectItemCaseSensitive(
                                  applied, "brightness")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(applied, "output_active")));
    cJSON_Delete(response);

    /* The telemetry publish reflects exactly the successful change. */
    TEST_ASSERT_NOT_NULL(latest_telemetry_message());
    telemetry = cJSON_Parse(latest_telemetry_message());
    TEST_ASSERT_NOT_NULL(telemetry);
    TEST_ASSERT_FALSE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(telemetry, "power")));
    TEST_ASSERT_EQUAL_INT(80, cJSON_GetObjectItemCaseSensitive(
                                  telemetry, "brightness")->valueint);
    cJSON_Delete(telemetry);
}

/**
 * Valid setBrightness: a single-field change keeps the current power and
 * applies the new brightness; success response + telemetry follow the change.
 */
static void test_rpc_set_brightness_valid(void)
{
    cJSON *response;
    cJSON *desired;
    cJSON *applied;

    rpc_sync_state(true, 20u);

    deliver_rpc(2u, "setBrightness", "{\"brightness\":42}");

    TEST_ASSERT_EQUAL_UINT(2u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(42u, lamp_mock_applied_brightness());
    TEST_ASSERT_EQUAL_INT(1, rpc_response_count(2u));
    TEST_ASSERT_EQUAL_INT(TELEMETRY_AFTER_SYNC + 1, telemetry_publish_count());

    response = rpc_assert_success(2u);
    desired = cJSON_GetObjectItemCaseSensitive(response, "desired");
    TEST_ASSERT_TRUE(cJSON_IsObject(desired));
    TEST_ASSERT_TRUE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(desired, "power")));
    TEST_ASSERT_EQUAL_INT(42, cJSON_GetObjectItemCaseSensitive(
                                  desired, "brightness")->valueint);
    applied = cJSON_GetObjectItemCaseSensitive(response, "applied");
    TEST_ASSERT_TRUE(cJSON_IsObject(applied));
    TEST_ASSERT_TRUE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(applied, "power")));
    TEST_ASSERT_EQUAL_INT(42, cJSON_GetObjectItemCaseSensitive(
                                  applied, "brightness")->valueint);
    cJSON_Delete(response);
}

/**
 * Valid setState: both fields are required and applied together; success
 * response + telemetry follow the successful change.
 */
static void test_rpc_set_state_valid(void)
{
    cJSON *response;
    cJSON *desired;
    cJSON *applied;

    rpc_sync_state(true, 60u);

    /* power=false + brightness=0 is a complete, valid, applied state. */
    deliver_rpc(3u, "setState", "{\"power\":false,\"brightness\":0}");

    TEST_ASSERT_EQUAL_UINT(2u, lamp_mock_apply_calls());
    TEST_ASSERT_FALSE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_applied_brightness());
    TEST_ASSERT_EQUAL_INT(1, rpc_response_count(3u));
    TEST_ASSERT_EQUAL_INT(TELEMETRY_AFTER_SYNC + 1, telemetry_publish_count());

    response = rpc_assert_success(3u);
    desired = cJSON_GetObjectItemCaseSensitive(response, "desired");
    TEST_ASSERT_FALSE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(desired, "power")));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItemCaseSensitive(
                                  desired, "brightness")->valueint);
    applied = cJSON_GetObjectItemCaseSensitive(response, "applied");
    TEST_ASSERT_FALSE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(applied, "power")));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItemCaseSensitive(
                                  applied, "brightness")->valueint);
    cJSON_Delete(response);
}

/**
 * Valid getState: returns the desired+applied state; reading never applies
 * the hardware and never publishes telemetry.
 */
static void test_rpc_get_state_valid(void)
{
    cJSON *response;
    cJSON *desired;
    cJSON *applied;

    rpc_sync_state(true, 55u);
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());

    deliver_rpc(4u, "getState", "{}");

    TEST_ASSERT_EQUAL_INT(1, rpc_response_count(4u));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_EQUAL_INT(TELEMETRY_AFTER_SYNC, telemetry_publish_count());

    response = rpc_assert_success(4u);
    desired = cJSON_GetObjectItemCaseSensitive(response, "desired");
    TEST_ASSERT_TRUE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(desired, "power")));
    TEST_ASSERT_EQUAL_INT(55, cJSON_GetObjectItemCaseSensitive(
                                  desired, "brightness")->valueint);
    applied = cJSON_GetObjectItemCaseSensitive(response, "applied");
    TEST_ASSERT_TRUE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(applied, "power")));
    TEST_ASSERT_EQUAL_INT(55, cJSON_GetObjectItemCaseSensitive(
                                  applied, "brightness")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(applied, "output_active")));
    cJSON_Delete(response);
}

/**
 * A fully malformed request payload never reaches the module: the platform
 * tb_rpc layer drops it before parsing, so no response is published and an
 * invalid RPC can never modify the applied state.
 */
static void test_rpc_malformed_payload_dropped(void)
{
    rpc_sync_state(true, 80u);
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());

    deliver_rpc_raw(5u, "{not valid json");

    TEST_ASSERT_EQUAL_INT(0, rpc_response_count(5u));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(80u, lamp_mock_applied_brightness());
    TEST_ASSERT_EQUAL_INT(TELEMETRY_AFTER_SYNC, telemetry_publish_count());
}

/**
 * Missing required fields are rejected with structured invalid-payload errors;
 * no apply, no telemetry and the applied state stays untouched.
 */
static void test_rpc_missing_field_rejected(void)
{
    rpc_sync_state(true, 77u);
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());

    deliver_rpc(6u, "setPower", "{}");
    rpc_assert_error(6u, "invalid payload", "missing power");
    TEST_ASSERT_EQUAL_INT(1, rpc_response_count(6u));

    deliver_rpc(7u, "setBrightness", "{}");
    rpc_assert_error(7u, "invalid payload", "missing brightness");

    /* setState requires BOTH fields; a partial object is invalid. */
    deliver_rpc(8u, "setState", "{\"power\":true}");
    rpc_assert_error(8u, "invalid payload", "missing brightness");

    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(77u, lamp_mock_applied_brightness());
    TEST_ASSERT_EQUAL_INT(TELEMETRY_AFTER_SYNC, telemetry_publish_count());
}

/**
 * Wrong JSON types (power must be a boolean, brightness must be a
 * number, params must be an object) are rejected with structured
 * invalid-payload errors; no apply, no telemetry.

 * (power=1 parses as a cJSON number, never a boolean.)
 */
static void test_rpc_wrong_type_rejected(void)
{
    rpc_sync_state(true, 80u);

    deliver_rpc(9u, "setPower", "{\"power\":\"true\"}");
    rpc_assert_error(9u, "invalid payload", "wrong type");

    deliver_rpc(10u, "setBrightness", "{\"brightness\":\"50\"}");
    rpc_assert_error(10u, "invalid payload", "wrong type");

    deliver_rpc(11u, "setState", "{\"power\":1,\"brightness\":50}");
    rpc_assert_error(11u, "invalid payload", "wrong type");

    /* The params payload must be a JSON object. */
    deliver_rpc(12u, "setPower", "\"power\"");
    rpc_assert_error(12u, "invalid payload", "wrong type");

    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(80u, lamp_mock_applied_brightness());
    TEST_ASSERT_EQUAL_INT(TELEMETRY_AFTER_SYNC, telemetry_publish_count());
}

/**
 * Brightness outside the closed interval 0..100 (and fractional values,
 * which are never truncated to an integer) are rejected with structured
 * invalid-payload errors; no apply, no telemetry.

 * A rejected RPC never modifies the applied state.
 */
static void test_rpc_brightness_out_of_range_rejected(void)
{
    rpc_sync_state(true, 80u);

    deliver_rpc(13u, "setBrightness", "{\"brightness\":101}");
    rpc_assert_error(13u, "invalid payload", "out of range");

    deliver_rpc(14u, "setBrightness", "{\"brightness\":-1}");
    rpc_assert_error(14u, "invalid payload", "out of range");

    /* Fractional brightness: never wrapped or truncated. */
    deliver_rpc(15u, "setBrightness", "{\"brightness\":50.5}");
    rpc_assert_error(15u, "invalid payload", "out of range");

    deliver_rpc(16u, "setState", "{\"power\":true,\"brightness\":200}");
    rpc_assert_error(16u, "invalid payload", "out of range");

    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(80u, lamp_mock_applied_brightness());
    TEST_ASSERT_EQUAL_INT(TELEMETRY_AFTER_SYNC, telemetry_publish_count());
}

/**
 * An undocumented method name is rejected as unknown method with the
 * bounded method name echoed in the structured error response; no apply,
 * no telemetry.

 */
static void test_rpc_unknown_method_rejected(void)
{
    cJSON *response;
    cJSON *method;

    rpc_sync_state(true, 80u);

    deliver_rpc(17u, "setColor", "{\"x\":1}");

    TEST_ASSERT_EQUAL_INT(1, rpc_response_count(17u));
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_EQUAL_INT(TELEMETRY_AFTER_SYNC, telemetry_publish_count());

    response = rpc_parse_response(17u);
    TEST_ASSERT_NOT_NULL(response);
    TEST_ASSERT_TRUE(cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(response, "success")));
    TEST_ASSERT_TRUE(cJSON_IsString(
        cJSON_GetObjectItemCaseSensitive(response, "error")));
    TEST_ASSERT_EQUAL_STRING("unknown method",
                             cJSON_GetObjectItemCaseSensitive(response, "error")->valuestring);
    method = cJSON_GetObjectItemCaseSensitive(response, "method");
    TEST_ASSERT_TRUE(cJSON_IsString(method));
    TEST_ASSERT_EQUAL_STRING("setColor", method->valuestring);
    cJSON_Delete(response);
}

/**
 * Method name and params payload are length-bounded BEFORE any parsing or
 * comparison: an oversized method or payload is rejected as invalid
 * payload and never touches the hardware or telemetry.

 */
static void test_rpc_method_and_payload_bounds_rejected(void)
{
    char long_method[TB_APPLICATION_RPC_METHOD_MAX_LEN + 2u];
    char padded[TB_APPLICATION_RPC_PARAMS_MAX_LEN + 64u];
    char big_params[sizeof(padded) + 64u];

    rpc_sync_state(true, 80u);

    /* Oversized method name: rejected before comparison. */
    memset(long_method, 'm', sizeof(long_method) - 1u);
    long_method[sizeof(long_method) - 1u] = '\0';
    TEST_ASSERT_TRUE(strlen(long_method) > TB_APPLICATION_RPC_METHOD_MAX_LEN);
    deliver_rpc(18u, long_method, "{}");
    rpc_assert_error(18u, "invalid payload", "method too long");

    /* Oversized params payload: rejected before parsing. */
    memset(padded, 'p', sizeof(padded) - 1u);
    padded[sizeof(padded) - 1u] = '\0';
    snprintf(big_params, sizeof(big_params), "{\"pad\":\"%s\"}", padded);
    TEST_ASSERT_TRUE(strlen(big_params) > TB_APPLICATION_RPC_PARAMS_MAX_LEN);
    deliver_rpc(19u, "setState", big_params);
    rpc_assert_error(19u, "invalid payload", "payload too long");

    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());
    TEST_ASSERT_TRUE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(80u, lamp_mock_applied_brightness());
    TEST_ASSERT_EQUAL_INT(TELEMETRY_AFTER_SYNC, telemetry_publish_count());
}

/**
 * A hardware failure (lamp apply error) never produces a success
 * response: the RPC returns a structured "hardware failure" error, publishes
 * no telemetry and leaves both the hardware-applied state and the module's
 * desired state unchanged.  The success response path is only reachable
 * AFTER lamp_control_apply_state() returned #LAMP_OK.

 */
static void test_rpc_hardware_failure_rejected(void)
{
    bool has_state = false;
    tb_application_desired_state_t module_state;

    rpc_sync_state(true, 80u);
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_apply_calls());

    /* Inject a lamp apply failure: never a success response. */
    lamp_mock_set_apply_result(LAMP_ERR_INTERNAL);
    deliver_rpc(20u, "setPower", "{\"power\":false}");
    TEST_ASSERT_EQUAL_INT(1, rpc_response_count(20u));
    TEST_ASSERT_EQUAL_UINT(2u, lamp_mock_apply_calls());
    TEST_ASSERT_EQUAL_INT(TELEMETRY_AFTER_SYNC, telemetry_publish_count());
    rpc_assert_error(20u, "hardware failure", NULL);

    /* Hardware applied state unchanged (sync state preserved). */
    TEST_ASSERT_TRUE(lamp_mock_applied_power());
    TEST_ASSERT_EQUAL_UINT(80u, lamp_mock_applied_brightness());
    TEST_ASSERT_EQUAL(TB_APPLICATION_OK,
                      tb_application_get_desired_state(s_client, &has_state,
                                                       &module_state));
    TEST_ASSERT_TRUE(has_state);
    TEST_ASSERT_TRUE(module_state.power);
    TEST_ASSERT_EQUAL_UINT(80u, module_state.brightness_percent);

    /* A second method class fails the same way. */
    deliver_rpc(21u, "setBrightness", "{\"brightness\":0}");
    TEST_ASSERT_EQUAL_INT(1, rpc_response_count(21u));
    TEST_ASSERT_EQUAL_INT(TELEMETRY_AFTER_SYNC, telemetry_publish_count());
    rpc_assert_error(21u, "hardware failure", NULL);
    TEST_ASSERT_EQUAL_UINT(80u, lamp_mock_applied_brightness());
}

/**
 * Requests arriving while the transport is disconnected are dropped:
 * nothing is applied or reported and the applied state stays untouched. (The
 * return structure only applies to live transport sessions.)
 */
static void test_rpc_disconnected_dropped(void)
{
    unsigned apply_before;

    rpc_sync_state(true, 80u);
    mqtt_app_mock_simulate_remote_disconnect();
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() >= 1u);

    apply_before = lamp_mock_apply_calls();
    deliver_rpc(22u, "setPower", "{\"power\":false}");
    TEST_ASSERT_EQUAL_INT(0, rpc_response_count(22u));
    TEST_ASSERT_EQUAL_UINT(apply_before, lamp_mock_apply_calls());
    TEST_ASSERT_EQUAL_INT(TELEMETRY_AFTER_SYNC, telemetry_publish_count());
}

/* --------------------------------------------------------------------- */
/* Telemetry and health reporting (TASK-114)                               */
/* --------------------------------------------------------------------- */

/**
 * @brief Assert a telemetry record carries the documented fields with their
 *        documented JSON types and values.
 */
static void telemetry_assert_shape(const cJSON *telemetry)
{
    cJSON *power_item =
        cJSON_GetObjectItemCaseSensitive(telemetry, "power");
    cJSON *brightness_item =
        cJSON_GetObjectItemCaseSensitive(telemetry, "brightness");
    cJSON *duty_item =
        cJSON_GetObjectItemCaseSensitive(telemetry, "pwm_duty");
    cJSON *state_item =
        cJSON_GetObjectItemCaseSensitive(telemetry, "connection_state");
    cJSON *fw_item =
        cJSON_GetObjectItemCaseSensitive(telemetry, "fw_version");
    cJSON *hw_item =
        cJSON_GetObjectItemCaseSensitive(telemetry, "hardware");
    cJSON *uptime_item =
        cJSON_GetObjectItemCaseSensitive(telemetry, "uptime_ms");

    TEST_ASSERT_TRUE(cJSON_IsBool(power_item));
    TEST_ASSERT_TRUE(cJSON_IsNumber(brightness_item));
    TEST_ASSERT_TRUE(cJSON_IsNumber(duty_item));
    TEST_ASSERT_TRUE(cJSON_IsString(state_item));
    TEST_ASSERT_EQUAL_STRING(TB_APPLICATION_CONNECTION_STATE_ONLINE,
                             state_item->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsString(fw_item));
    TEST_ASSERT_TRUE(cJSON_IsString(hw_item));
    TEST_ASSERT_TRUE(cJSON_IsNumber(uptime_item));
    TEST_ASSERT_TRUE(uptime_item->valueint >= 0);
}

/**
 * @brief Assert a telemetry record contains EXACTLY the seven documented
 *        fields — no extra members, so no secret/credential/diagnostic dump
 *        can ever be smuggled into the payload.
 */
static void telemetry_assert_only_documented_fields(const cJSON *telemetry)
{
    static const char *keys[] = {
        "power", "brightness", "pwm_duty", "connection_state",
        "fw_version", "hardware", "uptime_ms",
    };
    const cJSON *child;
    int count = 0;

    TEST_ASSERT_TRUE(cJSON_IsObject(telemetry));
    for (child = telemetry->child; child != NULL; child = child->next)
    {
        count++;
    }
    TEST_ASSERT_EQUAL_INT(7, count);
    for (int i = 0; i < 7; i++)
    {
        TEST_ASSERT_NOT_NULL(
            cJSON_GetObjectItemCaseSensitive(telemetry, keys[i]));
    }
}

/**
 * Connect trigger: a fresh connection publishes exactly one health record
 * carrying the full documented shape.  Nothing was applied yet, so the
 * record reports the safe electrical-off state and the configured
 * firmware/hardware identity.
 */
static void test_telemetry_published_on_connect(void)
{
    cJSON *telemetry;
    cJSON *uptime;

    app_init(10000u);
    TEST_ASSERT_EQUAL_INT(0, telemetry_publish_count());

    connect_client();

    TEST_ASSERT_EQUAL_INT(1, telemetry_publish_count());
    TEST_ASSERT_NOT_NULL(latest_telemetry_message());
    telemetry = cJSON_Parse(latest_telemetry_message());
    TEST_ASSERT_NOT_NULL(telemetry);
    telemetry_assert_shape(telemetry);
    telemetry_assert_only_documented_fields(telemetry);

    TEST_ASSERT_FALSE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(telemetry, "power")));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItemCaseSensitive(
                                  telemetry, "brightness")->valueint);
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItemCaseSensitive(
                                  telemetry, "pwm_duty")->valueint);
    TEST_ASSERT_EQUAL_STRING("0.0.0-test",
        cJSON_GetObjectItemCaseSensitive(telemetry, "fw_version")->valuestring);
    TEST_ASSERT_EQUAL_STRING("esp32-wroom-32d",
        cJSON_GetObjectItemCaseSensitive(telemetry, "hardware")->valuestring);
    uptime = cJSON_GetObjectItemCaseSensitive(telemetry, "uptime_ms");
    TEST_ASSERT_TRUE(cJSON_IsNumber(uptime));
    cJSON_Delete(telemetry);
}

/**
 * Successful state-change trigger: synchronization (attribute response and
 * shared update paths) publishes the record again with the APPLIED state,
 * including the applied PWM duty derived from the applied brightness — not
 * the requested value alone.  `power=true, brightness=55` -> duty 5500;
 * `power=false, brightness=0` -> duty 0 (electrical off).
 */
static void test_telemetry_published_on_sync_change(void)
{
    cJSON *telemetry;

    app_init(10000u);
    connect_client();
    TEST_ASSERT_EQUAL_INT(1, telemetry_publish_count());

    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":55}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_INT(2, telemetry_publish_count());

    telemetry = cJSON_Parse(latest_telemetry_message());
    TEST_ASSERT_NOT_NULL(telemetry);
    telemetry_assert_shape(telemetry);
    TEST_ASSERT_TRUE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(telemetry, "power")));
    TEST_ASSERT_EQUAL_INT(55, cJSON_GetObjectItemCaseSensitive(
                                  telemetry, "brightness")->valueint);
    TEST_ASSERT_EQUAL_INT(5500, cJSON_GetObjectItemCaseSensitive(
                                    telemetry, "pwm_duty")->valueint);
    cJSON_Delete(telemetry);

    /* A shared-attribute update is also a successful change: applied off
     * state publishes pwm_duty 0 while power=false. */
    deliver_update("{\"power\":false,\"brightness\":0}");
    TEST_ASSERT_EQUAL_INT(3, telemetry_publish_count());
    telemetry = cJSON_Parse(latest_telemetry_message());
    TEST_ASSERT_NOT_NULL(telemetry);
    telemetry_assert_shape(telemetry);
    TEST_ASSERT_FALSE(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(telemetry, "power")));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItemCaseSensitive(
                                  telemetry, "brightness")->valueint);
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItemCaseSensitive(
                                  telemetry, "pwm_duty")->valueint);
    cJSON_Delete(telemetry);
}

/**
 * Periodic trigger + rate limit: while connected, poll() only publishes one
 * record per telemetry_period_ms.  Fast polling and polls just before the
 * boundary never add publishes; exactly one periodic publish happens at the
 * boundary and the next one needs another full period.
 */
static void test_telemetry_periodic_rate_limit(void)
{
    app_init(10000u);
    connect_client();
    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":30}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_INT(2, telemetry_publish_count()); /* connect + change */

    /* Fast polling well inside the period: still rate limited. */
    for (int i = 0; i < 50; i++)
    {
        tb_application_poll(s_client);
    }
    TEST_ASSERT_EQUAL_INT(2, telemetry_publish_count());

    /* One ms before the boundary: still suppressed. */
    advance_ms(29999u);
    tb_application_poll(s_client);
    TEST_ASSERT_EQUAL_INT(2, telemetry_publish_count());

    /* At the boundary exactly one periodic publish fires. */
    advance_ms(1u);
    tb_application_poll(s_client);
    TEST_ASSERT_EQUAL_INT(3, telemetry_publish_count());
    TEST_ASSERT_NOT_NULL(latest_telemetry_message());

    /* The next publish requires another full period. */
    tb_application_poll(s_client);
    TEST_ASSERT_EQUAL_INT(3, telemetry_publish_count());
    advance_ms(30000u);
    tb_application_poll(s_client);
    TEST_ASSERT_EQUAL_INT(4, telemetry_publish_count());
}

/**
 * Disconnection suppression: while the transport is down no telemetry is
 * published at all — even after the periodic interval elapses and the
 * application loop polls repeatedly.  Nothing is queued or retried.
 * Reconnecting publishes exactly the connect-time record.
 */
static void test_telemetry_suppressed_while_disconnected(void)
{
    app_init(10000u);
    connect_client();
    deliver_response(last_request_id(),
                     "{\"shared\":{\"power\":true,\"brightness\":30}}");
    TEST_ASSERT_TRUE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_EQUAL_INT(2, telemetry_publish_count());

    mqtt_app_mock_simulate_remote_disconnect();
    TEST_ASSERT_FALSE(tb_application_is_synchronized(s_client));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() >= 1u);

    /* Far past the period + repeated polling: fully suppressed. */
    advance_ms(30000u);
    for (int i = 0; i < 10; i++)
    {
        tb_application_poll(s_client);
    }
    TEST_ASSERT_EQUAL_INT(2, telemetry_publish_count());

    /* Reconnect publishes the connect-time record again. */
    mqtt_app_mock_simulate_connect();
    TEST_ASSERT_EQUAL_INT(3, telemetry_publish_count());
}

/**
 * Secrecy: hostile configuration values (path fragments, JSON quotes,
 * certificate text and token-like strings) never reach a published record
 * and never break the JSON framing.  The module keeps only the safe-charset
 * prefix of each value, and the payload carries exactly the seven documented
 * fields.
 */
static void test_telemetry_excludes_secrets_and_unsafe_strings(void)
{
    tb_application_config_t cfg;
    cJSON *telemetry;
    cJSON *fw_item;
    cJSON *hw_item;
    const char *raw;

    cfg = make_app_config(10000u, 5u);
    cfg.fw_version = "1.2.3\"/config/identity.json";
    cfg.hardware = "esp32-wroom-32d BEGIN CERTIFICATE";
    app_init_cfg(&cfg);
    connect_client();

    TEST_ASSERT_EQUAL_INT(1, telemetry_publish_count());
    raw = latest_telemetry_message();
    TEST_ASSERT_NOT_NULL(raw);

    /* No secret fragments, no paths, no certificate payloads, no tokens. */
    TEST_ASSERT_NULL(strstr(raw, "/config"));
    TEST_ASSERT_NULL(strstr(raw, "identity"));
    TEST_ASSERT_NULL(strstr(raw, "BEGIN"));
    TEST_ASSERT_NULL(strstr(raw, "test_token"));
    TEST_ASSERT_NULL(strstr(raw, "password"));

    /* The payload still parses as one clean object with exactly the seven
     * documented fields and only the safe-charset prefixes of the values. */
    telemetry = cJSON_Parse(raw);
    TEST_ASSERT_NOT_NULL(telemetry);
    telemetry_assert_shape(telemetry);
    telemetry_assert_only_documented_fields(telemetry);

    fw_item = cJSON_GetObjectItemCaseSensitive(telemetry, "fw_version");
    TEST_ASSERT_TRUE(cJSON_IsString(fw_item));
    TEST_ASSERT_EQUAL_STRING("1.2.3", fw_item->valuestring);
    hw_item = cJSON_GetObjectItemCaseSensitive(telemetry, "hardware");
    TEST_ASSERT_TRUE(cJSON_IsString(hw_item));
    TEST_ASSERT_EQUAL_STRING("esp32-wroom-32d", hw_item->valuestring);
    cJSON_Delete(telemetry);
}

/* --------------------------------------------------------------------- */
/* Runner                                                                 */
/* --------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_valid_sync_via_attribute_response);
    RUN_TEST(test_valid_sync_via_shared_update);
    RUN_TEST(test_partial_data_rejected_then_retry);
    RUN_TEST(test_invalid_power_type_rejected);
    RUN_TEST(test_invalid_brightness_type_rejected);
    RUN_TEST(test_brightness_out_of_range_rejected);
    RUN_TEST(test_fractional_brightness_rejected);
    RUN_TEST(test_oversized_payload_rejected);
    RUN_TEST(test_sync_timeout_fails_off_then_retry);
    RUN_TEST(test_duplicate_update_ignored);
    RUN_TEST(test_duplicate_response_dropped);
    RUN_TEST(test_late_response_after_timeout_rejected);
    RUN_TEST(test_stale_cancelled_dropped_and_reconnect);
    RUN_TEST(test_reconnect_authoritative_and_fail_off);
    RUN_TEST(test_invalid_update_fails_off);
    RUN_TEST(test_retry_budget_exhaustion_then_reconnect);
    RUN_TEST(test_lifecycle_and_state_queries);
    RUN_TEST(test_rpc_set_power_valid);
    RUN_TEST(test_rpc_set_brightness_valid);
    RUN_TEST(test_rpc_set_state_valid);
    RUN_TEST(test_rpc_get_state_valid);
    RUN_TEST(test_rpc_malformed_payload_dropped);
    RUN_TEST(test_rpc_missing_field_rejected);
    RUN_TEST(test_rpc_wrong_type_rejected);
    RUN_TEST(test_rpc_brightness_out_of_range_rejected);
    RUN_TEST(test_rpc_unknown_method_rejected);
    RUN_TEST(test_rpc_method_and_payload_bounds_rejected);
    RUN_TEST(test_rpc_hardware_failure_rejected);
    RUN_TEST(test_rpc_disconnected_dropped);
    RUN_TEST(test_telemetry_published_on_connect);
    RUN_TEST(test_telemetry_published_on_sync_change);
    RUN_TEST(test_telemetry_periodic_rate_limit);
    RUN_TEST(test_telemetry_suppressed_while_disconnected);
    RUN_TEST(test_telemetry_excludes_secrets_and_unsafe_strings);
    return UNITY_END();
}
