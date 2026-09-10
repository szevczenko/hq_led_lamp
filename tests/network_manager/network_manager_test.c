/**
 * @file network_manager_test.c
 * @brief Host unit tests for the product-owned network adapter (TASK-109)
 *
 * Runs the production components/network_manager code against the in-memory
 * Wi-Fi manager double (wifi_mgmt_mock.c), the OSAL doubles
 * (osal_test_support.c) and the lamp-control double (lamp_control_mock.c)
 * under Unity.  Coverage per the TASK-109 definition of done:
 *
 *   1. connect — a successful start performs Wi-Fi onboarding in order
 *      (type selection -> init -> subscribe -> start -> connect), reports
 *      NETWORK_OK, arms both callbacks and gates ThingsBoard through
 *      network_manager_wait_connected(),
 *   2. disconnect — a disconnect-class event forces the lamp output
 *      inactive BEFORE the application on_disconnected() runs, and the
 *      connected-state query turns false,
 *   3. invalid credentials — a failed connect attempt (CONNECT_FAILED)
 *      drives the same fail-off path and leaves the adapter started, so a
 *      later reconnect is possible,
 *   4. reconnect — after a loss the adapter reports the restored
 *      connection and delivers on_connected() again for every restore,
 *   5. stale callbacks — after network_manager_stop() a late event that
 *      raced the unsubscribe is dropped (no application callback runs
 *      again) while the idempotent lamp fail-off still happens,
 *   6. API contract — argument validation, double start, failed start
 *      rollback (nothing stays registered, restart allowed), the
 *      zero-timeout wait and the stop-before-start no-op,
 *   7. secrecy — the adapter log records state transitions only; no
 *      credential-like content (SSID/password) is ever logged.
 */

#include <string.h>

#include "lamp_control.h"
#include "lamp_control_mock.h"
#include "network_manager.h"
#include "osal_error.h"
#include "osal_test_support.h"
#include "unity.h"
#include "wifi_managment.h"
#include "wifi_mgmt_mock.h"

/* --------------------------------------------------------------------- */
/* Test fixtures                                                          */
/* --------------------------------------------------------------------- */

typedef struct app_callbacks
{
    unsigned connected_calls;
    unsigned disconnected_calls;
    void    *last_context;
    unsigned lamp_off_calls_at_connect_entry;
    unsigned lamp_off_calls_at_disconnect_entry;
} app_callbacks_t;

static app_callbacks_t s_app;

static void on_connected(void *context)
{
    s_app.connected_calls++;
    s_app.last_context = context;
    /* The disconnect path must have forced the output off before any
     * application callback of a subsequent cycle could run; for the
     * connect path we record the (0) baseline. */
    s_app.lamp_off_calls_at_connect_entry = lamp_mock_force_inactive_calls();
}

static void on_disconnected(void *context)
{
    s_app.disconnected_calls++;
    s_app.last_context = context;
    /* Fail-off ordering: the electrical safety action must already have
     * happened when the application state machine is informed. */
    s_app.lamp_off_calls_at_disconnect_entry =
        lamp_mock_force_inactive_calls();
}

static network_callbacks_t make_callbacks(void *context)
{
    network_callbacks_t cb = {
        .on_connected    = on_connected,
        .on_disconnected = on_disconnected,
        .context         = context,
    };
    return cb;
}

void setUp(void)
{
    wifi_mgmt_mock_reset();
    lamp_mock_reset();
    osal_test_log_reset();
    memset(&s_app, 0, sizeof(s_app));
}

void tearDown(void)
{
    /* Every test leaves the adapter stopped: no callback can fire later. */
    network_manager_stop();
}

/* --------------------------------------------------------------------- */
/* 1. Connect: onboarding order, callbacks, connection gate               */
/* --------------------------------------------------------------------- */

