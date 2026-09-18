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
 *   2. mode policy (TASK-129/TASK-130) — the adapter owns the Wi-Fi start
 *      mode (KLC_WIFI_DEFAULT_MODE): the default build requests AP+STA
 *      (T_WIFI_TYPE_CLI_SER) and it does so BEFORE wifi_mgmt_start()
 *      (ordering, not just occurrence); in AP+STA mode the
 *      connect/disconnect callbacks fire exactly as before the mode policy;
 *      and a concurrent stop during start still disarms the session
 *      regardless of the mode,
 *   3. disconnect — a disconnect-class event forces the lamp output
 *      inactive BEFORE the application on_disconnected() runs, and the
 *      connected-state query turns false,
 *   4. invalid credentials — a failed connect attempt (CONNECT_FAILED)
 *      drives the same fail-off path and leaves the adapter started, so a
 *      later reconnect is possible,
 *   5. reconnect — after a loss the adapter reports the restored
 *      connection and delivers on_connected() again for every restore,
 *   6. stale callbacks — after network_manager_stop() a late event that
 *      raced the unsubscribe is dropped (no application callback runs
 *      again) while the idempotent lamp fail-off still happens,
 *   7. API contract — argument validation, double start, failed start
 *      rollback (nothing stays registered, restart allowed), the
 *      zero-timeout wait and the stop-before-start no-op,
 *   8. secrecy — the adapter log records state transitions only; no
 *      credential-like content (SSID/password) is ever logged,
 *   9. concurrency — first-use races for the started flag and the lazy
 *      lock (review findings 3 and 4), plus the published-lock lifecycle
 *      race.
 */

#include <pthread.h>
#include <stdatomic.h>
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

/* Start/stop transaction race test (review round 2, issue 1): worker that
 * runs network_manager_start() on its own thread while the test stops the
 * adapter mid-transaction. */
static pthread_t  s_tx_thread;
static atomic_int s_tx_result;

static void *tx_start_worker(void *arg)
{
    network_callbacks_t cb = make_callbacks(NULL);
    atomic_store(&s_tx_result, network_manager_start(&cb));
    (void)arg;
    return NULL;
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
    /* The default build requests AP+STA (T_WIFI_TYPE_CLI_SER): the mode
     * policy (KLC_WIFI_DEFAULT_MODE) is a product decision, and the mode is
     * requested BEFORE the manager is started (ordering, not just
     * occurrence). */
    TEST_ASSERT_EQUAL_UINT32(T_WIFI_TYPE_CLI_SER, counters.last_type);
    TEST_ASSERT_TRUE(counters.type_before_start);
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
/* 2. Mode policy (TASK-129/TASK-130)                                     */
/* --------------------------------------------------------------------- */

/* The Wi-Fi start mode is a product policy decision owned by the adapter
 * (KLC_WIFI_DEFAULT_MODE, main/Kconfig.projbuild): the default build must
 * request AP+STA (T_WIFI_TYPE_CLI_SER) and it must do so BEFORE
 * wifi_mgmt_start() runs — the mode is selected inside the onboarding
 * transaction (set_wifi_type -> init -> subscribe -> start), not applied
 * afterwards.  The mock records this ordering explicitly
 * (type_before_start), so the assertion is order-based, not
 * occurrence-based. */
static void test_default_build_requests_apsta_mode_before_start(void)
{
    network_callbacks_t cb = make_callbacks(NULL);
    wifi_mock_counters_t counters;

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));

    counters = wifi_mgmt_mock_get_counters();
    /* The default build requests AP+STA: the provisioning portal's soft-AP
     * must be available when no usable credentials exist yet. */
    TEST_ASSERT_EQUAL_UINT32(T_WIFI_TYPE_CLI_SER, counters.last_type);
    /* Ordering: the Kconfig-selected mode was requested BEFORE the manager
     * was started (not just requested at all). */
    TEST_ASSERT_TRUE(counters.type_before_start);
    /* The default onboarding path selects the mode exactly once, before any
     * other platform call. */
    TEST_ASSERT_EQUAL_UINT(1U, counters.set_type_calls);
    TEST_ASSERT_EQUAL_UINT(1U, counters.init_calls);
    TEST_ASSERT_EQUAL_UINT(1U, counters.start_calls);
    TEST_ASSERT_EQUAL_UINT(1U, counters.connect_calls);
    TEST_ASSERT_TRUE(counters.start_before_connect);
}

