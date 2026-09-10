/**
 * @file tb_application_test.c
 * @brief Host mock tests for the ThingsBoard desired-state synchronizer
 *        (TASK-112)
 *
 * Runs the production components/tb_application code against the REAL
 * platform ThingsBoard client/attributes sources (tb_client.c,
 * tb_attributes.c, ...) and the platform's own mqtt_app test double
 * (mqtt_app_mock), exactly like the platform's tb_tests, plus a
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
    return UNITY_END();
}