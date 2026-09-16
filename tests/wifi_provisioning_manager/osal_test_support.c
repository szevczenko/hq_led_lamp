/**
 * @file osal_test_support.c
 * @brief Test-only OSAL doubles (see osal_test_support.h).
 */

#include "osal_test_support.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* Mutex (same handle shape as the platform posix backend)                */
/* --------------------------------------------------------------------- */

osal_status_t osal_mutex_create(osal_mutex_id_t *mutex_id, const char *name)
{
    (void)name;

    if (mutex_id == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    pthread_mutex_t *mutex = malloc(sizeof(*mutex));
    if (mutex == NULL)
    {
        return OSAL_ERROR;
    }

    if (pthread_mutex_init(mutex, NULL) != 0)
    {
        free(mutex);
        return OSAL_ERROR;
    }

    *mutex_id = mutex;
    return OSAL_SUCCESS;
}

osal_status_t osal_mutex_delete(osal_mutex_id_t mutex_id)
{
    if (mutex_id == NULL)
    {
        return OSAL_ERR_INVALID_ID;
    }

    if (pthread_mutex_destroy(mutex_id) != 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    free(mutex_id);
    return OSAL_SUCCESS;
}

osal_status_t osal_mutex_take(osal_mutex_id_t mutex_id)
{
    if (mutex_id == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    if (pthread_mutex_lock(mutex_id) != 0)
    {
        return OSAL_SEM_FAILURE;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_mutex_give(osal_mutex_id_t mutex_id)
{
    if (mutex_id == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    if (pthread_mutex_unlock(mutex_id) != 0)
    {
        return OSAL_SEM_FAILURE;
    }

    return OSAL_SUCCESS;
}

/* --------------------------------------------------------------------- */
/* Task delay: recorded, never sleeps                                     */
/* --------------------------------------------------------------------- */

static unsigned s_delay_calls;
static uint32_t s_delay_total_ms;

osal_status_t osal_task_delay_ms(uint32_t milliseconds)
{
    ++s_delay_calls;
    s_delay_total_ms += milliseconds;
    return OSAL_SUCCESS;
}

unsigned osal_test_delay_call_count(void)
{
    return s_delay_calls;
}

uint32_t osal_test_delay_total_ms(void)
{
    return s_delay_total_ms;
}

/* --------------------------------------------------------------------- */
/* Log capture                                                            */
/* --------------------------------------------------------------------- */

#define LOG_CAP_BYTES 4096u

static char    s_log[LOG_CAP_BYTES];
static size_t  s_log_len;

void osal_test_log_reset(void)
{
    memset(s_log, 0, sizeof(s_log));
    s_log_len = 0U;
}

const char *osal_test_log_get(void)
{
    return s_log;
}

/* osal_log_printf: called by the osal_log.h macros used by the adapter. */
void osal_log_printf(const char *level, const char *format, ...)
{
    va_list args;
    char line[512];
    int n;

    n = snprintf(line, sizeof(line), "[%s]: ", level);
    if ((n <= 0) || ((size_t)n >= sizeof(line)))
    {
        return;
    }

    va_start(args, format);
    int body = vsnprintf(line + n, sizeof(line) - (size_t)n, format, args);
    va_end(args);
    if (body < 0)
    {
        return;
    }

    size_t written = (size_t)(n + body);
    if (written >= (LOG_CAP_BYTES - 1U))
    {
        /* Drop overlong lines entirely rather than truncating them: a
         * truncated credential string could look like a leak. */
        return;
    }

    if ((s_log_len + written + 1U) > LOG_CAP_BYTES)
    {
        /* Ring overflow: drop the oldest half. */
        size_t keep = s_log_len - (s_log_len / 2U);
        memmove(s_log, s_log + (s_log_len - keep), keep);
        s_log_len = keep;
    }

    memcpy(s_log + s_log_len, line, written);
    s_log_len += written;
    s_log[s_log_len] = '\0';
}