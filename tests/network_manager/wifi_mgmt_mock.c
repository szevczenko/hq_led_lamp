/**
 * @file wifi_mgmt_mock.c
 * @brief Test-only Wi-Fi manager double (see wifi_mgmt_mock.h).
 */

#include "wifi_mgmt_mock.h"

#include <pthread.h>
#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* Internal state                                                         */
/* --------------------------------------------------------------------- */

#define WIFI_MOCK_MAX_SUBSCRIBERS 8

typedef struct wifi_mock_subscription
{
  wifi_mgmt_event_cb_t cb;
  void                *user_data;
} wifi_mock_subscription_t;

typedef struct wifi_mock_state
{
  wifi_mock_config_t        config;
  wifi_mock_counters_t      counters;
  wifi_mock_subscription_t  subs[WIFI_MGMT_EVENT_MODE_CHANGED + 1]
                                 [WIFI_MOCK_MAX_SUBSCRIBERS];
} wifi_mock_state_t;

static wifi_mock_state_t s_mock;

/**
 * @brief Protects the mock state (counters, subscription slots, config) and
 *        the emission/snapshot paths.
 *
 * The concurrent first-use/start-stop tests exercise the double from
 * multiple pthreads, so every access to @c s_mock is serialized with this
 * mutex.  Delivery callbacks (the adapter handler) run INSIDE the mutex on
 * purpose: this mirrors the production manager, which serializes event
 * dispatch against subscription-table mutations; the adapter's handler only
 * re-enters the mock's stateless connect query or the recording counters,
 * both of which are mutex-guarded and non-blocking, so no deadlock is
 * possible (no API of this double takes a second lock or blocks).
 *
 * wifi_mgmt_mock_block_wait_ready() is the one deliberate exception: it
 * makes wifi_mgmt_wait_ready() block while HOLDING s_mock_lock so a test can
 * park a start() inside its transaction.  Because that path never re-enters
 * any other mock function (the adapter does not call anything else while
 * wait_ready blocks) and the releasing test thread only touches the condvar
 * API below, this cannot deadlock.
 */
static pthread_mutex_t s_mock_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t s_wait_block_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_wait_block_cond  = PTHREAD_COND_INITIALIZER;
static bool            s_wait_block       = false;
static bool            s_wait_blocked     = false;
static bool            s_wait_release     = false;

/* --------------------------------------------------------------------- */
/* Test control API                                                       */
/* --------------------------------------------------------------------- */

void wifi_mgmt_mock_reset(void)
{
  memset(&s_mock, 0, sizeof(s_mock));

  pthread_mutex_lock(&s_wait_block_lock);
  s_wait_block   = false;
  s_wait_blocked = false;
  s_wait_release = false;
  pthread_mutex_unlock(&s_wait_block_lock);
}

void wifi_mgmt_mock_set_config(const wifi_mock_config_t *config)
{
  pthread_mutex_lock(&s_mock_lock);
  if (config != NULL)
  {
    s_mock.config = *config;
  }
  pthread_mutex_unlock(&s_mock_lock);
}

int wifi_mgmt_mock_emit(wifi_mgmt_event_t event)
{
  int delivered = 0;

  if ((event < WIFI_MGMT_EVENT_CONNECTED) ||
      (event > WIFI_MGMT_EVENT_MODE_CHANGED))
  {
    return 0;
  }

  /* Snapshot the callback pointers and deliver INSIDE the mock mutex: the
   * concurrent tests drive this double from several pthreads, so the
   * snapshot and the subscriber-table walk must be serialized against
   * subscribe/unsubscribe (see the s_mock_lock comment).  Handlers never
   * block on another mock lock, so this cannot deadlock. */
  pthread_mutex_lock(&s_mock_lock);

  wifi_mgmt_event_cb_t cbs[WIFI_MOCK_MAX_SUBSCRIBERS];
  void *data[WIFI_MOCK_MAX_SUBSCRIBERS];
  unsigned n = 0U;

  for (unsigned i = 0U; i < WIFI_MOCK_MAX_SUBSCRIBERS; ++i)
  {
    if (s_mock.subs[event][i].cb != NULL)
    {
      cbs[n]  = s_mock.subs[event][i].cb;
      data[n] = s_mock.subs[event][i].user_data;
      ++n;
    }
  }

  for (unsigned i = 0U; i < n; ++i)
  {
    cbs[i](event, data[i]);
    ++delivered;
  }

  pthread_mutex_unlock(&s_mock_lock);

  return delivered;
}

