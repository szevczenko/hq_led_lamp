/**
 * @file lamp_control_mock.h
 * @brief Test-only lamp-control double for device_identity host tests
 *        (TASK-111)
 *
 * device_identity's fail-off contract is expressed through
 * lamp_control_force_inactive(): every identity load rejection must force
 * the lamp output inactive.  The double records the calls and lets tests
 * assert that a rejected identity fails the output off and that a valid
 * identity does not latch anything further (the verified-TLS connect owns
 * the re-enable transition).
 */

#ifndef LAMP_CONTROL_MOCK_H
#define LAMP_CONTROL_MOCK_H

#include "lamp_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reset the double (call counts zero, success result). */
void lamp_mock_reset(void);

/** @brief Number of lamp_control_force_inactive() calls recorded. */
unsigned lamp_mock_force_inactive_calls(void);

/** @brief Make lamp_control_force_inactive() return @p status. */
void lamp_mock_set_force_inactive_result(lamp_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* LAMP_CONTROL_MOCK_H */