/* The platform behaves differently in AP+STA mode (the AP interface is up
 * while the station retries), so the connect/disconnect callback semantics
 * must be regression-covered: with the mocked boundary reporting AP+STA
 * mode, on_connected/on_disconnected fire exactly as before the mode policy
 * — CONNECTED delivers on_connected(context) once per event, DISCONNECTED
 * forces the lamp output inactive BEFORE on_disconnected(context) runs. */
static void test_callbacks_fire_in_apsta_mode_as_before(void)
{
    static int ctx;
    network_callbacks_t cb = make_callbacks(&ctx);
    wifi_mock_counters_t counters;

    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));

    /* The session onboarded in AP+STA mode. */
    counters = wifi_mgmt_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT32(T_WIFI_TYPE_CLI_SER, counters.last_type);

    /* Connected: exactly one on_connected with the registered context, no
     * spurious disconnect delivery. */
    wifi_mgmt_mock_set_connected(true);
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(1U, s_app.connected_calls);
    TEST_ASSERT_EQUAL_UINT(0U, s_app.disconnected_calls);
    TEST_ASSERT_EQUAL_PTR(&ctx, s_app.last_context);

    /* Disconnected: fail-off happens BEFORE the application callback, the
     * lamp is off and exactly one on_disconnected runs. */
    wifi_mgmt_mock_set_connected(false);
    TEST_ASSERT_EQUAL_INT(1,
                          wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_DISCONNECTED));
    TEST_ASSERT_EQUAL_UINT(1U, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL_UINT(1U, s_app.lamp_off_calls_at_disconnect_entry);
    TEST_ASSERT_EQUAL_UINT(1U, s_app.disconnected_calls);
    TEST_ASSERT_EQUAL_UINT(1U, s_app.connected_calls);
    TEST_ASSERT_FALSE(network_manager_is_connected());

    /* A second cycle delivers the same exact semantics again. */
    wifi_mgmt_mock_set_connected(true);
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(2U, s_app.connected_calls);
    wifi_mgmt_mock_set_connected(false);
    TEST_ASSERT_EQUAL_INT(1,
                          wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_DISCONNECTED));
    TEST_ASSERT_EQUAL_UINT(2U, s_app.disconnected_calls);
    TEST_ASSERT_EQUAL_PTR(&ctx, s_app.last_context);
}

/* Start/stop transaction race regression for the mode policy (TASK-130): a
 * concurrent stop() that completes while a start() is still inside its
 * platform transaction must still disarm the session REGARDLESS of the
 * Kconfig-selected mode — with the default AP+STA request active, no
 * subscription survives the completed stop(), no application callback ever
 * fires and the racing start() reports a startup failure (never NETWORK_OK
 * for an adapter that was already stopped). */
