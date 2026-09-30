/**
 * @file lamp_control_mock.h
 * @brief Test-only lamp-control double for network-adapter host tests
 *        (TASK-109)
 *
 * The adapter's fail-off contract is expressed through
 * lamp_control_force_inactive().  The double records the calls so tests can
 * assert that a disconnect-class event forces the output off BEFORE the
 * application callback runs, and that a stale (dropped) event still
 * performs the idempotent fail-off.
 */

#ifndef LAMP_CONTROL_MOCK_H
#define LAMP_CONTROL_MOCK_H

#include <stdbool.h>

#include "lamp_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reset the double (call count zero, success result). */
void lamp_mock_reset(void);

/** @brief Number of lamp_control_force_inactive() calls recorded. */
unsigned lamp_mock_force_inactive_calls(void);

/** @brief Make lamp_control_force_inactive() return @p status. */
void lamp_mock_set_force_inactive_result(lamp_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* LAMP_CONTROL_MOCK_H */
