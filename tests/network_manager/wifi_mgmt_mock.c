/**
 * @file wifi_mgmt_mock.c
 * @brief Test-only Wi-Fi manager double (see wifi_mgmt_mock.h).
 */

#include "wifi_mgmt_mock.h"

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

/* --------------------------------------------------------------------- */
/* Test control API                                                       */
/* --------------------------------------------------------------------- */

void wifi_mgmt_mock_reset(void)
{
  memset(&s_mock, 0, sizeof(s_mock));
}

void wifi_mgmt_mock_set_config(const wifi_mock_config_t *config)
{
  if (config != NULL)
  {
    s_mock.config = *config;
  }
}

int wifi_mgmt_mock_emit(wifi_mgmt_event_t event)
{
  int delivered = 0;

  if ((event < WIFI_MGMT_EVENT_CONNECTED) ||
      (event > WIFI_MGMT_EVENT_MODE_CHANGED))
  {
    return 0;
  }

  /* Snapshot the callback pointers before delivery so a handler that
   * unsubscribes during delivery cannot walk a mutated table. */
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

  return delivered;
}

void wifi_mgmt_mock_set_connected(bool connected)
{
  s_mock.config.connected_state = connected;
}

wifi_mock_counters_t wifi_mgmt_mock_get_counters(void)
{
  return s_mock.counters;
}

wifi_mgmt_event_cb_t wifi_mgmt_mock_get_subscribed_cb(wifi_mgmt_event_t event)
{
  if ((event < WIFI_MGMT_EVENT_CONNECTED) ||
      (event > WIFI_MGMT_EVENT_MODE_CHANGED))
  {
    return NULL;
  }

  for (unsigned i = 0U; i < WIFI_MOCK_MAX_SUBSCRIBERS; ++i)
  {
    if (s_mock.subs[event][i].cb != NULL)
    {
      return s_mock.subs[event][i].cb;
    }
  }

  return NULL;
}

/* --------------------------------------------------------------------- */
// Wi-Fi manager contract implementation
/* --------------------------------------------------------------------- */

void wifi_mgmt_set_wifi_type(wifi_type_t type)
{
  ++s_mock.counters.set_type_calls;
  s_mock.counters.last_type = type;
}

void wifi_mgmt_init(void)
{
  ++s_mock.counters.init_calls;
}

void wifi_mgmt_start(void)
{
  ++s_mock.counters.start_calls;
}

bool wifi_mgmt_stop(void)
{
  ++s_mock.counters.stop_calls;
  return true;
}

bool wifi_mgmt_wait_ready(uint32_t timeout_ms)
{
  (void)timeout_ms;
  return !s_mock.config.fail_wait_ready;
}

bool wifi_mgmt_connect(void)
{
  ++s_mock.counters.connect_calls;
  s_mock.counters.start_before_connect = (s_mock.counters.start_calls > 0U);

  return !s_mock.config.fail_connect;
}

bool wifi_mgmt_disconnect(void)
{
  ++s_mock.counters.disconnect_calls;
  return true;
}

bool wifi_mgmt_is_connected(void)
{
  return s_mock.config.connected_state;
}

bool wifi_mgmt_subscribe(wifi_mgmt_event_t event, wifi_mgmt_event_cb_t cb,
                         void *user_data)
{
  ++s_mock.counters.subscribe_calls;

  if (s_mock.config.fail_subscribe)
  {
    return false;
  }

  if ((event < WIFI_MGMT_EVENT_CONNECTED) ||
      (event > WIFI_MGMT_EVENT_MODE_CHANGED) || (cb == NULL))
  {
    return false;
  }

  for (unsigned i = 0U; i < WIFI_MOCK_MAX_SUBSCRIBERS; ++i)
  {
    if (s_mock.subs[event][i].cb == NULL)
    {
      s_mock.subs[event][i].cb        = cb;
      s_mock.subs[event][i].user_data = user_data;
      return true;
    }
  }

  return false;
}

bool wifi_mgmt_unsubscribe(wifi_mgmt_event_t event, wifi_mgmt_event_cb_t cb,
                           void *user_data)
{
  ++s_mock.counters.unsubscribe_calls;

  if ((event < WIFI_MGMT_EVENT_CONNECTED) ||
      (event > WIFI_MGMT_EVENT_MODE_CHANGED) || (cb == NULL))
  {
    return false;
  }

  bool removed = false;

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

  return removed;
}

bool wifi_mgmt_deinit(void)
{
  /* Not used by the adapter; present for link completeness. */
  return true;
}
