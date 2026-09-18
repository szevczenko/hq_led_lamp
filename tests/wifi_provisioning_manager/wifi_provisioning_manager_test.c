/**
 * @file wifi_provisioning_manager_test.c
 * @brief Host unit tests for the product Wi-Fi provisioning adapter
 *        (TASK-126)
 *
 * The production adapter (components/wifi_provisioning_manager) is compiled
 * against test-only doubles for every platform boundary:
 *
 *   - wifi_provisioning_mock.c   — the platform provisioning application
 *     (wifi_http_provisioning.h): idempotent lifecycle, runtime URL
 *     overrides, failure injection, a credential "mock connect path" for
 *     the log-content regression and a start/stop concurrency watermark,
 *   - wifi_provisioning_controller_mock.c — the platform automatic fallback
 *     controller (wifi_provisioning_controller.h, TASK-132): records the
 *     notification hook the adapter registers and lets tests fire
 *     state-change notifications transition by transition with a chosen
 *     lifecycle session token,
 *   - mongoose_process_mock.c    — the shared Mongoose process
 *     (mongoose_process.h): running-state query and Init/Deinit/Invoke
 *     counters proving the adapter never tears the process down and never
 *     schedules work on its poll thread,
 *   - wifi_mgmt_mock.c           — the Wi-Fi manager saved-credentials query
 *     (wifi_mgmt_is_read_data),
 *   - osal_test_support.c        — real pthread mutexes (same shape as the
 *     platform POSIX backend) and a captured log ring that the log-content
 *     regression inspects.
 *
 * The adapter is compiled with WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY
 * so the runtime listen-URL overrides are exercised (on the target build
 * they are compiled out and the compiled-in default URLs are used).
 *
 * Coverage: init/deinit lifecycle, start/stop lifecycle + idempotency,
 * start before Mongoose is running, stop with the portal already stopped,
 * start/stop failure propagation, listener-URL override behavior, the
 * is-portal-active and has-saved-credentials queries, deinit tearing down
 * an active portal, concurrent start/stop serialization, the controller
 * notification translation (TASK-132: hook registration with the platform
 * fallback-budget default untouched, an event stored for each transition,
 * stale-session notifications discarded across deinit/re-init, poll-event
 * ordering STARTED then SUCCEEDED, portal start failure -> FAILED) and the
 * platform start-status mapping (TASK-134: every documented platform
 * failure mode maps to its distinct adapter status, success maps to OK,
 * and is_active() is true only when the whole portal (AP + both listeners)
 * is genuinely reachable) and the credentials-never-logged secrecy rule.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mongoose_process_mock.h"
#include "osal_test_support.h"
#include "unity.h"
#include "wifi_mgmt_mock.h"
#include "wifi_provisioning_controller_mock.h"
#include "wifi_provisioning_manager.h"
#include "wifi_provisioning_mock.h"

/* --------------------------------------------------------------------- */
/* Fixtures                                                               */
/* --------------------------------------------------------------------- */

void setUp(void)
{
    wifi_provisioning_mock_reset();
    wifi_provisioning_controller_mock_reset();
    mongoose_process_mock_reset();
    wifi_mgmt_mock_reset();
    osal_test_log_reset();

    /* Default environment: the shared Mongoose process is running and a
     * start may proceed past the adapter precondition. */
    mongoose_process_mock_set_running(true);
}

void tearDown(void)
{
    /* Leave a clean adapter behind: an active portal is stopped, the
     * adapter is deinitialized.  tearDown must never fail the suite. */
    (void)wifi_provisioning_manager_deinit();
    wifi_provisioning_mock_reset();
    wifi_provisioning_controller_mock_reset();
    mongoose_process_mock_reset();
    wifi_mgmt_mock_reset();
    osal_test_log_reset();
}

/* --------------------------------------------------------------------- */
/* Adapter lifecycle                                                      */
/* --------------------------------------------------------------------- */

