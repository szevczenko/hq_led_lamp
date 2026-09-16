/**
 * @file app_state_test.c
 * @brief Host fault-injection tests for the application state machine and
 *        watchdog policy (TASK-115)
 *
 * Runs the production components/app_state implementation against a
 * lamp-control double (lamp_control_mock.h) and the OSAL test doubles
 * (osal_test_support.h), with a fully deterministic injected clock
 * (app_state_config_t::now_ms) and an observer that records every
 * (from, to, event, owner, session) transition.
 *
 * Coverage per the TASK-115 definition of done:
 *   - every failure transition (filesystem, configuration, Wi-Fi/timeout,
 *     provisioning, TLS, sync, invalid state, disconnect, OTA) forces the
 *     lamp output inactive — the fail-off invariant is asserted by exact
 *     force-inactive call counts on every non-online entry,
 *   - online is reachable ONLY through all five gates (filesystem,
 *     configuration, Wi-Fi, verified TLS, synchronization), with the
 *     optional provisioning gate (TASK-125) between Wi-Fi and TLS: entered
 *     only from NETWORK on PROVISIONING_STARTED when no saved credential
 *     exists, returning to NETWORK on SUCCEEDED or degrading to SAFE_OFF on
 *     FAILED (same bounded retry budget as NETWORK failures),
 *   - the reconnect loop (disconnect -> bounded backoff -> re-gate ->
 *     online) converges and resets the retry budget on each online entry,
 *   - retry is bounded and cannot produce connection storms: exponential
 *     backoff schedule verified exactly, exhaustion parks the machine
 *     with no further retries despite continued polling,
 *   - timeouts (connect/sync/wait) are delivered as the documented
 *     failure events and drive the same fail-off + bounded retry,
 *   - stale callbacks (wrong generation/session) and wrong-owner / not-
 *     legal-for-state callbacks are rejected and counted; a stale
 *     disconnect-class event still performs the fail-off first,
 *   - watchdog timeout: a starved feed expires exactly at the boundary,
 *     fires the expiry callback exactly once, forces the output inactive
 *     and transitions to FATAL; recovery only through an explicit RESET,
 *   - watchdog recovery: after RESET a fresh boot re-runs all gates and
 *     reaches online again,
 *   - blocking constraints: the only feed is poll(); transitions refresh
 *     the deadline while polls keep a running machine alive,
 *   - OTA entry/exit: beginning OTA from online (and from safe-off) closes
 *     the output and bumps the session; OTA_END returns to boot and a
 *     fresh boot re-enters online; OTA_FAILED degrades to safe-off.
 */

#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "unity.h"

#include "lamp_control_mock.h"
#include "osal_test_support.h"

/* --------------------------------------------------------------------- */
/* Test clock and observer                                                */
/* --------------------------------------------------------------------- */

static uint32_t s_now_ms;

static uint32_t test_now(void)
{
    return s_now_ms;
}

#define OBS_CAPACITY 96

typedef struct obs_record {
    app_state_t           from;
    app_state_t           to;
    app_state_event_t     event;
    app_transition_owner_t owner;
    uint32_t              session;
} obs_record_t;

static int         s_obs_count;
static obs_record_t s_obs[OBS_CAPACITY];

static void test_observer(app_state_t from, app_state_t to,
                          app_state_event_t event,
                          app_transition_owner_t owner, uint32_t session)
{
    if (s_obs_count < OBS_CAPACITY)
    {
        int i = s_obs_count;
        s_obs[i].from    = from;
        s_obs[i].to      = to;
        s_obs[i].event   = event;
        s_obs[i].owner   = owner;
        s_obs[i].session = session;
        s_obs_count++;
    }
}

static void obs_reset(void)
{
    s_obs_count = 0;
}

static int obs_count_where_event(app_state_event_t event)
{
    int i;
    int count = 0;

    for (i = 0; i < s_obs_count; ++i)
    {
        if (s_obs[i].event == event)
        {
            count++;
        }
    }
    return count;
}

/* --------------------------------------------------------------------- */
/* Watchdog expiry callback                                               */
/* --------------------------------------------------------------------- */

static int s_wdt_expired_calls;

static void test_watchdog_expired(void)
{
    s_wdt_expired_calls++;
}

/* --------------------------------------------------------------------- */
/* Configuration helpers                                                  */
/* --------------------------------------------------------------------- */

static app_state_config_t make_cfg(uint32_t wdt_ms)
{
    app_state_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.now_ms                  = test_now;
    cfg.retry_initial_delay_ms  = APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS;
    cfg.retry_max_delay_ms      = APP_STATE_RETRY_MAX_DELAY_DEFAULT_MS;
    cfg.retry_max_attempts      = APP_STATE_RETRY_MAX_ATTEMPTS_DEFAULT;
    cfg.retry_backoff_factor    = APP_STATE_RETRY_BACKOFF_FACTOR_DEFAULT;
    cfg.watchdog_timeout_ms     = wdt_ms;
    cfg.on_watchdog_expired     = test_watchdog_expired;
    cfg.observer                = test_observer;
    return cfg;
}

static app_state_config_t make_custom_retry_cfg(uint32_t initial_ms,
                                                uint32_t max_ms,
                                                uint32_t max_attempts,
                                                uint32_t factor)
{
    app_state_config_t cfg = make_cfg(60000u);

    cfg.retry_initial_delay_ms = initial_ms;
    cfg.retry_max_delay_ms     = max_ms;
    cfg.retry_max_attempts     = max_attempts;
    cfg.retry_backoff_factor   = factor;
    return cfg;
}

/** @brief init() with the given watchdog window (clock + observer wired). */
static app_state_status_t init_with_wdt(uint32_t wdt_ms)
{
    app_state_config_t cfg = make_cfg(wdt_ms);
    return app_state_init(&cfg);
}

