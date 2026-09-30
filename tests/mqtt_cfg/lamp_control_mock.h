/**
 * @file lamp_control_mock.h
 * @brief Test-only lamp-control double for mqtt_cfg host tests (TASK-110)
 *
 * mqtt_cfg's fail-off contract is expressed through
 * lamp_control_force_inactive(), and the single re-enable transition
 * (verified TLS connected) through lamp_control_release_fail_off().  The
 * double records the calls so tests can assert that a configuration,
 * application or TLS-connection failure forces the output inactive and
 * never releases it, and that only a successful verified connect does.
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

/** @brief Number of lamp_control_release_fail_off() calls recorded. */
unsigned lamp_mock_release_calls(void);

/** @brief Make lamp_control_force_inactive() return @p status. */
void lamp_mock_set_force_inactive_result(lamp_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* LAMP_CONTROL_MOCK_H */