static void test_stop_during_start_disarms_apsta_session(void)
{
    network_callbacks_t cb = make_callbacks(NULL);
    wifi_mock_counters_t counters;

    /* Park the in-flight start() inside its transaction (after it subscribed
     * and started the manager), exactly like the generic race regression —
     * but now with the AP+STA mode policy observable in the counters. */
    wifi_mgmt_mock_block_wait_ready();
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&s_tx_thread, NULL,
                                            tx_start_worker, NULL));
    wifi_mgmt_mock_wait_blocked_in_wait_ready();

    /* The mode policy was applied before the manager was started. */
    counters = wifi_mgmt_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT32(T_WIFI_TYPE_CLI_SER, counters.last_type);
    TEST_ASSERT_TRUE(counters.type_before_start);

    /* A complete stop() runs while the start() is still in flight: it must
     * disarm the session regardless of the mode. */
    network_manager_stop();
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECTED));

    /* Let the start() continue: it must observe the completed stop(), roll
     * everything back and report a startup failure. */
    wifi_mgmt_mock_release_wait_ready();
    TEST_ASSERT_EQUAL_INT(0, pthread_join(s_tx_thread, NULL));

    TEST_ASSERT_TRUE(atomic_load(&s_tx_result) != NETWORK_OK);

    /* Nothing survives the completed stop(): no subscription and no armed
     * callback; a fresh AP+STA start works normally afterwards. */
    counters = wifi_mgmt_mock_get_counters();
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(0U, s_app.connected_calls);
    TEST_ASSERT_EQUAL_UINT(0U, s_app.disconnected_calls);
    TEST_ASSERT_FALSE(network_manager_is_connected());
    (void)counters;

    wifi_mock_config_t config = { .connected_state = true };
    wifi_mgmt_mock_set_config(&config);
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    TEST_ASSERT_TRUE(network_manager_wait_connected(0));
    counters = wifi_mgmt_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT32(T_WIFI_TYPE_CLI_SER, counters.last_type);
}

/* --------------------------------------------------------------------- */
/* 3. Disconnect: fail-off before the application callback                */
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
/* 4. Invalid credentials: connect attempt fails, device fails off        */
/* --------------------------------------------------------------------- */