/** @brief init() with a custom retry schedule (see make_custom_retry_cfg). */
static app_state_status_t init_with_retry_cfg(uint32_t initial_ms,
                                              uint32_t max_ms,
                                              uint32_t max_attempts,
                                              uint32_t factor)
{
    app_state_config_t cfg = make_custom_retry_cfg(initial_ms, max_ms,
                                                   max_attempts, factor);
    return app_state_init(&cfg);
}

/** @brief init() + start(): the machine is in FILESYSTEM with a fresh
 *         session and the output forced inactive once. */
static void boot_to_filesystem(void)
{
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, init_with_wdt(30000u));
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(APP_STATE_FILESYSTEM, app_state_current());
    TEST_ASSERT_EQUAL(1u, lamp_mock_force_inactive_calls());
}

/** @brief Full legal happy path into ONLINE (all five gates). */
static void drive_online(void)
{
    boot_to_filesystem();

    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_CONFIGURATION, app_state_current());

    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());

    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_TLS, app_state_current());

    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_SYNC, app_state_current());

    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_SYNC_COMPLETE, APP_OWNER_THINGSBOARD));
    TEST_ASSERT_EQUAL(APP_STATE_ONLINE, app_state_current());
    TEST_ASSERT_EQUAL(5u, lamp_mock_force_inactive_calls());
}

/* --------------------------------------------------------------------- */
/* Test scaffolding                                                       */
/* --------------------------------------------------------------------- */

void setUp(void)
{
    lamp_mock_reset();
    osal_test_log_reset();
    osal_test_set_time_ms(0u);
    s_now_ms = 0u;
    obs_reset();
    s_wdt_expired_calls = 0;
}

void tearDown(void)
{
    app_state_deinit();
}

/* --------------------------------------------------------------------- */
/* 1. API contract / lifecycle                                             */
/* --------------------------------------------------------------------- */

static void test_init_rejects_null_config(void)
{
    TEST_ASSERT_EQUAL(APP_STATE_ERR_INVALID_ARGUMENT, app_state_init(NULL));
}

static void test_ops_before_init_return_not_initialized(void)
{
    TEST_ASSERT_EQUAL(APP_STATE_ERR_NOT_INITIALIZED, app_state_start());
    TEST_ASSERT_EQUAL(APP_STATE_ERR_NOT_INITIALIZED,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_ERR_NOT_INITIALIZED,
        app_state_deliver_session(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM, 1u));
    TEST_ASSERT_EQUAL(APP_STATE_ERR_NOT_INITIALIZED, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_BOOT, app_state_current());
    TEST_ASSERT_EQUAL(0u, app_state_session());
    TEST_ASSERT_FALSE(app_state_is_online());
}

static void test_double_init_and_restart(void)
{
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, init_with_wdt(30000u));
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ALREADY_INITIALIZED,
                      init_with_wdt(30000u));

    /* deinit -> a fresh init is allowed and starts at boot. */
    app_state_deinit();
    TEST_ASSERT_EQUAL(APP_STATE_OK, init_with_wdt(30000u));
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(APP_STATE_FILESYSTEM, app_state_current());

    /* after deinit everything is inert again. */
    app_state_deinit();
    TEST_ASSERT_EQUAL(APP_STATE_ERR_NOT_INITIALIZED,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
}

static void test_start_rejects_when_not_in_boot(void)
{
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, init_with_wdt(30000u));
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(APP_STATE_ERR_STATE, app_state_start());
}

static void test_non_deliverable_events_are_illegal(void)
{
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_START, APP_OWNER_BOOTSTRAP));
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_RETRY_DUE, APP_OWNER_TIMER));
    TEST_ASSERT_EQUAL(2u, app_state_invalid_dropped());
}

/* --------------------------------------------------------------------- */
/* 2. Boot sequence: online requires all five gates                       */
/* --------------------------------------------------------------------- */

static void test_happy_path_reaches_online_only_after_all_gates(void)
{
    boot_to_filesystem();

    /* No shorter prefix may enable the output. */
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_FALSE(app_state_is_online());

    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_FALSE(app_state_is_online());

    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_FALSE(app_state_is_online());

    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_FALSE(app_state_is_online());

    /* Only SYNC_COMPLETE flips the bit. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_SYNC_COMPLETE, APP_OWNER_THINGSBOARD));
    TEST_ASSERT_TRUE(app_state_is_online());

    /* The fail-off invariant: one force per non-online entry, none for
     * the online entry. */
    TEST_ASSERT_EQUAL(5u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL(APP_OWNER_THINGSBOARD, app_state_last_owner());
}

static void test_every_non_online_state_forbids_output(void)
{
    /* BOOT (before start) */
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, init_with_wdt(30000u));
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_FALSE(app_state_is_online());

    /* FILESYSTEM .. ONLINE in one sweep. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_SYNC_COMPLETE, APP_OWNER_THINGSBOARD));
    TEST_ASSERT_TRUE(app_state_is_online());

    /* OTA and SAFE_OFF and FATAL forbid output. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_OTA_BEGIN, APP_OWNER_OTA));
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_OTA_FAILED, APP_OWNER_OTA));
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FATAL, APP_OWNER_BOOTSTRAP));
    TEST_ASSERT_FALSE(app_state_is_online());
}

/* --------------------------------------------------------------------- */
/* 3. Failure transitions: every one forces the output inactive            */
/* --------------------------------------------------------------------- */

static void test_fs_failure_transition_degrades(void)
{
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_FAIL, APP_OWNER_FILESYSTEM));

    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(2u, lamp_mock_force_inactive_calls());
    /* Degraded: NO automatic retry is scheduled. */
    TEST_ASSERT_FALSE(app_state_retry_pending());
    TEST_ASSERT_EQUAL(0u, app_state_retry_delay_ms());
}

