/**
 * @file mongoose_process_mock.c
 * @brief Test-only shared Mongoose process double
 *        (see mongoose_process_mock.h).
 */

#include "mongoose_process_mock.h"

#include <pthread.h>

#include "mongoose_process.h"

/* The shared Mongoose manager the platform exports on its poll thread.
 * The adapter only queries the process, but the symbol must exist because
 * mongoose_process.h declares it as an extern. */
struct mg_mgr mgr;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static bool s_running = false;

static mongoose_process_mock_counters_t s_counters;

void mongoose_process_mock_reset(void)
{
  pthread_mutex_lock(&s_lock);
  s_running = false;
  memset(&s_counters, 0, sizeof(s_counters));
  pthread_mutex_unlock(&s_lock);
}

void mongoose_process_mock_set_running(bool running)
{
  pthread_mutex_lock(&s_lock);
  s_running = running;
  pthread_mutex_unlock(&s_lock);
}

mongoose_process_mock_counters_t mongoose_process_mock_get_counters(void)
{
  mongoose_process_mock_counters_t counters;

  pthread_mutex_lock(&s_lock);
  counters = s_counters;
  pthread_mutex_unlock(&s_lock);
  return counters;
}

/* --------------------------------------------------------------------- */
/* mongoose_process.h contract                                            */
/* --------------------------------------------------------------------- */

void MongooseProcess_Init(void)
{
  pthread_mutex_lock(&s_lock);
  ++s_counters.init_calls;
  s_running = true;
  pthread_mutex_unlock(&s_lock);
}

void MongooseProcess_Deinit(void)
{
  pthread_mutex_lock(&s_lock);
  ++s_counters.deinit_calls;
  s_running = false;
  pthread_mutex_unlock(&s_lock);
}

bool MongooseProcess_IsRunning(void)
{
  bool running;

  pthread_mutex_lock(&s_lock);
  ++s_counters.is_running_calls;
  running = s_running;
  pthread_mutex_unlock(&s_lock);
  return running;
}

bool MongooseProcess_Invoke(mongoose_process_fn_t fn, void *user,
                            uint32_t timeout_ms)
{
  (void)timeout_ms;

  pthread_mutex_lock(&s_lock);
  ++s_counters.invoke_calls;
  pthread_mutex_unlock(&s_lock);

  if (fn == NULL)
  {
    return false;
  }
  fn(&mgr, user);
  return true;
}