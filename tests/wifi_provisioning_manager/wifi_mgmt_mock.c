/**
 * @file wifi_mgmt_mock.c
 * @brief Test-only Wi-Fi manager double (see wifi_mgmt_mock.h).
 */

#include "wifi_mgmt_mock.h"

#include <pthread.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static bool    s_saved_credentials = false;
static unsigned s_calls            = 0u;

void wifi_mgmt_mock_reset(void)
{
  pthread_mutex_lock(&s_lock);
  s_saved_credentials = false;
  s_calls            = 0u;
  pthread_mutex_unlock(&s_lock);
}

void wifi_mgmt_mock_set_saved_credentials(bool saved)
{
  pthread_mutex_lock(&s_lock);
  s_saved_credentials = saved;
  pthread_mutex_unlock(&s_lock);
}

unsigned wifi_mgmt_mock_is_read_data_calls(void)
{
  unsigned calls;

  pthread_mutex_lock(&s_lock);
  calls = s_calls;
  pthread_mutex_unlock(&s_lock);
  return calls;
}

/* --------------------------------------------------------------------- */
/* wifi_managment.h contract (saved-credentials query only)               */
/* --------------------------------------------------------------------- */

bool wifi_mgmt_is_read_data(void)
{
  bool saved;

  pthread_mutex_lock(&s_lock);
  ++s_calls;
  saved = s_saved_credentials;
  pthread_mutex_unlock(&s_lock);
  return saved;
}