static void test_config_failure_transition_degrades(void)
{
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_FAIL, APP_OWNER_CONFIGURATION));

    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(3u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_FALSE(app_state_retry_pending());
}

static void test_network_failure_transition_retries_network(void)
{
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_FAILED, APP_OWNER_NETWORK));

    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(4u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_TRUE(app_state_retry_pending());
    TEST_ASSERT_EQUAL(APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS,
                      app_state_retry_delay_ms());
    TEST_ASSERT_EQUAL(0u, app_state_retry_attempts_used());
}

static void test_network_connect_timeout_fault_injection(void)
{
    /* A bounded Wi-Fi wait timeout is reported by the supervisor as a
     * NETWORK_FAILED event; it must fail off and schedule the bounded
     * retry exactly like any other network failure. */
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_FAILED, APP_OWNER_NETWORK));

    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(4u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_TRUE(app_state_retry_pending());
}

static void test_tls_failure_transition_retries_tls(void)
{
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_FAILED, APP_OWNER_MQTT));

    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(5u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_TRUE(app_state_retry_pending());

    /* The bounded retry must return to TLS, not restart the network. */
    s_now_ms = APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_TLS, app_state_current());
}

static void test_sync_failure_transition_retries_sync(void)
{
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_SYNC_FAILED, APP_OWNER_THINGSBOARD));

    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(6u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_TRUE(app_state_retry_pending());

    s_now_ms = APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_SYNC, app_state_current());
}

static void test_sync_timeout_fault_injection(void)
{
    /* A ThingsBoard sync timeout is reported as SYNC_FAILED. */
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_SYNC_FAILED, APP_OWNER_THINGSBOARD));

    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(6u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_TRUE(app_state_retry_pending());
}

static void test_invalid_state_from_sync_retries_sync(void)
{
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_INVALID_STATE, APP_OWNER_THINGSBOARD));

    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(6u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_TRUE(app_state_retry_pending());
}

static void test_disconnect_during_boot_stages_fails_off(void)
{
    /* Wi-Fi can be lost while the device is still booting; the adapter
     * reports DISCONNECTED and the machine must fail off. */
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_DISCONNECTED, APP_OWNER_NETWORK));

    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(2u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_TRUE(app_state_retry_pending());
}

static void test_disconnect_while_online_fails_off(void)
{
    drive_online();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_DISCONNECTED, APP_OWNER_NETWORK));

    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(6u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_TRUE(app_state_retry_pending());
}

static void test_disconnect_from_mqtt_context_while_online_fails_off(void)
{
    drive_online();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_DISCONNECTED, APP_OWNER_MQTT));

    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(6u, lamp_mock_force_inactive_calls());
}

static void test_invalid_state_while_online_fails_off(void)
{
    drive_online();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_INVALID_STATE, APP_OWNER_THINGSBOARD));

    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(6u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_TRUE(app_state_retry_pending());

    /* The retry re-enters at SYNC: the bad data is re-requested. */
    s_now_ms = APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_SYNC, app_state_current());
}

static void test_fatal_from_any_state(void)
{
    drive_online();
    uint32_t session_before = app_state_session();

    /* Any owner may report a fatal failure of its domain. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FATAL, APP_OWNER_CONFIGURATION));

    TEST_ASSERT_EQUAL(APP_STATE_FATAL, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(6u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL(session_before + 1u, app_state_session());

    /* Already fatal: honored without a duplicate transition. */
    int transitions = s_obs_count;
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FATAL, APP_OWNER_WATCHDOG));
    TEST_ASSERT_EQUAL(transitions, s_obs_count);
}

static void test_lamp_failoff_failure_does_not_wedge(void)
{
    /* Even when the lamp-control double cannot prove the output off, the
     * machine must still perform the transition and log the safety event
     * (fail-off responsibility stays with the transition machinery). */
    lamp_mock_set_force_inactive_result(LAMP_ERR_FAIL_OFF);

    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_FAIL, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());

    const char *log = osal_test_log_get();
    TEST_ASSERT_TRUE(log != NULL && strstr(log, "fail-off") != NULL);
}

/* --------------------------------------------------------------------- */
/* 3b. Provisioning gate (TASK-125)                                       */
/* --------------------------------------------------------------------- */