void wifi_mgmt_mock_set_connected(bool connected)
{
  pthread_mutex_lock(&s_mock_lock);
  s_mock.config.connected_state = connected;
  pthread_mutex_unlock(&s_mock_lock);
}

wifi_mock_counters_t wifi_mgmt_mock_get_counters(void)
{
  pthread_mutex_lock(&s_mock_lock);
  wifi_mock_counters_t counters = s_mock.counters;
  pthread_mutex_unlock(&s_mock_lock);
  return counters;
}

wifi_mgmt_event_cb_t wifi_mgmt_mock_get_subscribed_cb(wifi_mgmt_event_t event)
{
  if ((event < WIFI_MGMT_EVENT_CONNECTED) ||
      (event > WIFI_MGMT_EVENT_MODE_CHANGED))
  {
    return NULL;
  }

  pthread_mutex_lock(&s_mock_lock);
  wifi_mgmt_event_cb_t cb = NULL;
  for (unsigned i = 0U; i < WIFI_MOCK_MAX_SUBSCRIBERS; ++i)
  {
    if (s_mock.subs[event][i].cb != NULL)
    {
      cb = s_mock.subs[event][i].cb;
      break;
    }
  }
  pthread_mutex_unlock(&s_mock_lock);

  return cb;
}

void *wifi_mgmt_mock_get_subscribed_user_data(wifi_mgmt_event_t event)
{
  if ((event < WIFI_MGMT_EVENT_CONNECTED) ||
      (event > WIFI_MGMT_EVENT_MODE_CHANGED))
  {
    return NULL;
  }

  pthread_mutex_lock(&s_mock_lock);
  void *user_data = NULL;
  for (unsigned i = 0U; i < WIFI_MOCK_MAX_SUBSCRIBERS; ++i)
  {
    if (s_mock.subs[event][i].cb != NULL)
    {
      user_data = s_mock.subs[event][i].user_data;
      break;
    }
  }
  pthread_mutex_unlock(&s_mock_lock);

  return user_data;
}

int wifi_mgmt_mock_emit_with_user_data(wifi_mgmt_event_t event,
                                       void *user_data)
{
  if ((event < WIFI_MGMT_EVENT_CONNECTED) ||
      (event > WIFI_MGMT_EVENT_MODE_CHANGED))
  {
    return 0;
  }

  /* Deliver to the FIRST current subscriber with the caller-supplied
   * user_data (stale-snapshot model: current callback, old token), inside
   * the mock mutex like every other subscriber-table access. */
  pthread_mutex_lock(&s_mock_lock);

  for (unsigned i = 0U; i < WIFI_MOCK_MAX_SUBSCRIBERS; ++i)
  {
    if (s_mock.subs[event][i].cb != NULL)
    {
      s_mock.subs[event][i].cb(event, user_data);
      pthread_mutex_unlock(&s_mock_lock);
      return 1;
    }
  }

  pthread_mutex_unlock(&s_mock_lock);
  return 0;
}

/* --------------------------------------------------------------------- */
// Wi-Fi manager contract implementation
/* --------------------------------------------------------------------- */

void wifi_mgmt_set_wifi_type(wifi_type_t type)
{
  pthread_mutex_lock(&s_mock_lock);
  ++s_mock.counters.set_type_calls;
  s_mock.counters.last_type = type;
  pthread_mutex_unlock(&s_mock_lock);
}

void wifi_mgmt_init(void)
{
  pthread_mutex_lock(&s_mock_lock);
  ++s_mock.counters.init_calls;
  pthread_mutex_unlock(&s_mock_lock);
}

void wifi_mgmt_start(void)
{
  pthread_mutex_lock(&s_mock_lock);
  ++s_mock.counters.start_calls;
  pthread_mutex_unlock(&s_mock_lock);
}

bool wifi_mgmt_stop(void)
{
  pthread_mutex_lock(&s_mock_lock);
  ++s_mock.counters.stop_calls;
  pthread_mutex_unlock(&s_mock_lock);
  return true;
}

bool wifi_mgmt_wait_ready(uint32_t timeout_ms)
{
  /* Transaction-park point (review round 2, issue 1): with
   * wifi_mgmt_mock_block_wait_ready() armed, this call blocks until the
   * test releases it, so a start() is parked after it subscribed and
   * started the manager — the exact window the start/stop transaction
   * regression needs.  This is the ONLY mock function that blocks; it
   * holds s_mock_lock while waiting (see the s_mock_lock comment). */
  (void)timeout_ms;

  pthread_mutex_lock(&s_wait_block_lock);
  const bool block_requested = s_wait_block;
  if (block_requested)
  {
    s_wait_blocked = true;
    pthread_cond_broadcast(&s_wait_block_cond);

    while (!s_wait_release)
    {
      pthread_cond_wait(&s_wait_block_cond, &s_wait_block_lock);
    }
    s_wait_release   = false;
    s_wait_block     = false;
    s_wait_blocked   = false;
  }
  pthread_mutex_unlock(&s_wait_block_lock);

  pthread_mutex_lock(&s_mock_lock);
  const bool ok = !s_mock.config.fail_wait_ready;
  pthread_mutex_unlock(&s_mock_lock);

  return ok;
}

