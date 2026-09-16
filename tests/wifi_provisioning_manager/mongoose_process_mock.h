/**
 * @file mongoose_process_mock.h
 * @brief Test-only shared Mongoose process double (TASK-126)
 *
 * Implements the mongoose_process.h contract as an in-memory test double so
 * the provisioning adapter can be exercised on the host without a real
 * Mongoose poll thread:
 *
 *   - the double tracks whether the shared process would be running,
 *   - MongooseProcess_Invoke() records the call and runs the callback
 *     synchronously on the caller (the adapter never uses it directly; it
 *     exists so the mock fully satisfies the platform header contract),
 *   - call counters let tests assert the adapter only QUERIES the process
 *     (IsRunning) and never initializes or deinitializes it (Init/Deinit
 *     counters must stay zero during adapter operation).
 *
 * This file is a test double only: it is compiled solely into the
 * provisioning-manager host test binary and is never part of any production
 * build.
 */

#ifndef MONGOOSE_PROCESS_MOCK_H
#define MONGOOSE_PROCESS_MOCK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mongoose_process_mock_counters
{
  unsigned init_calls;      /**< MongooseProcess_Init() invocations.      */
  unsigned deinit_calls;    /**< MongooseProcess_Deinit() invocations.    */
  unsigned is_running_calls;/**< MongooseProcess_IsRunning() invocations. */
  unsigned invoke_calls;    /**< MongooseProcess_Invoke() invocations.    */
} mongoose_process_mock_counters_t;

/** @brief Reset the double to a not-running process with zero counters. */
void mongoose_process_mock_reset(void);

/** @brief Set the "running" state reported by MongooseProcess_IsRunning(). */
void mongoose_process_mock_set_running(bool running);

/** @brief Snapshot of the accumulated call counters. */
mongoose_process_mock_counters_t mongoose_process_mock_get_counters(void);

#ifdef __cplusplus
}
#endif

#endif /* MONGOOSE_PROCESS_MOCK_H */