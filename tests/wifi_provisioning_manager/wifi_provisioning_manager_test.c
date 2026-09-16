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
 * an active portal, concurrent start/stop serialization and the
 * credentials-never-logged secrecy rule.
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
#include "wifi_provisioning_manager.h"
#include "wifi_provisioning_mock.h"

/* --------------------------------------------------------------------- */
/* Fixtures                                                               */
/* --------------------------------------------------------------------- */

void setUp(void)
{
    wifi_provisioning_mock_reset();
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

    /* Stop brings the portal down. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_STOPPED,
                          wifi_provisioning_manager_get_state());
    TEST_ASSERT_EQUAL_UINT(1u,
                           wifi_provisioning_mock_get_counters().stop_calls);

    /* Still not initialized/deinitialized the process even after stop. */
    mg = mongoose_process_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT(0u, mg.init_calls);
    TEST_ASSERT_EQUAL_UINT(0u, mg.deinit_calls);

    /* Second stop while stopped: idempotent no-op. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_EQUAL_UINT(2u,
                           wifi_provisioning_mock_get_counters().stop_calls);
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
}

static void test_stop_with_portal_already_stopped(void)
{
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_init());

    /* A stop on a portal that never started and a repeat stop are both
     * successful no-ops. */
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_OK,
                          wifi_provisioning_manager_stop());
    TEST_ASSERT_EQUAL_UINT(2u,
                           wifi_provisioning_mock_get_counters().stop_calls);
    TEST_ASSERT_FALSE(wifi_provisioning_manager_is_active());
    TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_MANAGER_STOPPED,
                          wifi_provisioning_manager_get_state());
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
    wifi_provisioning_mock_config_t ok = {.fail_start = false};
    wifi_provisioning_mock_set_config(&ok);

    /* Queries and the provisioning decision hook. */
    (void)wifi_provisioning_manager_get_state();
    (void)wifi_provisioning_manager_is_active();
    wifi_mgmt_mock_set_saved_credentials(true);
    TEST_ASSERT_TRUE(wifi_provisioning_manager_has_saved_credentials());

    /* Captured log: the adapter logged sanitized transitions, but never any
     * credential content and never a provisioning URL. */
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

    /* Sanity: the sanitized adapter transitions WERE logged. */
    TEST_ASSERT_NOT_NULL(strstr(log, "[prov_mgr]"));
    TEST_ASSERT_NOT_NULL(strstr(log, "provisioning portal running"));
    TEST_ASSERT_NOT_NULL(strstr(log, "provisioning portal stopped"));
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
    RUN_TEST(test_start_before_mongoose_running);
    RUN_TEST(test_start_failure_propagates_and_recovers);
    RUN_TEST(test_stop_failure_propagates);
    RUN_TEST(test_listener_url_overrides_applied_before_start);
    RUN_TEST(test_no_overrides_uses_compiled_in_defaults);
    RUN_TEST(test_invalid_url_overrides_rejected);
    RUN_TEST(test_is_portal_active_query);
    RUN_TEST(test_has_saved_credentials_query);
    RUN_TEST(test_deinit_stops_active_portal);
    RUN_TEST(test_concurrent_start_stop_is_serialized);
    RUN_TEST(test_log_content_regression);

    return UNITY_END();
}