static void test_invalid_credentials_fail_off_and_stay_started(void)
{
    network_callbacks_t cb = make_callbacks(NULL);

    /* Wrong persisted credentials: the manager accepts the start, the
     * connect request itself is accepted, but the attempt fails
     * asynchronously (CONNECT_FAILED): the adapter maps that onto the
     * disconnect path and stays started, so a corrected credential set (or
     * an AP coming back) can still be picked up.  (A connect request that
     * is rejected synchronously is a different, startup-failure path and
     * has its own test below.) */
    wifi_mock_config_t config = {
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
/* 5. Reconnect                                                           */
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
/* 6. Stale callbacks after stop                                          */
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

/* Stale-token regression (review finding 1): the platform manager snapshots
 * its subscriber list and can deliver an event AFTER stop() completed and a
 * fresh start() registered new callbacks/context.  A late event from the
 * OLD session carries the OLD per-start generation token and must never be
 * delivered to the NEW session's callbacks/context. */
static void test_stale_token_from_previous_session_is_dropped(void)
{
    static int first_ctx;
    static int second_ctx;
    network_callbacks_t cb = make_callbacks(&first_ctx);

    /* Session 1: capture its per-start generation token. */
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    void *old_token =
        wifi_mgmt_mock_get_subscribed_user_data(WIFI_MGMT_EVENT_CONNECTED);
    TEST_ASSERT_NOT_NULL(old_token);
    network_manager_stop();

    /* Session 2: a different context, a fresh (never reused) token. */
    cb = make_callbacks(&second_ctx);
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    void *new_token =
        wifi_mgmt_mock_get_subscribed_user_data(WIFI_MGMT_EVENT_CONNECTED);
    TEST_ASSERT_NOT_NULL(new_token);
    TEST_ASSERT_TRUE(old_token != new_token);

    /* The new session works normally with its own token. */
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(1U, s_app.connected_calls);
    TEST_ASSERT_EQUAL_PTR(&second_ctx, s_app.last_context);

    /* A late event from the manager's pre-restart dispatch snapshot: the
     * handler is current, but the token belongs to the OLD session.  It
     * must NOT reach the new context's callbacks. */
    unsigned connected_before    = s_app.connected_calls;
    unsigned disconnected_before = s_app.disconnected_calls;
    unsigned fail_off_before     = lamp_mock_force_inactive_calls();

    TEST_ASSERT_EQUAL_INT(
        1, wifi_mgmt_mock_emit_with_user_data(WIFI_MGMT_EVENT_CONNECTED,
                                              old_token));
    TEST_ASSERT_EQUAL_UINT(connected_before, s_app.connected_calls);

    /* A late DISCONNECT with the old token also drops the application
     * callback; the (idempotent, safe) lamp fail-off still happens. */
    TEST_ASSERT_EQUAL_INT(
        1, wifi_mgmt_mock_emit_with_user_data(WIFI_MGMT_EVENT_DISCONNECTED,
                                              old_token));
    TEST_ASSERT_EQUAL_UINT(disconnected_before, s_app.disconnected_calls);
    TEST_ASSERT_EQUAL_UINT(fail_off_before + 1U,
                           lamp_mock_force_inactive_calls());

    /* Positive control: the new session's token is still delivered. */
    TEST_ASSERT_EQUAL_INT(
        1, wifi_mgmt_mock_emit_with_user_data(WIFI_MGMT_EVENT_CONNECTED,
                                              new_token));
    TEST_ASSERT_EQUAL_UINT(connected_before + 1U, s_app.connected_calls);
}

/* Synchronous-connect-rejection regression (review finding 5): a connect
 * request rejected by the manager is a startup failure with the same
 * rollback/error path — and the disconnect-class transition (fail-off,
 * then on_disconnected) is delivered while the callbacks are still armed. */
static void test_synchronous_connect_rejection_is_startup_failure(void)
{
    network_callbacks_t cb = make_callbacks(NULL);
    wifi_mock_config_t config = { .fail_connect = true };
    wifi_mock_counters_t counters;

    wifi_mgmt_mock_set_config(&config);
    TEST_ASSERT_EQUAL_INT(NETWORK_ERR_START_FAILED,
                          network_manager_start(&cb));

    /* The disconnect-class transition ran: fail-off, then the application. */
    TEST_ASSERT_EQUAL_UINT(1U, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL_UINT(1U, s_app.lamp_off_calls_at_disconnect_entry);
    TEST_ASSERT_EQUAL_UINT(1U, s_app.disconnected_calls);
    TEST_ASSERT_EQUAL_UINT(0U, s_app.connected_calls);

    /* The same rollback as a startup failure: nothing stays registered. */
    counters = wifi_mgmt_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(1U, counters.connect_calls);
    TEST_ASSERT_EQUAL_UINT(3U, counters.unsubscribe_calls);
    TEST_ASSERT_EQUAL_UINT(1U, counters.stop_calls);
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECTED));

    /* The adapter is left in the not-started state. */
    TEST_ASSERT_FALSE(network_manager_is_connected());
    TEST_ASSERT_EQUAL_INT(0, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(0U, s_app.connected_calls);

    /* A fresh start is allowed from the not-started state. */
    config.fail_connect    = false;
    config.connected_state = true;
    wifi_mgmt_mock_set_config(&config);
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    TEST_ASSERT_TRUE(network_manager_wait_connected(0));
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
/* 7. API contract                                                        */
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

/* Start/stop transaction race regression (review round 2, issue 1): a
 * start() that is still inside its platform registration transaction when a
 * stop() completes must NOT return NETWORK_OK and must not leave any
 * subscription behind — the subscription set after a completed stop() is
 * always empty. */
static void test_start_interrupted_by_stop_rolls_back_completely(void)
{
    network_callbacks_t cb = make_callbacks(NULL);
    wifi_mock_counters_t counters;

    /* The mock blocks wifi_mgmt_wait_ready() until the test releases it:
     * this parks a live start() inside its transaction, after it subscribed
     * and started the manager. */
    wifi_mgmt_mock_block_wait_ready();
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&s_tx_thread, NULL,
                                            tx_start_worker, NULL));

    /* Wait until the in-flight start() has subscribed, then run a full
     * stop(): it must disarm the session and, per the transaction contract,
     * force the racing start() to abort/roll back instead of succeeding. */
    wifi_mgmt_mock_wait_blocked_in_wait_ready();
    network_manager_stop();

    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECTED));

    /* Let the start() continue: it must observe that its session was
     * stopped, roll everything back and report a startup failure (NOT
     * NETWORK_OK for an adapter that was already stopped). */
    wifi_mgmt_mock_release_wait_ready();
    TEST_ASSERT_EQUAL_INT(0, pthread_join(s_tx_thread, NULL));

    TEST_ASSERT_TRUE(atomic_load(&s_tx_result) != NETWORK_OK);

    /* Nothing survives the completed stop(): no subscription and no armed
     * callback; a fresh start works normally afterwards. */
    counters = wifi_mgmt_mock_get_counters();
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(0U, s_app.connected_calls);
    TEST_ASSERT_EQUAL_UINT(0U, s_app.disconnected_calls);
    TEST_ASSERT_FALSE(network_manager_is_connected());
    (void)counters;

    wifi_mock_config_t config = { .connected_state = true };
    wifi_mgmt_mock_set_config(&config);
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    TEST_ASSERT_TRUE(network_manager_wait_connected(0));
}