static void test_init_deinit_lifecycle(void)
{
    /* Safe defaults before init. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_STOPPED,
                          wifi_provisioning_manager_get_state());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_has_saved_credentials());

    /* start/stop before init are rejected cleanly. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED,
                          wifi_provisioning_manager_stop());
    wifi_provisioning_mock_counters_t before =
        wifi_provisioning_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(0u, before.start_calls);
    TEST_ASSERT_EQUAL_UINT(0u, before.stop_calls);

    /* init is idempotent. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* deinit is idempotent. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_deinit());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_deinit());

    /* After deinit the adapter reports safe defaults again. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_STOPPED,
                          wifi_provisioning_manager_get_state());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
}

/* --------------------------------------------------------------------- */
/* Start/stop lifecycle and idempotency                                   */
/* --------------------------------------------------------------------- */

static void test_start_stop_lifecycle_and_idempotency(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* First start: the platform application is invoked once and the portal
     * reaches RUNNING. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_TRUE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_RUNNING,
                          wifi_provisioning_manager_get_state());

    wifi_provisioning_mock_counters_t after_start =
        wifi_provisioning_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(1u, after_start.start_calls);
    TEST_ASSERT_EQUAL_UINT(0u, after_start.stop_calls);

    /* The adapter never initializes/deinitializes the shared Mongoose
     * process and never schedules its own work on the poll thread. */
    mongoose_process_mock_counters_t mg =
        mongoose_process_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(0u, mg.init_calls);
    TEST_ASSERT_EQUAL_UINT(0u, mg.deinit_calls);
    TEST_ASSERT_EQUAL_UINT(0u, mg.invoke_calls);
    TEST_ASSERT_TRUE(mg.is_running_calls > 0u);

    /* Second start while running: idempotent no-op (state unchanged). */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_TRUE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_RUNNING,
                          wifi_provisioning_manager_get_state());
    TEST_ASSERT_EQUAL_UINT(2u,
                           wifi_provisioning_mock_get_counters().start_calls);

    /* Stop brings the portal down.  The stop also ends the controller's
     * portal lifecycle (TASK-133: grace timer cancelled, AP retirement
     * requested) before the belt-and-braces listener close. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_STOPPED,
                          wifi_provisioning_manager_get_state());
    TEST_ASSERT_EQUAL_UINT(1u,
                           wifi_provisioning_mock_get_counters().stop_calls);
    wifi_provisioning_controller_mock_counters_t ctrl_after_stop =
        wifi_provisioning_controller_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(1u, ctrl_after_stop.stop_calls);

    /* Still not initialized/deinitialized the process even after stop. */
    mg = mongoose_process_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(0u, mg.init_calls);
    TEST_ASSERT_EQUAL_UINT(0u, mg.deinit_calls);

    /* Second stop while stopped: idempotent no-op (controller and listeners
     * are both already down). */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_EQUAL_UINT(2u,
                           wifi_provisioning_mock_get_counters().stop_calls);
    ctrl_after_stop = wifi_provisioning_controller_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(2u, ctrl_after_stop.stop_calls);
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
}

static void test_stop_with_portal_already_stopped(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* A stop on a portal that never started and a repeat stop are both
     * successful no-ops.  The controller stop is likewise a safe no-op for
     * a disabled controller (credentialed device with no active portal). */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_EQUAL_UINT(2u,
                           wifi_provisioning_mock_get_counters().stop_calls);
    TEST_ASSERT_EQUAL_UINT(2u,
        wifi_provisioning_controller_mock_get_counters().stop_calls);
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_STOPPED,
                          wifi_provisioning_manager_get_state());
}

static void test_stop_ends_controller_lifecycle_and_closes_listeners(void)
{
    /* TASK-133 stop path: on OTA entry / FATAL the adapter stop must end
     * the controller's portal lifecycle (grace timer cancelled, AP retired)
     * in addition to closing the portal listeners. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* Even with no portal listeners up the controller stop is always
     * issued: a pending grace timer or a recoverable portal must never
     * survive an explicit stop. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_EQUAL_UINT(1u,
        wifi_provisioning_controller_mock_get_counters().stop_calls);
    TEST_ASSERT_EQUAL_UINT(1u,
                           wifi_provisioning_mock_get_counters().stop_calls);

    /* A controller that cannot complete the retirement (reports false,
     * stays recoverable) does not fail the adapter stop: the belt-and-
     * braces listener close still guarantees no portal listener is left
     * on the shared Mongoose process. */
    wifi_provisioning_controller_mock_set_state(
        WIFI_PROVISIONING_CONTROLLER_PROVISIONING);
    wifi_provisioning_controller_mock_set_stop_result(false);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_EQUAL_UINT(2u,
        wifi_provisioning_controller_mock_get_counters().stop_calls);
    TEST_ASSERT_EQUAL_UINT(2u,
                           wifi_provisioning_mock_get_counters().stop_calls);
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());

    /* The controller stop never stores a provisioning outcome (DISABLED is
     * not a product event), and it does not disturb the notification hook
     * (a later deinit still ends the lifecycle). */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_deinit());
    TEST_ASSERT_EQUAL_UINT(1u,
        wifi_provisioning_controller_mock_get_counters().deinit_calls);
}

/* --------------------------------------------------------------------- */
/* Precondition: shared Mongoose process running                          */
/* --------------------------------------------------------------------- */

