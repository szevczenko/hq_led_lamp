/**
 * @file concurrency_first_use_test.c
 * @brief Standalone first-use concurrency regression (review round 3,
 *        issue 4)
 *
 * Review round 3, issue 4: the original first-use lock-creation regression
 * lived inside the main Unity suite, where every tearDown() and earlier
 * test had already called network_manager_start()/stop() — so the adapter
 * mutex was always published long before the "first-use" burst ran, and
 * the lazy-creation race (finding 4) was never actually exercised.
 *
 * This executable runs the concurrent burst as the very FIRST network
 * manager API use in a fresh process and fails immediately if that premise
 * is violated (network_manager_test_get_lock() must still be NULL).  A
 * companion test then hammers the already-published-lock start/stop/query
 * lifecycle race, which a fresh-process-only burst cannot cover.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "lamp_control_mock.h"
#include "network_manager.h"
#include "osal_test_support.h"
#include "unity.h"
#include "wifi_mgmt_mock.h"

#define CONCURRENT_THREADS 8

/* --------------------------------------------------------------------- */
/* Fixtures                                                               */
/* --------------------------------------------------------------------- */

typedef struct app_callbacks
{
    unsigned connected_calls;
    unsigned disconnected_calls;
    void    *last_context;
} app_callbacks_t;

static app_callbacks_t s_app;

static void on_connected(void *context)
{
    s_app.connected_calls++;
    s_app.last_context = context;
}

static void on_disconnected(void *context)
{
    s_app.disconnected_calls++;
    s_app.last_context = context;
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

static void run_burst(void *(*entry)(void *), unsigned count)
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

void setUp(void)
{
    wifi_mgmt_mock_reset();
    lamp_mock_reset();
    osal_test_log_reset();
    memset(&s_app, 0, sizeof(s_app));
    memset(&s_conc, 0, sizeof(s_conc));
}

void tearDown(void)
{
    /* Every test leaves the adapter stopped. */
    network_manager_stop();
}

/* --------------------------------------------------------------------- */
/* Tests                                                                  */
/* --------------------------------------------------------------------- */

/**
 * Concurrent FIRST-use start in a fresh process: every thread hits the
 * lazy mutex creation (finding 4) and the atomic started flag (finding 3)
 * at once.  Exactly one start may win; the losers are rejected coherently
 * and the manager ends with a single, consistent registration set.
 */
static void test_concurrent_true_first_start_race_is_safe(void)
{
    wifi_mock_counters_t counters;

    /* The whole premise of this regression: NO network_manager API call
     * has run in this process yet, so the adapter mutex is unpublished and
     * the burst below really races through the lock's first-use CAS
     * adoption.  If this ever fails, the first-use coverage has silently
     * regressed into an already-published-lock test. */
    TEST_ASSERT_NULL_MESSAGE(network_manager_test_get_lock(),
                             "premise: lock must be unpublished at the "
                             "process's first network_manager API use");

    run_burst(start_worker, CONCURRENT_THREADS);

    /* Exactly one session owns the adapter; the rest were rejected without
     * corrupting the lifecycle state (no invalid mutex use, no torn flag,
     * no CAS loser left without a usable lock). */
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_conc.start_ok));
    TEST_ASSERT_EQUAL_UINT(CONCURRENT_THREADS - 1U,
                           atomic_load(&s_conc.start_rejected));

    /* The winner's onboarding ran: station mode selected and the three
     * product events subscribed exactly once (the losers never subscribed:
     * they were rejected under the adapter lock before touching the
     * manager). */
    counters = wifi_mgmt_mock_get_counters();
    TEST_ASSERT_EQUAL_UINT32(T_WIFI_TYPE_CLIENT, counters.last_type);
    TEST_ASSERT_EQUAL_UINT(3U, counters.subscribe_calls);

    /* The adapter is operational after the race: events deliver normally
     * to the one armed session, and the lock survived the creation race. */
    TEST_ASSERT_EQUAL_INT(1, wifi_mgmt_mock_emit(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(1U, s_app.connected_calls);
    TEST_ASSERT_NOT_NULL(network_manager_test_get_lock());

    network_manager_stop();
    TEST_ASSERT_FALSE(network_manager_is_connected());
}

/**
 * Companion: the ALREADY-published-lock lifecycle race (start/stop/query
 * colliding after first use).  This complements the fresh-process burst,
 * which cannot cover the published-lock case by construction.
 */
static void test_concurrent_published_lock_start_stop_query_race_is_safe(void)
{
    /* The previous test published the lock; assert that premise too. */
    TEST_ASSERT_NOT_NULL(network_manager_test_get_lock());

    /* Mixed burst against the published lock: half the threads start, a
     * quarter stop, a quarter query. */
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
     * coherent, quiesced state: stop() is idempotent, so after the final
     * stop() the adapter is disarmed, nothing is registered and no
     * application callback was delivered. */
    network_manager_stop();
    TEST_ASSERT_FALSE(network_manager_is_connected());
    TEST_ASSERT_NULL(
        wifi_mgmt_mock_get_subscribed_cb(WIFI_MGMT_EVENT_CONNECTED));
    TEST_ASSERT_EQUAL_UINT(0U, s_app.connected_calls);
    TEST_ASSERT_EQUAL_UINT(0U, s_app.disconnected_calls);

    /* A fresh start still works: the published lock survived the race and
     * the lifecycle state is arming again. */
    network_callbacks_t cb = make_callbacks(NULL);
    TEST_ASSERT_EQUAL_INT(NETWORK_OK, network_manager_start(&cb));
    TEST_ASSERT_TRUE(network_manager_is_connected() ||
                     wifi_mgmt_mock_get_subscribed_user_data(
                         WIFI_MGMT_EVENT_CONNECTED) != NULL);
}

int main(void)
{
    UNITY_BEGIN();

    /* Order matters: the fresh-process first-use burst MUST run before any
     * other network_manager API call in this process (guarded by the NULL
     * premise assertion inside the test), then the published-lock race. */
    RUN_TEST(test_concurrent_true_first_start_race_is_safe);
    RUN_TEST(test_concurrent_published_lock_start_stop_query_race_is_safe);

    return UNITY_END();
}