static void test_connect_starts_wifi_and_reports_connected(void)
{
    network_callbacks_t cb = make_callbacks(NULL);
    wifi_mock_counters_t counters;

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    TEST_ASSERT_TRUE(network_manager_is_connected() == false);

    counters = wifi_mgmt_mock_get_counters();
    /* Station mode is the product role. */
    TEST_ASSERT_EQUAL_UINT32(T_WIFI_TYPE_CLIENT, counters.last_type);
    TEST_ASSERT_TRUE(counters.set_type_calls >= 1U);
    TEST_ASSERT_TRUE(counters.init_calls >= 1U);
    TEST_ASSERT_TRUE(counters.start_calls >= 1U);
    TEST_ASSERT_TRUE(counters.subscribe_calls >= 3U);
    /* The explicit connect request happens after start. */
    TEST_ASSERT_EQUAL_UINT(1U, counters.connect_calls);
    TEST_ASSERT_TRUE(counters.start_before_connect);

    /* The gate reports not-connected until the manager does. */
    TEST_ASSERT_FALSE(network_manager_wait_connected(0));

    wifi_mgmt_mock_set_connected(true);
    TEST_ASSERT_TRUE(network_manager_wait_connected(0));
    TEST_ASSERT_TRUE(network_manager_is_connected());
}

static void test_connect_event_delivers_on_connected_with_context(void)
{
    static int marker;
    network_callbacks_t cb = make_callbacks(&marker);

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));

    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));

    TEST_ASSERT_EQUAL_UINT(1U, s_app.connected_calls);
    TEST_ASSERT_EQUAL_UINT(0U, s_app.disconnected_calls);
    TEST_ASSERT_EQUAL_PTR(&marker, s_app.last_context);
}

static void test_wait_connected_blocks_until_connected(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    TEST_ASSERT_FALSE(network_manager_wait_connected(0));

    /* Deliver the connected event and set the state, as the manager would
     * from its worker task. */
    wifi_mgmt_mock_set_connected(true);
    (void)wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED);

    /* The zero-timeout gate is an immediate check; polling behavior with
     * real sleeping is a hardware/integration concern and is not tested
     * on the host. */
    TEST_ASSERT_TRUE(network_manager_wait_connected(0));
    TEST_ASSERT_EQUAL_UINT(0U, osal_test_delay_call_count());
}

/* --------------------------------------------------------------------- */
/* 2. Disconnect: fail-off before the application callback                */
/* --------------------------------------------------------------------- */

static void test_disconnect_forces_lamp_off_before_callback(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    wifi_mgmt_mock_set_connected(true);
    (void)wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED);
    TEST_ASSERT_EQUAL_UINT(0U, lamp_mock_force_inactive_calls());

    wifi_mgmt_mock_set_connected(false);
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_DISCONNECTED));

    /* Exactly one fail-off, performed BEFORE the application callback. */
    TEST_ASSERT_EQUAL_UINT(1U, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL_UINT(1U, s_app.lamp_off_calls_at_disconnect_entry);
    TEST_ASSERT_EQUAL_UINT(1U, s_app.disconnected_calls);
    TEST_ASSERT_FALSE(network_manager_is_connected());
}

static void test_connect_failure_event_also_fails_off(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));

    TEST_ASSERT_EQUAL_INT(1,
                          wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECT_FAILED));

    TEST_ASSERT_EQUAL_UINT(1U, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL_UINT(1U, s_app.lamp_off_calls_at_disconnect_entry);
    TEST_ASSERT_EQUAL_UINT(1U, s_app.disconnected_calls);
    TEST_ASSERT_EQUAL_UINT(0U, s_app.connected_calls);
}

static void test_fail_off_error_is_survived_and_logged(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));

    lamp_mock_set_force_inactive_result(LAMP_ERR_FAIL_OFF);
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_DISCONNECTED));

    /* The application state machine is still informed. */
    TEST_ASSERT_EQUAL_UINT(1U, s_app.disconnected_calls);
    /* The failure is logged, never silently swallowed. */
    TEST_ASSERT_NOT_NULL(strstr(osal_test_log_get(), "fail-off"));
}