/* Start/stop CONNECT-window race regression (the remaining open finding): a
 * stop() that completes AFTER the post-wait_ready re-check and BEFORE the
 * start()'s final return — i.e. while the start() is inside its
 * wifi_mgmt_connect() request — must make that start() roll back (disarm,
 * unsubscribe with its own token, wifi_mgmt_stop()) and return
 * NETWORK_ERR_START_FAILED.  A completed stop() owns the stopped state, so
 * the racing start() must neither return NETWORK_OK nor leave this session's
 * subscriptions published. */
static void test_stop_in_connect_window_rolls_back_start(void)
{
    network_callbacks_t cb = make_callbacks(NULL);
    wifi_mock_counters_t counters;

    /* The mock parks wifi_mgmt_connect(): the in-flight start() has already
     * passed the post-wait_ready re-check and is now inside the connect
     * window, before its final return. */
    wifi_mgmt_mock_block_connect();
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&s_tx_thread, NULL,
                                            tx_start_worker, NULL));
    wifi_mgmt_mock_wait_blocked_in_connect();

    /* The stop() completes exactly in that window: it disarms the session
     * and owns the stopped state while the start() is still in flight. */
    network_manager_stop();
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECTED));

    /* Let the start() continue: its final stop_count re-check must observe
     * the completed stop() and roll everything back instead of returning
     * NETWORK_OK for an adapter that is already stopped. */
    wifi_mgmt_mock_release_connect();
    TEST_ASSERT_EQUAL_INT(0, pthread_join(s_tx_thread, NULL));

    TEST_ASSERT_EQUAL_INT(NETWORK_ERR_START_FAILED,
                          atomic_load(&s_tx_result));

    /* Nothing survives the completed stop(): no subscription for any product
     * event and no armed callback; a fresh start works normally from the
     * not-started state. */
    counters = wifi_mgmt_mock_get_counters();
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_DISCONNECTED));
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECT_FAILED));
    TEST_ASSERT_EQUAL_UINT(0U, s_app.connected_calls);
    TEST_ASSERT_EQUAL_UINT(0U, s_app.disconnected_calls);
    TEST_ASSERT_FALSE(network_manager_is_connected());
    (void)counters;

    wifi_mock_config_t config = { .connected_state = true };
    wifi_mgmt_mock_set_config(&config);
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    TEST_ASSERT_TRUE(network_manager_wait_connected(0));
}