static void test_start_before_mongoose_running(void)
{
    mongoose_process_mock_set_running(false);

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    TEST_ASSERT_EQUAL_INT(
        WIFI_PROVISIONING_MANAGER_ERR_MONGOOSE_NOT_RUNNING,
        wifi_provisioning_manager_start());

    /* The platform application must not have been touched. */
    wifi_provisioning_mock_counters_t counters =
        wifi_provisioning_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(0u, counters.start_calls);
    TEST_ASSERT_EQUAL_UINT(0u, counters.stop_calls);
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_STOPPED,
                          wifi_provisioning_manager_get_state());

    /* Once the shared process is up the same start succeeds. */
    mongoose_process_mock_set_running(true);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_TRUE(wifi_provisioning_manager_is_active());
}

/* --------------------------------------------------------------------- */
/* Failure propagation                                                    */
/* --------------------------------------------------------------------- */

static void test_start_failure_propagates_and_recovers(void)
{
    wifi_provisioning_mock_config_t fail = {.fail_start = true};
    wifi_provisioning_mock_set_config(&fail);

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_ERR_START_FAILED,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_ERROR,
                          wifi_provisioning_manager_get_state());

    /* A later start from the platform ERROR state succeeds (the platform
     * treats ERROR as a valid start point; the adapter must not keep a
     * sticky failure). */
    wifi_provisioning_mock_config_t ok = {.fail_start = false};
    wifi_provisioning_mock_set_config(&ok);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_TRUE(wifi_provisioning_manager_is_active());
}

/* --------------------------------------------------------------------- */
/* Platform start-status mapping (TASK-134)                          */
/* --------------------------------------------------------------------- */

/**
 * @brief Drive one platform start SUCCESS through the adapter.
 *
 * The platform result (WIFI_HTTP_PROVISIONING_START_OK on a fresh start or
 * its idempotent ALREADY_RUNNING double) must map to the product OK; no
 * FAILED outcome is fabricated.  Ends with the portal stopped so every
 * case starts from a clean non-running portal.
 */
static void assert_platform_start_success(
    wifi_http_provisioning_start_status_t platform_status)
{
    wifi_provisioning_mock_config_t cfg = {0};

    if (platform_status == WIFI_HTTP_PROVISIONING_START_ALREADY_RUNNING)
    {
        /* Idempotent repeat: put the portal up first (the mock naturally
         * reports ALREADY_RUNNING on the repeat start, mirroring the
         * platform), then exercise the adapter's ALREADY_RUNNING -> OK
         * mapping. */
        TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                              wifi_provisioning_manager_start());
    }
    else
    {
        /* Fresh start: inject the requested platform result for start_ex. */
        cfg.start_status_set = true;
        cfg.start_status = platform_status;
    }
    wifi_provisioning_mock_set_config(&cfg);

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_TRUE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());

    /* Stop so every case starts from a clean non-running portal. */
    wifi_provisioning_mock_config_t ok = {0};
    wifi_provisioning_mock_set_config(&ok);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
}

/**
 * @brief Drive one platform start FAILURE through the adapter.
 *
 * Asserts the mapped product status is EXACTLY the distinct adapter code for
 * that failure mode, the FAILED outcome is recorded for the supervisor, and
 * the adapter leaves no sticky failure (the next clean start succeeds; ERROR
 * is a valid platform start point).
 */