static void test_unrelated_events_are_ignored(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));

    (void)wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_SCAN_COMPLETED);
    (void)wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_MODE_CHANGED);

    TEST_ASSERT_EQUAL_UINT(0U, s_app.connected_calls);
    TEST_ASSERT_EQUAL_UINT(0U, s_app.disconnected_calls);
    TEST_ASSERT_EQUAL_UINT(0U, lamp_mock_force_inactive_calls());
}

/* --------------------------------------------------------------------- */
/* 3. Invalid credentials: connect attempt fails, device fails off        */
/* --------------------------------------------------------------------- */

static void test_invalid_credentials_fail_off_and_stay_started(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    /* The manager accepts start but every connect attempt fails (wrong
     * persisted credentials): the adapter maps that onto the disconnect
     * path and stays started, so a corrected credential set (or an AP
     * coming back) can still be picked up. */
    wifi_mock_config_t config = {
        .fail_connect    = true,
        .connected_state = false,
    };
    wifi_mgmt_mock_set_config(&config);

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));

    TEST_ASSERT_EQUAL_INT(1,
                          wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECT_FAILED));

    TEST_ASSERT_EQUAL_UINT(1U, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL_UINT(1U, s_app.disconnected_calls);
    TEST_ASSERT_EQUAL_UINT(0U, s_app.connected_calls);
    TEST_ASSERT_FALSE(network_manager_is_connected());
    TEST_ASSERT_FALSE(network_manager_wait_connected(0));

    /* The adapter stays armed: a later successful attempt reconnects. */
    config.fail_connect    = false;
    config.connected_state = true;
    wifi_mgmt_mock_set_config(&config);
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(1U, s_app.connected_calls);
    TEST_ASSERT_TRUE(network_manager_wait_connected(0));
}

/* --------------------------------------------------------------------- */
/* 4. Reconnect                                                           */
/* --------------------------------------------------------------------- */

static void test_reconnect_after_loss_restores_connection(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));

    /* First connection. */
    wifi_mgmt_mock_set_connected(true);
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(1U, s_app.connected_calls);

    /* Loss. */
    wifi_mgmt_mock_set_connected(false);
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_DISCONNECTED));
    TEST_ASSERT_EQUAL_UINT(1U, s_app.disconnected_calls);
    TEST_ASSERT_FALSE(network_manager_is_connected());

    /* Reconnect: the callback fires again and the gate passes. */
    wifi_mgmt_mock_set_connected(true);
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(2U, s_app.connected_calls);
    TEST_ASSERT_EQUAL_UINT(1U, s_app.disconnected_calls);
    TEST_ASSERT_TRUE(network_manager_is_connected());
    TEST_ASSERT_TRUE(network_manager_wait_connected(0));
}

static void test_repeated_connect_events_deliver_each_time(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    wifi_mgmt_mock_set_connected(true);

    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));

    TEST_ASSERT_EQUAL_UINT(2U, s_app.connected_calls);
}

/* --------------------------------------------------------------------- */
/* 5. Stale callbacks after stop                                          */
/* --------------------------------------------------------------------- */