static void test_provisioning_legal_entry_and_exit(void)
{
    /* NETWORK -> PROVISIONING on PROVISIONING_STARTED (no saved station
     * credential); PROVISIONING is a non-online state so the entry forces
     * the output inactive and does not bump the session. */
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());

    uint32_t session_network = app_state_session();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_PROVISIONING, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(4u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL(session_network, app_state_session());

    /* PROVISIONING -> NETWORK on PROVISIONING_SUCCEEDED (credential
     * saved; the station may now connect).  Re-entering NETWORK fails off
     * again (non-online entry), still no session bump. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_SUCCEEDED,
                          APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
    TEST_ASSERT_EQUAL(5u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL(session_network, app_state_session());

    /* The device now connects with the saved credential and passes
     * straight through the remaining gates to ONLINE — provisioning is
     * NOT re-entered. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_TLS, app_state_current());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_SYNC_COMPLETE, APP_OWNER_THINGSBOARD));
    TEST_ASSERT_TRUE(app_state_is_online());
    TEST_ASSERT_EQUAL(session_network, app_state_session());
}

static void test_provisioning_failure_degrades_and_retries_network(void)
{
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_FAILED, APP_OWNER_NETWORK));

    /* Same fail-off + budget semantics as NETWORK failures. */
    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(5u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_TRUE(app_state_retry_pending());
    TEST_ASSERT_EQUAL(APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS,
                      app_state_retry_delay_ms());
    TEST_ASSERT_EQUAL(0u, app_state_retry_attempts_used());

    /* The bounded retry re-enters NETWORK (the stage that owns the
     * provisioning adapter), never PROVISIONING directly. */
    s_now_ms = APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
    TEST_ASSERT_EQUAL(1u, app_state_retry_attempts_used());

    /* A later attempt may save a credential: the device can then connect
     * straight through to TLS without provisioning again. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_TLS, app_state_current());
}

static void test_provisioning_started_illegal_from_every_other_state(void)
{
    /* PROVISIONING may ONLY be entered from NETWORK on
     * PROVISIONING_STARTED (owner NETWORK).  Walk the machine through
     * every other state and assert the event is rejected (dropped and
     * counted) with no transition. */
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, init_with_wdt(30000u));

    /* BOOT (before start). */
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_BOOT, app_state_current());

    /* FILESYSTEM. */
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_FILESYSTEM, app_state_current());

    /* CONFIGURATION. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_CONFIGURATION, app_state_current());

    /* NETWORK is the ONLY legal source: enter provisioning (legal), then
     * a redundant STARTED while already provisioning is illegal. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_PROVISIONING, app_state_current());
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_PROVISIONING, app_state_current());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_SUCCEEDED,
                          APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());

    /* TLS (saved-credential path). */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_TLS, app_state_current());

    /* SYNC. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_SYNC, app_state_current());

    /* ONLINE. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_SYNC_COMPLETE, APP_OWNER_THINGSBOARD));
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_ONLINE, app_state_current());

    /* SAFE_OFF (via disconnect). */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_DISCONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());

    /* OTA (from SAFE_OFF). */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_OTA_BEGIN, APP_OWNER_OTA));
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OTA, app_state_current());

    /* FATAL. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FATAL, APP_OWNER_BOOTSTRAP));
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_FATAL, app_state_current());

    /* Ten probes, ten drops: BOOT, FILESYSTEM, CONFIGURATION, PROVISIONING
     * (redundant STARTED), TLS, SYNC, ONLINE, SAFE_OFF, OTA, FATAL. */
    TEST_ASSERT_EQUAL(10u, app_state_invalid_dropped());
}