static void assert_platform_start_failure(
    wifi_http_provisioning_start_status_t platform_status,
    wifi_provisioning_manager_status_t expected)
{
    wifi_provisioning_mock_config_t cfg = {.start_status_set = true,
                                            .start_status = platform_status};
    wifi_provisioning_mock_set_config(&cfg);

    TEST_ASSERT_EQUAL_INT(expected, wifi_provisioning_manager_start());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_ERROR,
                          wifi_provisioning_manager_get_state());

    /* The supervisor still gets a FAILED outcome for any start failure. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_FAILED,
                          wifi_provisioning_manager_poll_event());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());

    /* Recovery: with the failure injection cleared the next start succeeds
     * (no sticky adapter failure). */
    wifi_provisioning_mock_config_t ok = {0};
    wifi_provisioning_mock_set_config(&ok);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_TRUE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());

    /* Stop so every failure case starts from a clean non-running portal. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
}

static void test_platform_start_status_mapping(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* Success: the fresh start OK and the idempotent ALREADY_RUNNING result
     * both mean "the portal is (or already was) up" -> product OK. */
    assert_platform_start_success(WIFI_HTTP_PROVISIONING_START_OK);
    assert_platform_start_success(
        WIFI_HTTP_PROVISIONING_START_ALREADY_RUNNING);

    /* Every documented platform failure mode maps to its distinct adapter
     * status: the supervisor can distinguish "portal up and reachable" from
     * exactly which start step failed. */
    assert_platform_start_failure(
        WIFI_HTTP_PROVISIONING_START_ERR_DEPENDENCY,
        WIFI_PROVISIONING_MANAGER_ERR_DEPENDENCY);
    assert_platform_start_failure(
        WIFI_HTTP_PROVISIONING_START_ERR_MODE_TRANSITION,
        WIFI_PROVISIONING_MANAGER_ERR_MODE_TRANSITION);
    assert_platform_start_failure(
        WIFI_HTTP_PROVISIONING_START_ERR_HTTP_BIND,
        WIFI_PROVISIONING_MANAGER_ERR_HTTP_BIND);
    assert_platform_start_failure(
        WIFI_HTTP_PROVISIONING_START_ERR_DNS_BIND,
        WIFI_PROVISIONING_MANAGER_ERR_DNS_BIND);
    assert_platform_start_failure(
        WIFI_HTTP_PROVISIONING_START_ERR_NO_AP,
        WIFI_PROVISIONING_MANAGER_ERR_AP_NOT_UP);

    /* Defensive default: an undocumented/unknown platform status maps to
     * the generic ERR_START_FAILED (also exercised by the legacy fail_start
     * fixture above). */
    assert_platform_start_failure(
        (wifi_http_provisioning_start_status_t)0x7E,
        WIFI_PROVISIONING_MANAGER_ERR_START_FAILED);
}

static void test_stop_failure_propagates(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());

    wifi_provisioning_mock_config_t fail = {.fail_stop = true};
    wifi_provisioning_mock_set_config(&fail);

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_ERROR,
                          wifi_provisioning_manager_get_state());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
}

/* --------------------------------------------------------------------- */
/* Listener-URL override behavior (test/host build only)                  */
/* --------------------------------------------------------------------- */

static void test_listener_url_overrides_applied_before_start(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    TEST_ASSERT_TRUE(wifi_provisioning_manager_set_http_url(
        "http://127.0.0.1:8080"));
    TEST_ASSERT_TRUE(
        wifi_provisioning_manager_set_dns_url("udp://127.0.0.1:10053"));

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_TRUE(wifi_provisioning_manager_is_active());

    wifi_provisioning_mock_counters_t counters =
        wifi_provisioning_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(1u, counters.set_http_url_calls);
    TEST_ASSERT_EQUAL_UINT(1u, counters.set_dns_url_calls);
    TEST_ASSERT_EQUAL_UINT(1u, counters.set_http_before_start);
    TEST_ASSERT_EQUAL_UINT(1u, counters.set_dns_before_start);
    TEST_ASSERT_EQUAL_STRING("http://127.0.0.1:8080", counters.last_http_url);
    TEST_ASSERT_EQUAL_STRING("udp://127.0.0.1:10053", counters.last_dns_url);
}

static void test_no_overrides_uses_compiled_in_defaults(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* No override configured: the adapter hands the platform nothing and
     * the platform uses its compiled-in default listen URLs. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());

    wifi_provisioning_mock_counters_t counters =
        wifi_provisioning_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(0u, counters.set_http_url_calls);
    TEST_ASSERT_EQUAL_UINT(0u, counters.set_dns_url_calls);
    TEST_ASSERT_TRUE(wifi_provisioning_manager_is_active());
}

static void test_invalid_url_overrides_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* NULL, empty and over-long URLs are rejected. */
    TEST_ASSERT_FALSE(wifi_provisioning_manager_set_http_url(NULL));
    TEST_ASSERT_FALSE(wifi_provisioning_manager_set_http_url(""));
    TEST_ASSERT_FALSE(wifi_provisioning_manager_set_dns_url(NULL));
    TEST_ASSERT_FALSE(wifi_provisioning_manager_set_dns_url(""));

    {
        char long_url[200];
        memset(long_url, 'h', sizeof(long_url) - 1u);
        long_url[sizeof(long_url) - 1u] = '\0';
        TEST_ASSERT_FALSE(wifi_provisioning_manager_set_http_url(long_url));
        TEST_ASSERT_FALSE(wifi_provisioning_manager_set_dns_url(long_url));
    }

    /* After deinit the setters are rejected too (adapter not initialized). */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_deinit());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_set_http_url(
        "http://127.0.0.1:8080"));
    TEST_ASSERT_FALSE(
        wifi_provisioning_manager_set_dns_url("udp://127.0.0.1:10053"));

    /* The rejected values never reached the platform. */
    wifi_provisioning_mock_counters_t counters =
        wifi_provisioning_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(0u, counters.set_http_url_calls);
    TEST_ASSERT_EQUAL_UINT(0u, counters.set_dns_url_calls);
}