static void test_late_event_after_stop_is_dropped_but_fails_off(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));

    /* Simulate the manager's dispatch snapshot: grab the adapter's event
     * handler while it is subscribed, then stop the adapter.  The late
     * event invokes the captured handler after stop() returned. */
    wifi_mgmt_event_cb_t late_cb =
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECTED);
    TEST_ASSERT_NOT_NULL(late_cb);

    network_manager_stop();
    TEST_ASSERT_FALSE(network_manager_is_connected());

    /* Registration is gone: an emitted event reaches nobody... */
    TEST_ASSERT_EQUAL_INT(0, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_INT(0,
                          wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_DISCONNECTED));

    /* ...and a late delivery through the snapshot is re-validated and
     * dropped by the adapter: no application callback runs again. */
    unsigned connected_before    = s_app.connected_calls;
    unsigned disconnected_before = s_app.disconnected_calls;

    late_cb(WIFI_MGMT_EVENT_CONNECTED, NULL);
    TEST_ASSERT_EQUAL_UINT(connected_before, s_app.connected_calls);

    /* A late disconnect-class event still performs the idempotent
     * (safe-by-construction) lamp fail-off before it is dropped. */
    unsigned fail_off_before = lamp_mock_force_inactive_calls();
    late_cb(WIFI_MGMT_EVENT_DISCONNECTED, NULL);
    TEST_ASSERT_EQUAL_UINT(fail_off_before + 1U,
                           lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL_UINT(disconnected_before, s_app.disconnected_calls);
}

static void test_stop_disarms_and_unsubscribes(void)
{
    network_callbacks_t cb = make_callbacks(NULL);
    wifi_mock_counters_t counters;

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    network_manager_stop();

    counters = wifi_mgmt_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(3U, counters.unsubscribe_calls);
    TEST_ASSERT_TRUE(counters.disconnect_calls >= 1U);
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_DISCONNECTED));

    /* Events after stop reach nobody. */
    TEST_ASSERT_EQUAL_INT(0, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(0U, s_app.connected_calls);
}

static void test_stop_is_idempotent(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    network_manager_stop(); /* stop-before-start: no-op */
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    network_manager_stop();
    network_manager_stop();
    TEST_ASSERT_FALSE(network_manager_is_connected());
}

/* --------------------------------------------------------------------- */
/* 6. API contract                                                        */
/* --------------------------------------------------------------------- */

static void test_start_rejects_invalid_callbacks(void)
{
    network_callbacks_t missing_connected = {
        .on_connected    = NULL,
        .on_disconnected = on_disconnected,
        .context         = NULL,
    };
    network_callbacks_t missing_disconnected = {
        .on_connected    = on_connected,
        .on_disconnected = NULL,
        .context         = NULL,
    };

    TEST_ASSERT_EQUAL_INT(NETWORK_ERR_INVALID_ARGUMENT,
                          network_manager_start(NULL));
    TEST_ASSERT_EQUAL_INT(NETWORK_ERR_INVALID_ARGUMENT,
                          network_manager_start(&missing_connected));
    TEST_ASSERT_EQUAL_INT(NETWORK_ERR_INVALID_ARGUMENT,
                          network_manager_start(&missing_disconnected));

    /* Nothing was touched: no manager call happened, nothing armed. */
    wifi_mock_counters_t counters = wifi_mgmt_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(0U, counters.start_calls);
    TEST_ASSERT_EQUAL_UINT(0U, counters.subscribe_calls);
}

static void test_double_start_is_rejected(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    TEST_ASSERT_EQUAL_INT(NETWORK_ERR_ALREADY_STARTED,
                          network_manager_start(&cb));

    /* The original registration stays intact and operational. */
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(1U, s_app.connected_calls);
}

static void test_failed_start_rolls_back_and_allows_restart(void)
{
    network_callbacks_t cb = make_callbacks(NULL);
    wifi_mock_config_t config = { .fail_wait_ready = true };
    wifi_mock_counters_t counters;

    wifi_mgmt_mock_set_config(&config);
    TEST_ASSERT_EQUAL_INT(NETWORK_ERR_START_FAILED,
                          network_manager_start(&cb));

    /* Rollback: everything is unregistered, the manager is stopped. */
    counters = wifi_mgmt_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(3U, counters.unsubscribe_calls);
    TEST_ASSERT_EQUAL_UINT(1U, counters.stop_calls);
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECTED));

    /* A failed start may not leave an armed callback behind. */
    TEST_ASSERT_EQUAL_INT(0, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(0U, s_app.connected_calls);
    TEST_ASSERT_FALSE(network_manager_is_connected());

    /* A fresh start is allowed from the not-started state. */
    config.fail_wait_ready = false;
    config.connected_state = true;
    wifi_mgmt_mock_set_config(&config);
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    TEST_ASSERT_TRUE(network_manager_wait_connected(0));
}