static void test_provisioning_retry_exhaustion_parks_in_safe_off(void)
{
    /* Custom tight schedule: 1s initial, 4s cap, 3 retries max, 2x — the
     * same anti-storm budget semantics as NETWORK failures. */
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        init_with_retry_cfg(1000u, 4000u, 3u, 2u));
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));

    /* Provisioning attempt 1 fails: 1s backoff, re-enters NETWORK. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_FAILED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_TRUE(app_state_retry_pending());
    TEST_ASSERT_EQUAL(1000u, app_state_retry_delay_ms());
    TEST_ASSERT_EQUAL(0u, app_state_retry_attempts_used());

    s_now_ms = 1000u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
    TEST_ASSERT_EQUAL(1u, app_state_retry_attempts_used());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_FAILED, APP_OWNER_NETWORK));
    TEST_ASSERT_TRUE(app_state_retry_pending());
    TEST_ASSERT_EQUAL(2000u, app_state_retry_delay_ms());

    /* Retry 2 at 3000 ms; fail again -> capped 4s backoff. */
    s_now_ms = 3000u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(2u, app_state_retry_attempts_used());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_FAILED, APP_OWNER_NETWORK));
    TEST_ASSERT_TRUE(app_state_retry_pending());
    TEST_ASSERT_EQUAL(4000u, app_state_retry_delay_ms());

    /* Retry 3 at 7000 ms; the following failure exhausts the budget. */
    s_now_ms = 7000u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(3u, app_state_retry_attempts_used());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_FAILED, APP_OWNER_NETWORK));

    /* Exhausted: parked in SAFE_OFF, nothing scheduled, no retry storm. */
    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_TRUE(app_state_retry_exhausted());
    TEST_ASSERT_FALSE(app_state_retry_pending());
    TEST_ASSERT_EQUAL(0u, app_state_retry_delay_ms());
    int retries_fired = obs_count_where_event(APP_EVENT_RETRY_DUE);

    /* Continued polling must NOT produce another retry. */
    int iteration;
    for (iteration = 0; iteration < 5; ++iteration)
    {
        s_now_ms += 4000u;
        TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
        TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
        TEST_ASSERT_EQUAL(3u, app_state_retry_attempts_used());
    }
    TEST_ASSERT_EQUAL(retries_fired, obs_count_where_event(APP_EVENT_RETRY_DUE));

    /* The budget still resets only on reaching ONLINE: an explicit reset
     * recovers, a fresh boot re-enters all gates and ONLINE resets the
     * budget for the next recovery episode. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_RESET, APP_OWNER_EXTERNAL));
    TEST_ASSERT_EQUAL(APP_STATE_BOOT, app_state_current());
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_SYNC_COMPLETE, APP_OWNER_THINGSBOARD));
    TEST_ASSERT_TRUE(app_state_is_online());
    TEST_ASSERT_EQUAL(0u, app_state_retry_attempts_used());
    TEST_ASSERT_FALSE(app_state_retry_exhausted());
}

static void test_provisioning_stale_session_delivery_rejected(void)
{
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));

    /* The provisioning adapter arms under THIS session... */
    uint32_t armed_session = app_state_session();

    /* ...then the network fails and the machine retries (new session). */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_FAILED, APP_OWNER_NETWORK));
    s_now_ms = APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
    TEST_ASSERT_NOT_EQUAL(armed_session, app_state_session());

    /* Late PROVISIONING_STARTED from the previous session: dropped and
     * counted (non-safety event: no fail-off), no transition. */
    int transitions_before = s_obs_count;
    unsigned forces_before = lamp_mock_force_inactive_calls();
    TEST_ASSERT_EQUAL(APP_STATE_ERR_STALE,
        app_state_deliver_session(APP_EVENT_PROVISIONING_STARTED,
                                  APP_OWNER_NETWORK, armed_session));
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
    TEST_ASSERT_EQUAL(transitions_before, s_obs_count);
    TEST_ASSERT_EQUAL(forces_before, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL(1u, app_state_stale_dropped());

    /* SUCCEEDED/FAILED are only legal from PROVISIONING: delivered from
     * NETWORK they are illegal.  FAILED is disconnect-class, so the
     * illegal drop still fails off first. */
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_PROVISIONING_SUCCEEDED,
                          APP_OWNER_NETWORK));
    forces_before = lamp_mock_force_inactive_calls();
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_PROVISIONING_FAILED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(forces_before + 1u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL(2u, app_state_invalid_dropped());

    /* Enter provisioning (current session): stale SUCCEEDED and stale
     * FAILED from the old session are both dropped and counted without a
     * transition.  The stale FAILED (disconnect-class) fails off first. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_STARTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_PROVISIONING, app_state_current());
    transitions_before = s_obs_count;
    forces_before = lamp_mock_force_inactive_calls();
    TEST_ASSERT_EQUAL(APP_STATE_ERR_STALE,
        app_state_deliver_session(APP_EVENT_PROVISIONING_SUCCEEDED,
                                  APP_OWNER_NETWORK, armed_session));
    TEST_ASSERT_EQUAL(APP_STATE_PROVISIONING, app_state_current());
    TEST_ASSERT_EQUAL(transitions_before, s_obs_count);
    TEST_ASSERT_EQUAL(forces_before, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL(2u, app_state_stale_dropped());

    forces_before = lamp_mock_force_inactive_calls();
    TEST_ASSERT_EQUAL(APP_STATE_ERR_STALE,
        app_state_deliver_session(APP_EVENT_PROVISIONING_FAILED,
                                  APP_OWNER_NETWORK, armed_session));
    TEST_ASSERT_EQUAL(APP_STATE_PROVISIONING, app_state_current());
    TEST_ASSERT_EQUAL(transitions_before, s_obs_count);
    TEST_ASSERT_EQUAL(forces_before + 1u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL(3u, app_state_stale_dropped());

    /* The current-session callback is still honored afterwards. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_PROVISIONING_SUCCEEDED,
                          APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
}

/* --------------------------------------------------------------------- */
/* 4. Reconnect loop and bounded retry (anti-storm)                        */
/* --------------------------------------------------------------------- */

static void test_reconnect_loop_reaches_online_and_resets_budget(void)
{
    drive_online();

    /* The supervisor loop calls poll() each iteration; wdt is long enough
     * that the reconnect never trips it. */
    int cycle;
    for (cycle = 0; cycle < 3; ++cycle)
    {
        /* Link drop: output off, retry scheduled with backoff. */
        uint32_t session_before = app_state_session();
        TEST_ASSERT_EQUAL(APP_STATE_OK,
            app_state_deliver(APP_EVENT_DISCONNECTED, APP_OWNER_NETWORK));
        TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
        TEST_ASSERT_FALSE(app_state_is_online());
        TEST_ASSERT_TRUE(app_state_retry_pending());
        TEST_ASSERT_EQUAL(session_before, app_state_session());

        /* Backoff elapses: the machine returns to the network gate.  The
         * per-episode budget was reset when online was reached, so each
         * cycle starts with a fresh budget of one used attempt. */
        s_now_ms += APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS;
        TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
        TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
        TEST_ASSERT_EQUAL(1u, app_state_retry_attempts_used());

        /* Re-gate: Wi-Fi, TLS, sync. */
        TEST_ASSERT_EQUAL(APP_STATE_OK,
            app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
        TEST_ASSERT_EQUAL(APP_STATE_OK,
            app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
        TEST_ASSERT_EQUAL(APP_STATE_OK,
            app_state_deliver(APP_EVENT_SYNC_COMPLETE, APP_OWNER_THINGSBOARD));
        TEST_ASSERT_EQUAL(APP_STATE_ONLINE, app_state_current());
        TEST_ASSERT_TRUE(app_state_is_online());

        /* Online resets the retry budget for the next outage. */
        TEST_ASSERT_EQUAL(0u, app_state_retry_attempts_used());
        TEST_ASSERT_FALSE(app_state_retry_pending());
        TEST_ASSERT_FALSE(app_state_retry_exhausted());
    }
}

static void test_retry_backoff_exponential_bounded_and_parks(void)
{
    /* Custom tight schedule: 1s initial, 4s cap, 3 retries max, 2x. */
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        init_with_retry_cfg(1000u, 4000u, 3u, 2u));
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));

    /* Failure 1: 1s backoff. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_FAILED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_TRUE(app_state_retry_pending());
    TEST_ASSERT_EQUAL(1000u, app_state_retry_delay_ms());
    TEST_ASSERT_EQUAL(0u, app_state_retry_attempts_used());

    /* Retry 1 at 1000 ms; fail again -> 2s backoff. */
    s_now_ms = 1000u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
    TEST_ASSERT_EQUAL(1u, app_state_retry_attempts_used());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_FAILED, APP_OWNER_NETWORK));
    TEST_ASSERT_TRUE(app_state_retry_pending());
    TEST_ASSERT_EQUAL(2000u, app_state_retry_delay_ms());

    /* Retry 2 at 3000 ms; fail again -> capped 4s backoff. */
    s_now_ms = 3000u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(2u, app_state_retry_attempts_used());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_FAILED, APP_OWNER_NETWORK));
    TEST_ASSERT_TRUE(app_state_retry_pending());
    TEST_ASSERT_EQUAL(4000u, app_state_retry_delay_ms());

    /* Retry 3 at 7000 ms; the following failure exhausts the budget. */
    s_now_ms = 7000u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(3u, app_state_retry_attempts_used());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_FAILED, APP_OWNER_NETWORK));

    /* Exhausted: parked, nothing scheduled, no retry storm. */
    TEST_ASSERT_TRUE(app_state_retry_exhausted());
    TEST_ASSERT_FALSE(app_state_retry_pending());
    TEST_ASSERT_EQUAL(0u, app_state_retry_delay_ms());
    int retries_fired = obs_count_where_event(APP_EVENT_RETRY_DUE);

    /* Continued polling must NOT produce another retry. */
    int iteration;
    for (iteration = 0; iteration < 5; ++iteration)
    {
        s_now_ms += 4000u;
        TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
        TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
        TEST_ASSERT_EQUAL(3u, app_state_retry_attempts_used());
    }
    TEST_ASSERT_EQUAL(retries_fired, obs_count_where_event(APP_EVENT_RETRY_DUE));

    /* A repeated failure report while parked is dropped as illegal (and
     * still fails off), not re-armed. */
    unsigned forces_before = lamp_mock_force_inactive_calls();
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_NETWORK_FAILED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(forces_before + 1u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_FALSE(app_state_retry_pending());

    /* External reset is the recovery: boot, full re-gate, online, and a
     * fresh retry budget. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_RESET, APP_OWNER_EXTERNAL));
    TEST_ASSERT_EQUAL(APP_STATE_BOOT, app_state_current());
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_SYNC_COMPLETE, APP_OWNER_THINGSBOARD));
    TEST_ASSERT_TRUE(app_state_is_online());
    TEST_ASSERT_EQUAL(0u, app_state_retry_attempts_used());
    TEST_ASSERT_FALSE(app_state_retry_exhausted());
}

/* --------------------------------------------------------------------- */
/* 5. Stale / illegal callbacks                                            */
/* --------------------------------------------------------------------- */

static void test_stale_callback_is_rejected_and_counted(void)
{
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));

    /* A Wi-Fi worker arms its callback under THIS session... */
    uint32_t armed_session = app_state_session();

    /* ...then the network fails and the machine retries (new session). */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_FAILED, APP_OWNER_NETWORK));
    s_now_ms = APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
    TEST_ASSERT_NOT_EQUAL(armed_session, app_state_session());

    /* The late connect callback from the previous session must be
     * rejected and counted; the machine stays at the network gate. */
    int transitions_before = s_obs_count;
    TEST_ASSERT_EQUAL(APP_STATE_ERR_STALE,
        app_state_deliver_session(APP_EVENT_NETWORK_CONNECTED,
                                  APP_OWNER_NETWORK, armed_session));
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
    TEST_ASSERT_EQUAL(transitions_before, s_obs_count);
    TEST_ASSERT_EQUAL(1u, app_state_stale_dropped());

    /* The current-session callback is still honored afterwards. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver_session(APP_EVENT_NETWORK_CONNECTED,
                                  APP_OWNER_NETWORK, app_state_session()));
    TEST_ASSERT_EQUAL(APP_STATE_TLS, app_state_current());
}

static void test_stale_disconnect_still_fails_off(void)
{
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    uint32_t armed_session = app_state_session();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_FAILED, APP_OWNER_NETWORK));
    s_now_ms = APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());

    /* A stale DISCONNECTED from the previous session: dropped, but the
     * safety fail-off is performed BEFORE the drop. */
    unsigned forces_before = lamp_mock_force_inactive_calls();
    TEST_ASSERT_EQUAL(APP_STATE_ERR_STALE,
        app_state_deliver_session(APP_EVENT_DISCONNECTED, APP_OWNER_NETWORK,
                                  armed_session));
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
    TEST_ASSERT_EQUAL(forces_before + 1u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL(1u, app_state_stale_dropped());
}