/* --------------------------------------------------------------------- */
/* Queries                                                                */
/* --------------------------------------------------------------------- */

static void test_is_portal_active_query(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_STOPPED,
                          wifi_provisioning_manager_get_state());

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_TRUE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_RUNNING,
                          wifi_provisioning_manager_get_state());

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_STOPPED,
                          wifi_provisioning_manager_get_state());
}

static void test_is_active_requires_fully_reachable_portal(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* Fresh start: the radio reached AP+STA and both listeners are bound,
     * so the whole portal is reachable and is_active() is true. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_TRUE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_RUNNING,
                          wifi_provisioning_manager_get_state());

    /* "Started without an AP": the platform state still reports RUNNING,
     * but the radio never reached AP+STA.  A bare state check would read
     * this as active; reachability says the portal is NOT up (TASK-134). */
    wifi_provisioning_mock_set_radio_up(false);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_RUNNING,
                          wifi_provisioning_manager_get_state());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());

    /* Radio restored: the whole portal is up again. */
    wifi_provisioning_mock_set_radio_up(true);
    TEST_ASSERT_TRUE(wifi_provisioning_manager_is_active());

    /* After stop nothing is up and the state returns to STOPPED. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_STOPPED,
                          wifi_provisioning_manager_get_state());
}

static void test_has_saved_credentials_query(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    TEST_ASSERT_FALSE(wifi_provisioning_manager_has_saved_credentials());
    wifi_mgmt_mock_set_saved_credentials(true);
    TEST_ASSERT_TRUE(wifi_provisioning_manager_has_saved_credentials());
    wifi_mgmt_mock_set_saved_credentials(false);
    TEST_ASSERT_FALSE(wifi_provisioning_manager_has_saved_credentials());

    /* Safe default after deinit. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_deinit());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_has_saved_credentials());
    /* The query reached the platform Wi-Fi manager while initialized. */
    TEST_ASSERT_TRUE(wifi_mgmt_mock_is_read_data_calls() >= 3u);
}

/* --------------------------------------------------------------------- */
/* Deinit teardown of an active portal                                    */
/* --------------------------------------------------------------------- */

static void test_deinit_stops_active_portal(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_TRUE(wifi_provisioning_manager_is_active());

    /* Deinit best-effort stops the portal (listeners only) and disarms the
     * adapter; the shared Mongoose process is left untouched. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_deinit());
    TEST_ASSERT_EQUAL_UINT(1u,
                           wifi_provisioning_mock_get_counters().stop_calls);
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_STOPPED,
                          wifi_provisioning_manager_get_state());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED,
                          wifi_provisioning_manager_stop());

    mongoose_process_mock_counters_t mg =
        mongoose_process_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(0u, mg.init_calls);
    TEST_ASSERT_EQUAL_UINT(0u, mg.deinit_calls);
}

/* --------------------------------------------------------------------- */
/* Concurrent start/stop serialization                                    */
/* --------------------------------------------------------------------- */

#define CONCURRENT_THREADS 8u
#define CONCURRENT_LOOPS   200u

static pthread_barrier_t s_gen_barrier;
static _Atomic unsigned  s_start_errors;
static _Atomic unsigned  s_stop_errors;

static void *start_stop_worker(void *arg)
{
    const bool start_first = (arg != NULL);

    (void)pthread_barrier_wait(&s_gen_barrier);
    for (unsigned i = 0U; i < CONCURRENT_LOOPS; ++i)
    {
        if (start_first)
        {
            if (wifi_provisioning_manager_start() !=
                WIFI_PROVISIONING_MANAGER_OK)
                atomic_fetch_add(&s_start_errors, 1u);
            if (wifi_provisioning_manager_stop() !=
                WIFI_PROVISIONING_MANAGER_OK)
                atomic_fetch_add(&s_stop_errors, 1u);
        }
        else
        {
            if (wifi_provisioning_manager_stop() !=
                WIFI_PROVISIONING_MANAGER_OK)
                atomic_fetch_add(&s_stop_errors, 1u);
            if (wifi_provisioning_manager_start() !=
                WIFI_PROVISIONING_MANAGER_OK)
                atomic_fetch_add(&s_start_errors, 1u);
        }
    }
    return NULL;
}

static void test_concurrent_start_stop_is_serialized(void)
{
    pthread_t threads[CONCURRENT_THREADS];

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());
    TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&s_gen_barrier, NULL,
                                                  CONCURRENT_THREADS));
    atomic_store(&s_start_errors, 0u);
    atomic_store(&s_stop_errors, 0u);

    for (unsigned t = 0U; t < CONCURRENT_THREADS; ++t)
    {
        TEST_ASSERT_EQUAL_INT(
            0, pthread_create(&threads[t], NULL, start_stop_worker,
                              (void *)(uintptr_t)(t & 1u)));
    }
    for (unsigned t = 0U; t < CONCURRENT_THREADS; ++t)
    {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(threads[t], NULL));
    }
    (void)pthread_barrier_destroy(&s_gen_barrier);

    /* Every start and every stop succeeded: with the adapter lock
     * serializing them, a start is always legal (or an idempotent no-op)
     * and so is a stop. */
    TEST_ASSERT_EQUAL_UINT(0u, atomic_load(&s_start_errors));
    TEST_ASSERT_EQUAL_UINT(0u, atomic_load(&s_stop_errors));

    /* The start/stop pair must never overlap: the adapter serializes the
     * whole platform start/stop transaction. */
    wifi_provisioning_mock_counters_t counters =
        wifi_provisioning_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(1u, counters.max_concurrency);

    /* Final state is coherent: is_active exactly mirrors the platform. */
    const bool platform_running =
        (wifi_http_provisioning_get_state() == WIFI_PROVISIONING_RUNNING);
    TEST_ASSERT_EQUAL_INT(platform_running,
                          wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(platform_running
                              ? WIFI_PROVISIONING_MANAGER_RUNNING
                              : WIFI_PROVISIONING_MANAGER_STOPPED,
                          wifi_provisioning_manager_get_state());

    /* Leave the world consistent for tearDown. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
}

/* --------------------------------------------------------------------- */
/* Log-content regression: credentials never logged                       */
/* --------------------------------------------------------------------- */

#define KNOWN_SSID     "SecretHomeWiFi_126"
#define KNOWN_PASSWORD "VerySecretPassword_126"

/* Count non-overlapping occurrences of @p needle in @p haystack.  Used
 * to prove the TASK-135 rate limit (exactly two wait lines) and the
 * once-per-portal-up-period reachable edge. */
static unsigned count_substrings(const char *haystack, const char *needle)
{
    unsigned count = 0u;
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL)
    {
        ++count;
        p += strlen(needle);
    }
    return count;
}

