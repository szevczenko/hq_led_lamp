/**
 * @file wifi_provisioning_controller_mock.c
 * @brief Test-only platform fallback-controller double
 *        (see wifi_provisioning_controller_mock.h).
 */

#include "wifi_provisioning_controller_mock.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static wifi_provisioning_controller_mock_counters_t s_counters;
static bool s_init_result = true;
static bool s_stop_result = true;
static wifi_provisioning_controller_state_t s_state =
    WIFI_PROVISIONING_CONTROLLER_DISABLED;

void wifi_provisioning_controller_mock_reset(void)
{
  pthread_mutex_lock(&s_lock);
  memset(&s_counters, 0, sizeof(s_counters));
  s_init_result = true;
  s_stop_result = true;
  s_state       = WIFI_PROVISIONING_CONTROLLER_DISABLED;
  pthread_mutex_unlock(&s_lock);
}

void wifi_provisioning_controller_mock_set_init_result(bool ok)
{
  pthread_mutex_lock(&s_lock);
  s_init_result = ok;
  pthread_mutex_unlock(&s_lock);
}

void wifi_provisioning_controller_mock_set_stop_result(bool ok)
{
  pthread_mutex_lock(&s_lock);
  s_stop_result = ok;
  pthread_mutex_unlock(&s_lock);
}

void wifi_provisioning_controller_mock_set_state(
    wifi_provisioning_controller_state_t state)
{
  pthread_mutex_lock(&s_lock);
  s_state = state;
  pthread_mutex_unlock(&s_lock);
}

wifi_provisioning_controller_mock_counters_t
wifi_provisioning_controller_mock_get_counters(void)
{
  wifi_provisioning_controller_mock_counters_t counters;

  pthread_mutex_lock(&s_lock);
  counters = s_counters;
  pthread_mutex_unlock(&s_lock);
  return counters;
}

void wifi_provisioning_controller_mock_fire(
    wifi_provisioning_controller_state_t previous,
    wifi_provisioning_controller_state_t current,
    uint32_t session)
{
  wifi_provisioning_controller_state_cb_t cb;
  void *user_ctx;

  pthread_mutex_lock(&s_lock);
  cb       = s_counters.registered_cb;
  user_ctx = s_counters.registered_user_ctx;
  pthread_mutex_unlock(&s_lock);

  /* The real controller invokes the hook outside its internal lock (the
   * adapter re-enters only its own lock from the callback). */
  if (cb != NULL)
  {
    cb(previous, current, session, user_ctx);
  }
}

/* --------------------------------------------------------------------- */
/* wifi_provisioning_controller.h contract                                 */
/* --------------------------------------------------------------------- */

bool wifi_provisioning_controller_init_with_config(
    const wifi_provisioning_controller_config_t *config)
{
  bool result;

  pthread_mutex_lock(&s_lock);
  ++s_counters.init_with_config_calls;
  result = s_init_result;
  if (config != NULL)
  {
    s_counters.registered_cb              = config->on_state_changed;
    s_counters.registered_user_ctx        = config->user_ctx;
    s_counters.config_fallback_budget_set = config->fallback_budget_set;
    s_counters.config_fallback_budget     = config->fallback_budget;
  }
  else
  {
    s_counters.registered_cb              = NULL;
    s_counters.registered_user_ctx        = NULL;
    s_counters.config_fallback_budget_set = false;
    s_counters.config_fallback_budget     = 0u;
  }
  pthread_mutex_unlock(&s_lock);
  return result;
}

void wifi_provisioning_controller_deinit(void)
{
  pthread_mutex_lock(&s_lock);
  ++s_counters.deinit_calls;
  s_counters.registered_cb       = NULL;
  s_counters.registered_user_ctx = NULL;
  s_state                        = WIFI_PROVISIONING_CONTROLLER_DISABLED;
  pthread_mutex_unlock(&s_lock);
}

bool wifi_provisioning_controller_stop(void)
{
  bool result;

  pthread_mutex_lock(&s_lock);
  ++s_counters.stop_calls;
  result = s_stop_result;
  if (result)
  {
    s_state = WIFI_PROVISIONING_CONTROLLER_DISABLED;
  }
  pthread_mutex_unlock(&s_lock);
  return result;
}

wifi_provisioning_controller_state_t wifi_provisioning_controller_get_state(void)
{
  wifi_provisioning_controller_state_t state;

  pthread_mutex_lock(&s_lock);
  state = s_state;
  pthread_mutex_unlock(&s_lock);
  return state;
}