static void test_wrong_owner_and_illegal_event_rejected(void)
{
    boot_to_filesystem();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));

    /* Wrong owner for NETWORK_CONNECTED: dropped, no transition, no
     * fail-off (not a disconnect-class event). */
    int transitions_before = s_obs_count;
    unsigned forces_before = lamp_mock_force_inactive_calls();
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
    TEST_ASSERT_EQUAL(transitions_before, s_obs_count);
    TEST_ASSERT_EQUAL(forces_before, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL(1u, app_state_invalid_dropped());

    /* Event not legal for the current state: dropped. */
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
    TEST_ASSERT_EQUAL(2u, app_state_invalid_dropped());

    /* A disconnect-class event from the wrong context is dropped but
     * still fails off first. */
    forces_before = lamp_mock_force_inactive_calls();
    TEST_ASSERT_EQUAL(APP_STATE_ERR_ILLEGAL,
        app_state_deliver(APP_EVENT_DISCONNECTED, APP_OWNER_TIMER));
    TEST_ASSERT_EQUAL(APP_STATE_NETWORK, app_state_current());
    TEST_ASSERT_EQUAL(forces_before + 1u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL(3u, app_state_invalid_dropped());
}

/* --------------------------------------------------------------------- */
/* 6. Watchdog                                                            */
/* --------------------------------------------------------------------- */

static void test_watchdog_expiry_on_starved_feed(void)
{
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, init_with_wdt(1000u));
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());

    /* The owner task stalls (simulated: no poll, no transition) past the
     * 1000 ms window; the first poll detects the expiry. */
    TEST_ASSERT_EQUAL(1u, lamp_mock_force_inactive_calls());
    s_now_ms = 1001u;
    TEST_ASSERT_EQUAL(APP_STATE_ERR_WATCHDOG, app_state_poll());

    TEST_ASSERT_EQUAL(APP_STATE_FATAL, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(1, s_wdt_expired_calls);
    /* Explicit fail-off + the FATAL entry's fail-off. */
    TEST_ASSERT_EQUAL(3u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_FALSE(app_state_retry_pending());

    /* No spontaneous recovery: later polls keep the machine FATAL and do
     * not fire the callback again. */
    s_now_ms += 10000u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_FATAL, app_state_current());
    TEST_ASSERT_EQUAL(1, s_wdt_expired_calls);
}