static void test_log_content_regression(void)
{
    /* Known credential content is present in the system (fed through the
     * mock connect path, exactly as the portal would receive it from a
     * submitted POST /api/v1/wifi/credentials). */
    wifi_provisioning_mock_submit_credentials(KNOWN_SSID, KNOWN_PASSWORD);
    TEST_ASSERT_EQUAL_STRING(KNOWN_SSID,
                             wifi_provisioning_mock_get_last_ssid());
    TEST_ASSERT_EQUAL_STRING(KNOWN_PASSWORD,
                             wifi_provisioning_mock_get_last_password());

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* Success transitions. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start()); /* idempotent */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop()); /* idempotent */

    /* Failure transitions (bind refusal) log error codes only. */
    wifi_provisioning_mock_config_t fail = {.fail_start = true};
    wifi_provisioning_mock_set_config(&fail);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_ERR_START_FAILED,
                          wifi_provisioning_manager_start());

    /* A distinct TASK-134 failure mode (HTTP bind refusal) must surface
     * with its mapped adapter error code in the log line. */
    wifi_provisioning_mock_config_t bind_cfg = {
        .start_status_set = true,
        .start_status      = WIFI_HTTP_PROVISIONING_START_ERR_HTTP_BIND};
    wifi_provisioning_mock_set_config(&bind_cfg);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_ERR_HTTP_BIND,
                          wifi_provisioning_manager_start());
    wifi_provisioning_mock_config_t ok = {.fail_start = false};
    wifi_provisioning_mock_set_config(&ok);

    /* Drain the two FAILED outcomes queued by the failure starts above:
     * the wait-path polls below must observe an empty adapter queue. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_FAILED,
                          wifi_provisioning_manager_poll_event());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_FAILED,
                          wifi_provisioning_manager_poll_event());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());

    /* Portal-up wait signature (TASK-135): the controller waits in
     * PROVISIONING with the portal up and reachable.  Start the portal,
     * put the controller into the provisioning-wait state and poll the
     * adapter's periodic pump (the supervisor cadence). */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    wifi_provisioning_controller_mock_set_state(
        WIFI_PROVISIONING_CONTROLLER_PROVISIONING);

    /* First poll fires the wait signature immediately (the clock starts at
     * 0 and the never-fired sentinel guarantees a prompt first line). */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());
    /* Two more polls inside the 30 s window add nothing. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());

    /* Advance the monotonic clock past the 30 s rate-limit window: only
     * then does the second wait line appear. */
    osal_test_set_time_ms(30000u + 1u);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());

    /* The portal is down again: re-arm the reachable edge guard. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());

    /* Queries and the provisioning decision hook. */
    (void)wifi_provisioning_manager_get_state();
    (void)wifi_provisioning_manager_is_active();
    wifi_mgmt_mock_set_saved_credentials(true);
    TEST_ASSERT_TRUE(wifi_provisioning_manager_has_saved_credentials());

    /* Captured log: the adapter logged sanitized transitions, but never any
     * credential content and never a provisioning URL.  The new TASK-135
     * signatures are present: the portal-reachable edge and the rate-limited
     * wait signature. */
    const char *log = osal_test_log_get();
    TEST_ASSERT_NOT_NULL(log);
    TEST_ASSERT_TRUE(strlen(log) > 0u);

    TEST_ASSERT_NULL(strstr(log, KNOWN_SSID));
    TEST_ASSERT_NULL(strstr(log, KNOWN_PASSWORD));
    /* URL schemes are never logged either (an embedded-credential URL like
     * http://user:pass@host would otherwise leak through a scheme match). */
    TEST_ASSERT_NULL(strstr(log, "http://"));
    TEST_ASSERT_NULL(strstr(log, "udp://"));
    TEST_ASSERT_NULL(strstr(log, "127.0.0.1"));

    /* Sanity: the sanitized adapter transitions WERE logged, including the
     * new portal-observability signatures. */
    TEST_ASSERT_NOT_NULL(strstr(log, "[prov_mgr]"));
    TEST_ASSERT_NOT_NULL(strstr(log, "provisioning AP up; portal reachable"));
    TEST_ASSERT_NOT_NULL(strstr(log,
        "provisioning portal start failed"));
    TEST_ASSERT_NOT_NULL(strstr(log, "adapter_status=-8")); /* ERR_HTTP_BIND */
    TEST_ASSERT_NOT_NULL(strstr(log, "portal up; station not yet connected"));
    TEST_ASSERT_NOT_NULL(strstr(log, "provisioning portal stopped"));

    /* Rate limit: the sanitized wait signature appears EXACTLY twice - the
     * first poll (t=0) and the poll after the 30 s window - while the two
     * intermediate polls (still within the window) added nothing. */
    TEST_ASSERT_EQUAL_UINT(2u,
        count_substrings(log, "portal up; station not yet connected"));
    /* The portal-reachable edge signature appears exactly once per
     * portal-up period: two portal periods in this test, two lines. */
    TEST_ASSERT_EQUAL_UINT(2u,
        count_substrings(log, "provisioning AP up; portal reachable"));
}