static void test_restart_with_new_context(void)
{
    static int first_ctx;
    static int second_ctx;

    network_callbacks_t cb = make_callbacks(&first_ctx);
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_PTR(&first_ctx, s_app.last_context);

    network_manager_stop();

    cb                = make_callbacks(&second_ctx);
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_PTR(&second_ctx, s_app.last_context);
    TEST_ASSERT_EQUAL_UINT(2U, s_app.connected_calls);
}

static void test_is_connected_false_before_start(void)
{
    TEST_ASSERT_FALSE(network_manager_is_connected());
    TEST_ASSERT_FALSE(network_manager_wait_connected(0));
}

/* --------------------------------------------------------------------- */
/* 7. Secrecy: no credential content in adapter logs                      */
/* --------------------------------------------------------------------- */

static void test_logs_never_contain_credential_content(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    /* A full lifecycle incl. loss and failure runs with log capture on. */
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    wifi_mgmt_mock_set_connected(true);
    (void)wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED);
    wifi_mgmt_mock_set_connected(false);
    (void)wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_DISCONNECTED);
    (void)wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECT_FAILED);
    network_manager_stop();

    const char *log = osal_test_log_get();
    TEST_ASSERT_NOT_NULL(strstr(log, "network connected"));
    TEST_ASSERT_NOT_NULL(strstr(log, "network disconnected"));

    /* No credential-looking content: neither the platform default AP
     * credentials (WIFI_AP_NAME / WIFI_AP_PASSWORD macros) nor generic
     * SSID/password reporting may appear in adapter logs. */
    TEST_ASSERT_NULL(strstr(log, WIFI_AP_NAME));
    TEST_ASSERT_NULL(strstr(log, WIFI_AP_PASSWORD));
    TEST_ASSERT_NULL(strstr(log, "ssid"));
    TEST_ASSERT_NULL(strstr(log, "SSID"));
    TEST_ASSERT_NULL(strstr(log, "password"));
    TEST_ASSERT_NULL(strstr(log, "Password"));
}

/* --------------------------------------------------------------------- */
/* Runner                                                                 */
/* --------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    /* 1. connect */
    RUN_TEST(test_connect_starts_wifi_and_reports_connected);
    RUN_TEST(test_connect_event_delivers_on_connected_with_context);
    RUN_TEST(test_wait_connected_blocks_until_connected);

    /* 2. disconnect */
    RUN_TEST(test_disconnect_forces_lamp_off_before_callback);
    RUN_TEST(test_connect_failure_event_also_fails_off);
    RUN_TEST(test_fail_off_error_is_survived_and_logged);
    RUN_TEST(test_unrelated_events_are_ignored);

    /* 3. invalid credentials */
    RUN_TEST(test_invalid_credentials_fail_off_and_stay_started);

    /* 4. reconnect */
    RUN_TEST(test_reconnect_after_loss_restores_connection);
    RUN_TEST(test_repeated_connect_events_deliver_each_time);

    /* 5. stale callbacks */
    RUN_TEST(test_late_event_after_stop_is_dropped_but_fails_off);
    RUN_TEST(test_stop_disarms_and_unsubscribes);
    RUN_TEST(test_stop_is_idempotent);

    /* 6. API contract */
    RUN_TEST(test_start_rejects_invalid_callbacks);
    RUN_TEST(test_double_start_is_rejected);
    RUN_TEST(test_failed_start_rolls_back_and_allows_restart);
    RUN_TEST(test_restart_with_new_context);
    RUN_TEST(test_is_connected_false_before_start);

    /* 7. secrecy */
    RUN_TEST(test_logs_never_contain_credential_content);

    return UNITY_END();
}