static void test_watchdog_poll_keeps_running_machine_alive(void)
{
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, init_with_wdt(1000u));
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());

    /* Regular polls within the window never trigger the watchdog. */
    uint32_t t;
    for (t = 0u; t <= 4900u; t += 100u)
    {
        s_now_ms = t;
        TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
        TEST_ASSERT_NOT_EQUAL(APP_STATE_FATAL, app_state_current());
    }
    TEST_ASSERT_EQUAL(0, s_wdt_expired_calls);

    /* Starve it once: the next poll (2 s later) detects the expiry. */
    s_now_ms = 6900u;
    TEST_ASSERT_EQUAL(APP_STATE_ERR_WATCHDOG, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_FATAL, app_state_current());
    TEST_ASSERT_EQUAL(1, s_wdt_expired_calls);
}

static void test_watchdog_exact_boundary_and_poll_at_api(void)
{
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, init_with_wdt(1000u));
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());

    /* The window is a ">= timeout" boundary: a poll at exactly
     * deadline + timeout (start() refreshed at t=0, poll at t=1000) has
     * not been fed within the window and expires. */
    TEST_ASSERT_EQUAL(APP_STATE_ERR_WATCHDOG, app_state_poll_at(1000u));
    TEST_ASSERT_EQUAL(APP_STATE_FATAL, app_state_current());
}

static void test_watchdog_transitions_refresh_deadline(void)
{
    /* Forward progress (transitions) refreshes the deadline, so a busy
     * boot sequence does not trip the watchdog. */
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, init_with_wdt(1000u));
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());

    s_now_ms = 300u;
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    s_now_ms = 500u;
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));

    /* Without the transition feed the deadline would have been 500 ms
     * stale -> 900 ms later (t=1400) the machine would have expired; the
     * transitions refreshed it, so the poll is still fine. */
    s_now_ms = 1400u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_NOT_EQUAL(APP_STATE_FATAL, app_state_current());

    /* Now starve it past the window (no poll, no transition): the next
     * poll detects the expiry. */
    s_now_ms = 2501u;
    TEST_ASSERT_EQUAL(APP_STATE_ERR_WATCHDOG, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_FATAL, app_state_current());
}

static void test_watchdog_recovery_after_reset(void)
{
    drive_online();

    /* Starve the feed: watchdog expiry while online fails everything off. */
    s_now_ms = 30001u; /* > 30 s default window since the drives at t=0 */
    TEST_ASSERT_EQUAL(APP_STATE_ERR_WATCHDOG, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_FATAL, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(1, s_wdt_expired_calls);

    /* Recovery is explicit: an external reset -> boot -> full re-gate. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_RESET, APP_OWNER_EXTERNAL));
    TEST_ASSERT_EQUAL(APP_STATE_BOOT, app_state_current());
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_SYNC_COMPLETE, APP_OWNER_THINGSBOARD));
    TEST_ASSERT_TRUE(app_state_is_online());

    /* The recovered machine feeds normally: no second expiry. */
    TEST_ASSERT_EQUAL(1, s_wdt_expired_calls);
    s_now_ms += 5u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_TRUE(app_state_is_online());
}

static void test_watchdog_clears_pending_retry(void)
{
    /* Use a short watchdog so an owner-task stall is detectable while a
     * retry is already scheduled (due in the default 2 s backoff). */
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, init_with_wdt(1000u));
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_FAILED, APP_OWNER_NETWORK));
    TEST_ASSERT_TRUE(app_state_retry_pending());

    /* The owner task stalls (no poll) past its 1 s window; the watchdog
     * fires BEFORE the 2 s retry deadline would ever run, and clears the
     * schedule instead of letting the retry fire after a fault. */
    s_now_ms = 1500u;
    TEST_ASSERT_EQUAL(APP_STATE_ERR_WATCHDOG, app_state_poll());
    TEST_ASSERT_EQUAL(APP_STATE_FATAL, app_state_current());
    TEST_ASSERT_FALSE(app_state_retry_pending());
    TEST_ASSERT_EQUAL(0u, app_state_retry_delay_ms());
    TEST_ASSERT_EQUAL(0u, app_state_retry_attempts_used());
}

/* --------------------------------------------------------------------- */
/* 7. OTA entry/exit                                                      */
/* --------------------------------------------------------------------- */

static void test_ota_entry_and_exit_to_boot_then_online(void)
{
    drive_online();
    uint32_t session_online = app_state_session();

    /* OTA begins: output off, new session, watchdog keeps running. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_OTA_BEGIN, APP_OWNER_OTA));
    TEST_ASSERT_EQUAL(APP_STATE_OTA, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(6u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_EQUAL(session_online + 1u, app_state_session());

    /* While OTA runs, ThingsBoard callbacks from the old session are
     * stale and cannot move the machine. */
    TEST_ASSERT_EQUAL(APP_STATE_ERR_STALE,
        app_state_deliver_session(APP_EVENT_DISCONNECTED, APP_OWNER_MQTT,
                                  session_online));
    TEST_ASSERT_EQUAL(APP_STATE_OTA, app_state_current());

    /* OTA completes: return to boot, new session. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_OTA_END, APP_OWNER_OTA));
    TEST_ASSERT_EQUAL(APP_STATE_BOOT, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(session_online + 2u, app_state_session());

    /* A fresh boot episode reaches online again. */
    uint32_t session_after_ota = app_state_session();
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_SYNC_COMPLETE, APP_OWNER_THINGSBOARD));
    TEST_ASSERT_TRUE(app_state_is_online());
    TEST_ASSERT_EQUAL(session_after_ota + 1u, app_state_session());
}

