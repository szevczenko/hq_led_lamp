/**
 * @file wifi_provisioning_mock.c
 * @brief Test-only platform provisioning application double
 *        (see wifi_provisioning_mock.h).
 */

#include "wifi_provisioning_mock.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* Internal state                                                         */
/* --------------------------------------------------------------------- */

typedef struct wifi_provisioning_mock_state
{
  wifi_provisioning_mock_config_t   config;
  wifi_provisioning_mock_counters_t counters;
  wifi_http_provisioning_state_t    state;
                                 /**< Portal lifecycle state.                    */
  bool                              radio_up;
                                 /**< Whether the mock radio reached AP+STA
                                      (is_reachable gate, TASK-134).         */
  wifi_http_provisioning_start_status_t last_start_status;
                                 /**< Result of the most recent start attempt.  */
  char                              last_ssid[33];
  char                              last_password[65];
} wifi_provisioning_mock_state_t;

static wifi_provisioning_mock_state_t s_mock;

/* Protects the mock state.  The adapter serializes start/stop with its own
 * lock, so the double normally sees one caller at a time; the lock still
 * keeps the counters/state coherent if a test bypasses the adapter. */
static pthread_mutex_t s_mock_lock = PTHREAD_MUTEX_INITIALIZER;

/* Concurrency watermark for the start/stop serialization assertion.  Uses
 * atomics so the adapter's serialization property is measured without
 * holding the state lock across the (mock) start/stop body. */
static atomic_uint s_active;
static atomic_uint s_max_active;

static void mock_enter(void)
{
  unsigned active = atomic_fetch_add_explicit(&s_active, 1u,
                                              memory_order_acq_rel) + 1u;
  unsigned prev_max = atomic_load_explicit(&s_max_active,
                                           memory_order_relaxed);
  while (active > prev_max)
  {
    if (atomic_compare_exchange_weak_explicit(
            &s_max_active, &prev_max, active,
            memory_order_release, memory_order_relaxed))
      break;
  }
}

static void mock_leave(void)
{
  atomic_fetch_sub_explicit(&s_active, 1u, memory_order_acq_rel);
}

/* --------------------------------------------------------------------- */
/* Test control API                                                       */
/* --------------------------------------------------------------------- */

void wifi_provisioning_mock_reset(void)
{
  memset(&s_mock, 0, sizeof(s_mock));
  s_mock.state = WIFI_PROVISIONING_STOPPED;
  s_mock.radio_up = true;
  s_mock.last_start_status = WIFI_HTTP_PROVISIONING_START_OK;
  atomic_store_explicit(&s_active, 0u, memory_order_relaxed);
  atomic_store_explicit(&s_max_active, 0u, memory_order_relaxed);
}

void wifi_provisioning_mock_set_config(
    const wifi_provisioning_mock_config_t *config)
{
  pthread_mutex_lock(&s_mock_lock);
  if (config != NULL) s_mock.config = *config;
  pthread_mutex_unlock(&s_mock_lock);
}

void wifi_provisioning_mock_set_radio_up(bool up)
{
  pthread_mutex_lock(&s_mock_lock);
  s_mock.radio_up = up;
  pthread_mutex_unlock(&s_mock_lock);
}

wifi_provisioning_mock_counters_t wifi_provisioning_mock_get_counters(void)
{
  wifi_provisioning_mock_counters_t counters;

  pthread_mutex_lock(&s_mock_lock);
  counters = s_mock.counters;
  counters.max_concurrency = atomic_load_explicit(&s_max_active,
                                                  memory_order_relaxed);
  pthread_mutex_unlock(&s_mock_lock);
  return counters;
}

void wifi_provisioning_mock_submit_credentials(const char *ssid,
                                               const char *password)
{
  pthread_mutex_lock(&s_mock_lock);
  if (ssid != NULL)
  {
    strncpy(s_mock.last_ssid, ssid, sizeof(s_mock.last_ssid) - 1u);
    s_mock.last_ssid[sizeof(s_mock.last_ssid) - 1u] = '\0';
  }
  if (password != NULL)
  {
    strncpy(s_mock.last_password, password,
            sizeof(s_mock.last_password) - 1u);
    s_mock.last_password[sizeof(s_mock.last_password) - 1u] = '\0';
  }
  pthread_mutex_unlock(&s_mock_lock);
}

const char *wifi_provisioning_mock_get_last_ssid(void)
{
  /* Test-only accessor: returns a pointer into the mock state; safe only
   * because the log-content regression runs single-threaded. */
  const char *result;

  pthread_mutex_lock(&s_mock_lock);
  result = s_mock.last_ssid;
  pthread_mutex_unlock(&s_mock_lock);
  return result;
}

const char *wifi_provisioning_mock_get_last_password(void)
{
  static const char *result;

  pthread_mutex_lock(&s_mock_lock);
  result = s_mock.last_password;
  pthread_mutex_unlock(&s_mock_lock);
  return result;
}

/* --------------------------------------------------------------------- */
/* Platform provisioning application contract (wifi_http_provisioning.h)  */
/* --------------------------------------------------------------------- */

bool wifi_http_provisioning_start(void)
{
  /* Thin bool wrapper over start_ex, exactly like the platform: succeeds
   * when the portal is fully up (or already was).  Host mocks and other
   * platform-side callers keep using the wrapper, while the adapter
   * (TASK-134) uses the enum API to distinguish fault modes. */
  wifi_http_provisioning_start_status_t status = wifi_http_provisioning_start_ex();
  return status == WIFI_HTTP_PROVISIONING_START_OK ||
         status == WIFI_HTTP_PROVISIONING_START_ALREADY_RUNNING;
}