/* --------------------------------------------------------------------- */
/* 8. Secrecy: no credential content in adapter logs                      */
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

    /* No credential-looking content may appear in adapter logs.
     * (wifi_managment.h no longer ships WIFI_AP_NAME / WIFI_AP_PASSWORD at
     * all — the provisioning AP identity is runtime-only — so the generic
     * SSID/password checks below are the regression surface.) */
    TEST_ASSERT_NULL(strstr(log, "ssid"));
    TEST_ASSERT_NULL(strstr(log, "SSID"));
    TEST_ASSERT_NULL(strstr(log, "password"));
    TEST_ASSERT_NULL(strstr(log, "Password"));
}

/* --------------------------------------------------------------------- */
/* 9. Concurrency: first-use races (review findings 3 and 4)              */
/* --------------------------------------------------------------------- */

/* The adapter mutex is created lazily on the very first API call and the
 * started flag is a lock-free atomic, so the FIRST concurrent burst of
 * start/stop/is_connected calls is exactly the window where a non-atomic
 * started flag (finding 3) or a read-then-create lock (finding 4) would
 * race.  These tests release a burst of threads through a pthread barrier
 * so they genuinely collide on first use, and assert the invariants that
 * must survive the collision: exactly one winning start session, no
 * duplicate platform registrations left behind, coherent query results and
 * a final lifecycle state a fresh start can build on. */

#include <pthread.h>
#include <stdatomic.h>

#define CONCURRENT_THREADS 8

/**
 * The main Unity suite runs all tests in one process, so the TRUE
 * first-use window (adapter lock still unpublished) is unreachable here
 * once any earlier test has used the adapter API — silently running a
 * "first-use" burst in that state was exactly the regression review round
 * 3 (issue 4) flagged.  These published-lock burst tests therefore assert
 * the lock IS already published, and the genuine first-use window is
 * covered by the dedicated fresh-process executable
 * tests/network_manager/concurrency (network_manager_concurrency_tests),
 * which asserts the NULL-lock premise explicitly.
 */

typedef struct concurrency_result
{
    _Atomic unsigned start_ok;      /**< NETWORK_OK starts.                 */
    _Atomic unsigned start_rejected;/**< ALREADY_STARTED / START_FAILED.    */
    _Atomic unsigned stop_calls;    /**< stop() invocations completed.      */
    _Atomic unsigned query_calls;   /**< is_connected() calls completed.    */
} concurrency_result_t;

static concurrency_result_t s_conc;
static pthread_barrier_t    s_burst_barrier;

static void *start_worker(void *arg)
{
    network_callbacks_t cb = make_callbacks(NULL);

    (void)arg;
    (void)pthread_barrier_wait(&s_burst_barrier);
    switch (network_manager_start(&cb))
    {
        case NETWORK_OK:
            atomic_fetch_add(&s_conc.start_ok, 1u);
            break;
        default:
            /* ALREADY_STARTED or a clean START_FAILED: both are coherent
             * rejections for a losing racer. */
            atomic_fetch_add(&s_conc.start_rejected, 1u);
            break;
    }
    return NULL;
}

static void *stop_worker(void *arg)
{
    (void)arg;
    (void)pthread_barrier_wait(&s_burst_barrier);
    network_manager_stop();
    atomic_fetch_add(&s_conc.stop_calls, 1u);
    return NULL;
}

static void *is_connected_worker(void *arg)
{
    (void)arg;
    (void)pthread_barrier_wait(&s_burst_barrier);
    /* Lock-free query racing start/stop: legal for the atomic started flag
     * (finding 3) and it hammers the lazy mutex publication window
     * (finding 4). */
    (void)network_manager_is_connected();
    atomic_fetch_add(&s_conc.query_calls, 1u);
    return NULL;
}