bool wifi_mgmt_connect(void)
{
  pthread_mutex_lock(&s_mock_lock);
  ++s_mock.counters.connect_calls;
  s_mock.counters.start_before_connect = (s_mock.counters.start_calls > 0U);
  const bool ok = !s_mock.config.fail_connect;
  pthread_mutex_unlock(&s_mock_lock);

  return ok;
}

bool wifi_mgmt_disconnect(void)
{
  pthread_mutex_lock(&s_mock_lock);
  ++s_mock.counters.disconnect_calls;
  pthread_mutex_unlock(&s_mock_lock);
  return true;
}

bool wifi_mgmt_is_connected(void)
{
  /* Only read the single bool flag; no other state is touched. */
  pthread_mutex_lock(&s_mock_lock);
  const bool connected = s_mock.config.connected_state;
  pthread_mutex_unlock(&s_mock_lock);

  return connected;
}

bool wifi_mgmt_subscribe(wifi_mgmt_event_t event, wifi_mgmt_event_cb_t cb,
                         void *user_data)
{
  pthread_mutex_lock(&s_mock_lock);

  ++s_mock.counters.subscribe_calls;

  bool ok = false;
  if (!s_mock.config.fail_subscribe &&
      (event >= WIFI_MGMT_EVENT_CONNECTED) &&
      (event <= WIFI_MGMT_EVENT_MODE_CHANGED) && (cb != NULL))
  {
    for (unsigned i = 0U; i < WIFI_MOCK_MAX_SUBSCRIBERS; ++i)
    {
      if (s_mock.subs[event][i].cb == NULL)
      {
        s_mock.subs[event][i].cb        = cb;
        s_mock.subs[event][i].user_data = user_data;
        ok = true;
        break;
      }
    }
  }

  pthread_mutex_unlock(&s_mock_lock);
  return ok;
}

bool wifi_mgmt_unsubscribe(wifi_mgmt_event_t event, wifi_mgmt_event_cb_t cb,
                           void *user_data)
{
  pthread_mutex_lock(&s_mock_lock);

  ++s_mock.counters.unsubscribe_calls;

  bool removed = false;
  if ((event >= WIFI_MGMT_EVENT_CONNECTED) &&
      (event <= WIFI_MGMT_EVENT_MODE_CHANGED) && (cb != NULL))
  {
    for (unsigned i = 0U; i < WIFI_MOCK_MAX_SUBSCRIBERS; ++i)
    {
      if ((s_mock.subs[event][i].cb == cb) &&
          (s_mock.subs[event][i].user_data == user_data))
      {
        s_mock.subs[event][i].cb        = NULL;
        s_mock.subs[event][i].user_data = NULL;
        removed = true;
      }
    }
  }

  pthread_mutex_unlock(&s_mock_lock);
  return removed;
}

bool wifi_mgmt_deinit(void)
{
  /* Not used by the adapter; present for link completeness. */
  return true;
}

/* --------------------------------------------------------------------- */
/* Transaction-park control (start/stop race regression)                  */
/* --------------------------------------------------------------------- */

void wifi_mgmt_mock_block_wait_ready(void)
{
  pthread_mutex_lock(&s_wait_block_lock);
  s_wait_block   = true;
  s_wait_blocked = false;
  s_wait_release = false;
  pthread_mutex_unlock(&s_wait_block_lock);
}

void wifi_mgmt_mock_wait_blocked_in_wait_ready(void)
{
  pthread_mutex_lock(&s_wait_block_lock);
  while (!s_wait_blocked)
  {
    pthread_cond_wait(&s_wait_block_cond, &s_wait_block_lock);
  }
  pthread_mutex_unlock(&s_wait_block_lock);
}

void wifi_mgmt_mock_release_wait_ready(void)
{
  pthread_mutex_lock(&s_wait_block_lock);
  s_wait_release = true;
  s_wait_block   = false;
  pthread_cond_broadcast(&s_wait_block_cond);
  pthread_mutex_unlock(&s_wait_block_lock);
}