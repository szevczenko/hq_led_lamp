/**
 * @file osal_test_support.h
 * @brief Test-only OSAL doubles for app_state host tests (TASK-115)
 *
 * Provides the small OSAL surface the application state machine uses on
 * the host:
 *
 *   - mutexes: implemented directly on pthreads with the same handle type
 *     as the platform posix backend (osal_mutex_id_t = pthread_mutex_t*),
 *     so the module's lock/unlock discipline is exercised for real,
 *   - task delay: recorded, never sleeps (deterministic tests),
 *   - the monotonic clock: the app_state module only uses
 *     osal_task_get_time_ms() as its DEFAULT clock — the tests always
 *     inject a deterministic test clock instead, but the default symbol
 *     must still resolve, so it is provided as a settable test time,
 *   - logging: every formatted line is captured so tests can assert that
 *     no credential-like content is ever logged and that transitions are
 *     reported.
 */

#ifndef OSAL_TEST_SUPPORT_H
#define OSAL_TEST_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>

#include "osal_common_type.h"
#include "osal_error.h"
#include "osal_mutex.h"
#include "osal_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Monotonic clock double                                                 */
/* --------------------------------------------------------------------- */

/** @brief Set the value osal_task_get_time_ms() will report. */
void osal_test_set_time_ms(uint32_t now_ms);

/* --------------------------------------------------------------------- */
/* Task-delay double                                                      */
/* --------------------------------------------------------------------- */

/** @brief Total number of osal_task_delay_ms() calls recorded. */
unsigned osal_test_delay_call_count(void);

/** @brief Total accumulated delayed time [ms] recorded. */
uint32_t osal_test_delay_total_ms(void);

/* --------------------------------------------------------------------- */
/* Log capture                                                            */
/* --------------------------------------------------------------------- */

/** @brief Clear the captured log. */
void osal_test_log_reset(void);

/**
 * @brief Read-only access to the captured log (NUL-terminated).
 *
 * The capture is a fixed-size ring: on overflow the oldest content is
 * dropped, so tests must flush between checks.
 */
const char *osal_test_log_get(void);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_TEST_SUPPORT_H */