static void run_concurrent_burst(void *(*entry)(void *), unsigned count)
{
    pthread_t threads[CONCURRENT_THREADS];

    TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&s_burst_barrier, NULL,
                                                  count));
    for (unsigned i = 0u; i < count; ++i)
    {
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL, entry,
                                                NULL));
    }
    for (unsigned i = 0u; i < count; ++i)
    {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(threads[i], NULL));
    }
    TEST_ASSERT_EQUAL_INT(0, pthread_barrier_destroy(&s_burst_barrier));
}

/* Concurrent start collision with the lock ALREADY published (review
 * findings 3 and 4): exactly one start may win; the others must be rejected
 * coherently — and the manager must end up with a single, consistent
 * registration set (no duplicate subscribers from racing arm/subscribe
 * paths).
 *
 * The TRUE first-use window (unpublished lock) cannot be reached inside a
 * Unity suite where earlier tests have already used the adapter API; that
 * case is covered by the dedicated fresh-process executable
 * tests/network_manager/concurrency (network_manager_concurrency_tests),
 * which asserts the NULL-lock premise explicitly.  This test covers the
 * complementary published-lock lifecycle race. */
static void test_concurrent_first_start_race_is_safe(void)
{
    wifi_mock_counters_t counters;

    /* Premise of this variant: the lock is already published by the earlier
     * tests of this suite (the fresh-process first-use case lives in the
     * standalone concurrency executable, which asserts the opposite). */
    TEST_ASSERT_NOT_NULL_MESSAGE(network_manager_test_get_lock(),
                                 "premise: published-lock variant (the "
                                 "fresh-process first-use burst lives in "
                                 "tests/network_manager/concurrency)");

    memset(&s_conc, 0, sizeof(s_conc));
    run_concurrent_burst(start_worker, CONCURRENT_THREADS);

    /* Exactly one session owns the adapter; the rest were rejected without
     * corrupting the lifecycle state (no invalid mutex use, no torn flag). */
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_conc.start_ok));
    TEST_ASSERT_EQUAL_UINT(CONCURRENT_THREADS - 1U,
                           atomic_load(&s_conc.start_rejected));

    /* The winner's onboarding ran: the default AP+STA mode selected (before
     * the manager was started) and the three product events subscribed
     * exactly once (the losers never subscribed: they were rejected under
     * the adapter lock before touching the manager). */
    counters = wifi_mgmt_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT32(T_WIFI_TYPE_CLI_SER, counters.last_type);
    TEST_ASSERT_TRUE(counters.type_before_start);
    TEST_ASSERT_EQUAL_UINT(3U, counters.subscribe_calls);

    /* The adapter is operational after the race: events deliver normally
     * to the one armed session. */
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(1U, s_app.connected_calls);

    network_manager_stop();
    TEST_ASSERT_FALSE(network_manager_is_connected());
}

/* Concurrent start/stop/is_connected collision with the lock ALREADY
 * published (review findings 3 and 4): start threads, stop threads and
 * lock-free query threads all race through the same mutex and atomic flag.
 * The invariants are structural: no deadlock or corruption (every call
 * returns), the query is answered, and the end state is a clean adapter
 * that a fresh start can arm again.
 *
 * The unpublished-lock first-use window is covered by the dedicated
 * fresh-process executable tests/network_manager/concurrency
 * (network_manager_concurrency_tests). */