wifi_http_provisioning_start_status_t wifi_http_provisioning_start_ex(void)
{
  wifi_http_provisioning_start_status_t status = WIFI_HTTP_PROVISIONING_START_OK;

  mock_enter();

  pthread_mutex_lock(&s_mock_lock);
  ++s_mock.counters.start_calls;

  /* Idempotent: already running or starting is a safe no-op. */
  if (s_mock.state == WIFI_PROVISIONING_RUNNING ||
      s_mock.state == WIFI_PROVISIONING_STARTING)
  {
    status = WIFI_HTTP_PROVISIONING_START_ALREADY_RUNNING;
    s_mock.last_start_status = status;
    pthread_mutex_unlock(&s_mock_lock);
    mock_leave();
    return status;
  }

  s_mock.state = WIFI_PROVISIONING_STARTING;

  if (s_mock.config.start_status_set)
  {
    /* Every documented platform failure mode is selectable (TASK-134). */
    status = s_mock.config.start_status;
  }
  else if (s_mock.config.fail_start)
  {
    /* Legacy generic failure: an undocumented platform status, which the
     * adapter maps to WIFI_PROVISIONING_MANAGER_ERR_START_FAILED. */
    status = (wifi_http_provisioning_start_status_t)0x7F;
  }
  else
  {
    status = WIFI_HTTP_PROVISIONING_START_OK;
  }

  if (status == WIFI_HTTP_PROVISIONING_START_OK)
  {
    s_mock.state = WIFI_PROVISIONING_RUNNING;
  }
  else
  {
    s_mock.state = WIFI_PROVISIONING_ERROR;
  }
  s_mock.last_start_status = status;

  pthread_mutex_unlock(&s_mock_lock);
  mock_leave();
  return status;
}

bool wifi_http_provisioning_stop(void)
{
  bool ok = true;

  mock_enter();

  pthread_mutex_lock(&s_mock_lock);
  ++s_mock.counters.stop_calls;

  /* Idempotent: already stopped is a safe no-op. */
  if (s_mock.state == WIFI_PROVISIONING_STOPPED)
  {
    pthread_mutex_unlock(&s_mock_lock);
    mock_leave();
    return true;
  }

  s_mock.state = WIFI_PROVISIONING_STOPPING;

  if (s_mock.config.fail_stop)
  {
    s_mock.state = WIFI_PROVISIONING_ERROR;
    ok = false;
  }
  else
  {
    s_mock.state = WIFI_PROVISIONING_STOPPED;
  }
  pthread_mutex_unlock(&s_mock_lock);

  mock_leave();
  return ok;
}

wifi_http_provisioning_state_t wifi_http_provisioning_get_state(void)
{
  wifi_http_provisioning_state_t state;

  pthread_mutex_lock(&s_mock_lock);
  state = s_mock.state;
  pthread_mutex_unlock(&s_mock_lock);
  return state;
}

wifi_http_provisioning_start_status_t wifi_http_provisioning_get_last_start_status(void)
{
  wifi_http_provisioning_start_status_t status = WIFI_HTTP_PROVISIONING_START_OK;

  pthread_mutex_lock(&s_mock_lock);
  status = s_mock.last_start_status;
  pthread_mutex_unlock(&s_mock_lock);
  return status;
}

bool wifi_http_provisioning_is_reachable(void)
{
  /* Reachable only when the whole portal is up: RUNNING plus the radio
   * actually in AP+STA.  A RUNNING-without-AP double (radio_up false) is
   * therefore NOT reachable, exactly like the platform (TASK-134). */
  bool reachable = false;

  pthread_mutex_lock(&s_mock_lock);
  if (s_mock.radio_up && s_mock.state == WIFI_PROVISIONING_RUNNING)
    reachable = true;
  pthread_mutex_unlock(&s_mock_lock);
  return reachable;
}

bool wifi_http_provisioning_set_http_url(const char *url)
{
  bool ok = false;

  pthread_mutex_lock(&s_mock_lock);
  ++s_mock.counters.set_http_url_calls;
  if (url != NULL && url[0] != '\0' &&
      s_mock.state != WIFI_PROVISIONING_RUNNING &&
      s_mock.state != WIFI_PROVISIONING_STARTING &&
      s_mock.state != WIFI_PROVISIONING_STOPPING)
  {
    strncpy(s_mock.counters.last_http_url, url,
            sizeof(s_mock.counters.last_http_url) - 1u);
    s_mock.counters.last_http_url[sizeof(s_mock.counters.last_http_url) - 1u] =
        '\0';
    if (s_mock.counters.start_calls == 0u)
      ++s_mock.counters.set_http_before_start;
    ok = true;
  }
  pthread_mutex_unlock(&s_mock_lock);
  return ok;
}

bool wifi_http_provisioning_set_dns_url(const char *url)
{
  bool ok = false;

  pthread_mutex_lock(&s_mock_lock);
  ++s_mock.counters.set_dns_url_calls;
  if (url != NULL && url[0] != '\0' &&
      s_mock.state != WIFI_PROVISIONING_RUNNING &&
      s_mock.state != WIFI_PROVISIONING_STARTING &&
      s_mock.state != WIFI_PROVISIONING_STOPPING)
  {
    strncpy(s_mock.counters.last_dns_url, url,
            sizeof(s_mock.counters.last_dns_url) - 1u);
    s_mock.counters.last_dns_url[sizeof(s_mock.counters.last_dns_url) - 1u] =
        '\0';
    if (s_mock.counters.start_calls == 0u)
      ++s_mock.counters.set_dns_before_start;
    ok = true;
  }
  pthread_mutex_unlock(&s_mock_lock);
  return ok;
}