static void test_ota_failure_degrades_to_safe_off(void)
{
    drive_online();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_OTA_BEGIN, APP_OWNER_OTA));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_OTA_FAILED, APP_OWNER_OTA));

    /* OTA failure is an error path: the output stays off, the device
     * degrades (no automatic retry of the OTA). */
    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
    TEST_ASSERT_EQUAL(7u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_FALSE(app_state_retry_pending());
    TEST_ASSERT_FALSE(app_state_retry_exhausted());
}

static void test_ota_begin_allowed_from_safe_off(void)
{
    drive_online();
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_DISCONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_SAFE_OFF, app_state_current());

    /* A degraded unit must be updatable. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_OTA_BEGIN, APP_OWNER_OTA));
    TEST_ASSERT_EQUAL(APP_STATE_OTA, app_state_current());
    TEST_ASSERT_FALSE(app_state_is_online());
}

/* --------------------------------------------------------------------- */
/* 8. Session identity                                                     */
/* --------------------------------------------------------------------- */

static void test_session_bumps_on_episode_boundaries_only(void)
{
    s_now_ms = 0u;
    TEST_ASSERT_EQUAL(APP_STATE_OK, init_with_wdt(30000u));

    TEST_ASSERT_EQUAL(0u, app_state_session());
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(1u, app_state_session());

    /* Success events inside one episode do NOT bump the session. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_CONFIG_OK, APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_NETWORK_CONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT));
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_SYNC_COMPLETE, APP_OWNER_THINGSBOARD));
    TEST_ASSERT_EQUAL(1u, app_state_session());

    /* A failure (no bump while parking), then the RETRY bumps. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_DISCONNECTED, APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL(1u, app_state_session());
    s_now_ms = APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS;
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_poll());
    TEST_ASSERT_EQUAL(2u, app_state_session());

    /* RESET bumps; the following start() bumps again. */
    TEST_ASSERT_EQUAL(APP_STATE_OK,
        app_state_deliver(APP_EVENT_RESET, APP_OWNER_EXTERNAL));
    TEST_ASSERT_EQUAL(3u, app_state_session());
    TEST_ASSERT_EQUAL(APP_STATE_OK, app_state_start());
    TEST_ASSERT_EQUAL(4u, app_state_session());
}

/* --------------------------------------------------------------------- */
/* main()                                                                 */
/* --------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    /* 1. API contract / lifecycle */
    RUN_TEST(test_init_rejects_null_config);
    RUN_TEST(test_ops_before_init_return_not_initialized);
    RUN_TEST(test_double_init_and_restart);
    RUN_TEST(test_start_rejects_when_not_in_boot);
    RUN_TEST(test_non_deliverable_events_are_illegal);

    /* 2. Boot sequence */
    RUN_TEST(test_happy_path_reaches_online_only_after_all_gates);
    RUN_TEST(test_every_non_online_state_forbids_output);

    /* 3. Failure transitions (fault injection) */
    RUN_TEST(test_fs_failure_transition_degrades);
    RUN_TEST(test_config_failure_transition_degrades);
    RUN_TEST(test_network_failure_transition_retries_network);
    RUN_TEST(test_network_connect_timeout_fault_injection);
    RUN_TEST(test_tls_failure_transition_retries_tls);
    RUN_TEST(test_sync_failure_transition_retries_sync);
    RUN_TEST(test_sync_timeout_fault_injection);
    RUN_TEST(test_invalid_state_from_sync_retries_sync);
    RUN_TEST(test_disconnect_during_boot_stages_fails_off);
    RUN_TEST(test_disconnect_while_online_fails_off);
    RUN_TEST(test_disconnect_from_mqtt_context_while_online_fails_off);
    RUN_TEST(test_invalid_state_while_online_fails_off);
    RUN_TEST(test_fatal_from_any_state);
    RUN_TEST(test_lamp_failoff_failure_does_not_wedge);

    /* 3b. Provisioning gate (TASK-125) */
    RUN_TEST(test_provisioning_legal_entry_and_exit);
    RUN_TEST(test_provisioning_failure_degrades_and_retries_network);
    RUN_TEST(test_provisioning_started_illegal_from_every_other_state);
    RUN_TEST(test_provisioning_retry_exhaustion_parks_in_safe_off);
    RUN_TEST(test_provisioning_stale_session_delivery_rejected);

    /* 4. Reconnect loop and bounded retry (anti-storm) */
    RUN_TEST(test_reconnect_loop_reaches_online_and_resets_budget);
    RUN_TEST(test_retry_backoff_exponential_bounded_and_parks);

    /* 5. Stale / illegal callbacks */
    RUN_TEST(test_stale_callback_is_rejected_and_counted);
    RUN_TEST(test_stale_disconnect_still_fails_off);
    RUN_TEST(test_wrong_owner_and_illegal_event_rejected);

    /* 6. Watchdog */
    RUN_TEST(test_watchdog_expiry_on_starved_feed);
    RUN_TEST(test_watchdog_poll_keeps_running_machine_alive);
    RUN_TEST(test_watchdog_exact_boundary_and_poll_at_api);
    RUN_TEST(test_watchdog_transitions_refresh_deadline);
    RUN_TEST(test_watchdog_recovery_after_reset);
    RUN_TEST(test_watchdog_clears_pending_retry);

    /* 7. OTA entry/exit */
    RUN_TEST(test_ota_entry_and_exit_to_boot_then_online);
    RUN_TEST(test_ota_failure_degrades_to_safe_off);
    RUN_TEST(test_ota_begin_allowed_from_safe_off);

    /* 8. Session identity */
    RUN_TEST(test_session_bumps_on_episode_boundaries_only);

    return UNITY_END();
}