static void test_concurrent_first_start_stop_is_connected_race_is_safe(void)
{
    memset(&s_conc, 0, sizeof(s_conc));

    /* Premise of this variant: the lock is already published (see the
     * fresh-process standalone burst for the unpublished window). */
    TEST_ASSERT_NOT_NULL_MESSAGE(network_manager_test_get_lock(),
                                 "premise: published-lock variant (the "
                                 "fresh-process first-use burst lives in "
                                 "tests/network_manager/concurrency)");

    /* One burst: half the threads start, a quarter stop, a quarter query —
     * all released together against the already-published lock. */
    TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&s_burst_barrier, NULL,
                                                  CONCURRENT_THREADS));
    pthread_t threads[CONCURRENT_THREADS];
    for (unsigned i = 0u; i < CONCURRENT_THREADS; ++i)
    {
        void *(*entry)(void *) = (i < CONCURRENT_THREADS / 2U)
                                     ? start_worker
                                     : (i % 2U == 0U) ? stop_worker
                                                      : is_connected_worker;
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL, entry,
                                                NULL));
    }
    for (unsigned i = 0u; i < CONCURRENT_THREADS; ++i)
    {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(threads[i], NULL));
    }
    TEST_ASSERT_EQUAL_INT(0, pthread_barrier_destroy(&s_burst_barrier));

    /* Every worker made it through the race alive and coherent. */
    TEST_ASSERT_EQUAL_UINT(CONCURRENT_THREADS,
                           atomic_load(&s_conc.start_ok) +
                               atomic_load(&s_conc.start_rejected) +
                               atomic_load(&s_conc.stop_calls) +
                               atomic_load(&s_conc.query_calls));

    /* Whatever the interleaving produced, the lifecycle settles into a
     * coherent state: stop() is idempotent and leaves the adapter stopped
     * with nothing registered and no application callback delivered. */
    network_manager_stop();
    TEST_ASSERT_FALSE(network_manager_is_connected());
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(0U, s_app.connected_calls);
    TEST_ASSERT_EQUAL_UINT(0U, s_app.disconnected_calls);

    /* A fresh start still works: the lock survived the creation race and
     * the lifecycle state is arming again. */
    network_callbacks_t cb = make_callbacks(NULL);
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    TEST_ASSERT_NOT_NULL(
        wifi_mgmt_mock_get_subscribed_user_data(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(1U, s_app.connected_calls);
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

    /* 2. mode policy (TASK-129/TASK-130) */
    RUN_TEST(test_default_build_requests_apsta_mode_before_start);
    RUN_TEST(test_callbacks_fire_in_apsta_mode_as_before);
    RUN_TEST(test_stop_during_start_disarms_apsta_session);

    /* 3. disconnect */
    RUN_TEST(test_disconnect_forces_lamp_off_before_callback);
    RUN_TEST(test_connect_failure_event_also_fails_off);
    RUN_TEST(test_fail_off_error_is_survived_and_logged);
    RUN_TEST(test_unrelated_events_are_ignored);

    /* 4. invalid credentials */
    RUN_TEST(test_invalid_credentials_fail_off_and_stay_started);

    /* 5. reconnect */
    RUN_TEST(test_reconnect_after_loss_restores_connection);
    RUN_TEST(test_repeated_connect_events_deliver_each_time);

    /* 6. stale callbacks */
    RUN_TEST(test_late_event_after_stop_is_dropped_but_fails_off);
    RUN_TEST(test_stale_token_from_previous_session_is_dropped);
    RUN_TEST(test_stop_disarms_and_unsubscribes);
    RUN_TEST(test_stop_is_idempotent);

    /* 7. API contract */
    RUN_TEST(test_start_rejects_invalid_callbacks);
    RUN_TEST(test_double_start_is_rejected);
    RUN_TEST(test_failed_start_rolls_back_and_allows_restart);
    RUN_TEST(test_synchronous_connect_rejection_is_startup_failure);
    RUN_TEST(test_restart_with_new_context);
    RUN_TEST(test_start_interrupted_by_stop_rolls_back_completely);
    RUN_TEST(test_stop_in_connect_window_rolls_back_start);
    RUN_TEST(test_is_connected_false_before_start);

    /* 8. secrecy */
    RUN_TEST(test_logs_never_contain_credential_content);

    /* 9. concurrency: first-use races for the started flag and the lazy
     *    lock (review findings 3 and 4). */
    RUN_TEST(test_concurrent_first_start_race_is_safe);
    RUN_TEST(test_concurrent_first_start_stop_is_connected_race_is_safe);

    return UNITY_END();
}