/* --------------------------------------------------------------------- */
/* Controller notification translation (TASK-132)                          */
/* --------------------------------------------------------------------- */

static void test_init_registers_controller_notification_hook(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* init() registered the product handler with the platform controller. */
    wifi_provisioning_controller_mock_counters_t ctrl =
        wifi_provisioning_controller_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(1u, ctrl.init_with_config_calls);
    TEST_ASSERT_NOT_NULL(ctrl.registered_cb);
    TEST_ASSERT_NOT_NULL(ctrl.registered_user_ctx);

    /* The platform fallback-budget default from TASK-131 is untouched: the
     * adapter hands the controller no per-device override. */
    TEST_ASSERT_FALSE(ctrl.config_fallback_budget_set);

    /* Idempotent init does not re-register the hook. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());
    ctrl = wifi_provisioning_controller_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(1u, ctrl.init_with_config_calls);

    /* deinit() ends the controller lifecycle. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_deinit());
    ctrl = wifi_provisioning_controller_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(1u, ctrl.deinit_calls);
    TEST_ASSERT_NULL(ctrl.registered_cb);
}

static void test_notification_stored_for_each_transition(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* The full provisioning lifecycle as the controller reports it: the
     * fallback entry into PROVISIONING stores STARTED, the grace/retire
     * completion (ONLINE) stores exactly one SUCCEEDED. */
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_DISABLED,
        WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT, 1u);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());

    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
        WIFI_PROVISIONING_CONTROLLER_PROVISIONING, 1u);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_STARTED,
                          wifi_provisioning_manager_poll_event());

    /* GRACE and RETIRING_AP are intermediate: no product event yet. */
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
        WIFI_PROVISIONING_CONTROLLER_GRACE, 1u);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_GRACE,
        WIFI_PROVISIONING_CONTROLLER_RETIRING_AP, 1u);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());

    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
        WIFI_PROVISIONING_CONTROLLER_ONLINE, 1u);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_SUCCEEDED,
                          wifi_provisioning_manager_poll_event());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());
}

