/**
 * @file osal_test_support.h
 * @brief Test-only OSAL doubles for provisioning-adapter host tests
 *        (TASK-126)
 *
 * Provides the small OSAL surface the provisioning adapter uses on the host:
 *
 *   - mutexes: implemented directly on pthreads with the same handle type
 *     as the platform posix backend (osal_mutex_id_t = pthread_mutex_t*),
 *     so the adapter's lock/unlock discipline is exercised for real,
 *   - task delay: recorded, never sleeps (deterministic tests),
 *   - logging: every formatted line is captured so the tests can assert
 *     that no credential-like content is ever logged.
 */

#ifndef OSAL_TEST_SUPPORT_H
#define OSAL_TEST_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "osal_common_type.h"
#include "osal_error.h"
#include "osal_mutex.h"
#include "osal_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Task-delay double                                                      */
/* --------------------------------------------------------------------- */

/** @brief Total number of osal_task_delay_ms() calls recorded. */
unsigned osal_test_delay_call_count(void);

/** @brief Total accumulated delayed time [ms] recorded. */
uint32_t osal_test_delay_total_ms(void);

/* --------------------------------------------------------------------- */
/* Task time (TASK-135)                                                  */
/* --------------------------------------------------------------------- */

/**
 * @brief Set the test-controlled monotonic clock value in milliseconds.
 *
 * The adapter rates its "portal up; station not yet connected" wait
 * signature off osal_task_get_time_ms(); this function returns this value
 * (0 initially, restarted by osal_test_log_reset()) so tests can exercise
 * the 30 s rate limit deterministically.
 */
void osal_test_set_time_ms(uint32_t milliseconds);

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