static void test_saved_credential_connect_is_not_a_success(void)
{
    /* A credentialed device never enters PROVISIONING; its connect retires
     * the startup SoftAP through RETIRING_AP -> ONLINE.  That must NOT be
     * reported as a provisioning success. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_DISABLED,
        WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT, 1u);
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
        WIFI_PROVISIONING_CONTROLLER_RETIRING_AP, 1u);
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
        WIFI_PROVISIONING_CONTROLLER_ONLINE, 1u);

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());
}

static void test_stale_session_notification_discarded(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
        WIFI_PROVISIONING_CONTROLLER_PROVISIONING, 1u);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_STARTED,
                          wifi_provisioning_manager_poll_event());

    /* Deinit ends the controller lifecycle and drops pending events. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_deinit());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());

    /* Re-init starts a fresh adapter lifecycle. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_DISABLED,
        WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT, 2u);

    /* A notification carrying the PREVIOUS lifecycle's session token is
     * discarded, even though PROVISIONING would normally mean STARTED. */
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
        WIFI_PROVISIONING_CONTROLLER_PROVISIONING, 1u);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());

    /* The fresh lifecycle's own transition is accepted. */
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
        WIFI_PROVISIONING_CONTROLLER_PROVISIONING, 2u);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_STARTED,
                          wifi_provisioning_manager_poll_event());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());
}

static void test_poll_event_ordering_started_then_succeeded(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* Queue the whole lifecycle before polling: the supervisor must see
     * STARTED first and SUCCEEDED second - no reordering. */
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_DISABLED,
        WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT, 1u);
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
        WIFI_PROVISIONING_CONTROLLER_PROVISIONING, 1u);
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
        WIFI_PROVISIONING_CONTROLLER_GRACE, 1u);
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_GRACE,
        WIFI_PROVISIONING_CONTROLLER_RETIRING_AP, 1u);
    wifi_provisioning_controller_mock_fire(
        WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
        WIFI_PROVISIONING_CONTROLLER_ONLINE, 1u);

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_STARTED,
                          wifi_provisioning_manager_poll_event());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_SUCCEEDED,
                          wifi_provisioning_manager_poll_event());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());
}

static void test_portal_start_failure_records_failed_event(void)
{
    wifi_provisioning_mock_config_t fail = {.fail_start = true};
    wifi_provisioning_mock_set_config(&fail);

    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_ERR_START_FAILED,
                          wifi_provisioning_manager_start());

    /* The adapter stored the outcome; the supervisor polls it. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_FAILED,
                          wifi_provisioning_manager_poll_event());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());

    /* A later successful start does not fabricate an event. */
    wifi_provisioning_mock_config_t ok = {.fail_start = false};
    wifi_provisioning_mock_set_config(&ok);
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_start());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_EVENT_NONE,
                          wifi_provisioning_manager_poll_event());
}

/* --------------------------------------------------------------------- */
/* Test registry                                                          */
/* --------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_deinit_lifecycle);
    RUN_TEST(test_start_stop_lifecycle_and_idempotency);
    RUN_TEST(test_stop_with_portal_already_stopped);
    RUN_TEST(test_stop_ends_controller_lifecycle_and_closes_listeners);
    RUN_TEST(test_start_before_mongoose_running);
    RUN_TEST(test_start_failure_propagates_and_recovers);

    /* Platform start-status mapping (TASK-134). */
    RUN_TEST(test_platform_start_status_mapping);

    RUN_TEST(test_stop_failure_propagates);
    RUN_TEST(test_listener_url_overrides_applied_before_start);
    RUN_TEST(test_no_overrides_uses_compiled_in_defaults);
    RUN_TEST(test_invalid_url_overrides_rejected);
    RUN_TEST(test_is_portal_active_query);
    RUN_TEST(test_is_active_requires_fully_reachable_portal);
    RUN_TEST(test_has_saved_credentials_query);
    RUN_TEST(test_deinit_stops_active_portal);
    RUN_TEST(test_concurrent_start_stop_is_serialized);
    RUN_TEST(test_log_content_regression);

    /* Controller notification translation (TASK-132). */
    RUN_TEST(test_init_registers_controller_notification_hook);
    RUN_TEST(test_notification_stored_for_each_transition);
    RUN_TEST(test_saved_credential_connect_is_not_a_success);
    RUN_TEST(test_stale_session_notification_discarded);
    RUN_TEST(test_poll_event_ordering_started_then_succeeded);
    RUN_TEST(test_portal_start_failure_records_failed_event);

    return UNITY_END();
}