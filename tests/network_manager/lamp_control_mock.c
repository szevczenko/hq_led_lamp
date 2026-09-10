/**
 * @file lamp_control_mock.c
 * @brief Test-only lamp-control double (see lamp_control_mock.h).
 */

#include "lamp_control_mock.h"

/** @brief Only the fail-off operation is on the adapter's call surface. */
lamp_status_t lamp_control_force_inactive(void);

static unsigned       s_force_inactive_calls;
static lamp_status_t  s_force_inactive_result = LAMP_OK;

void lamp_mock_reset(void)
{
    s_force_inactive_calls  = 0U;
    s_force_inactive_result = LAMP_OK;
}

unsigned lamp_mock_force_inactive_calls(void)
{
    return s_force_inactive_calls;
}

void lamp_mock_set_force_inactive_result(lamp_status_t status)
{
    s_force_inactive_result = status;
}

lamp_status_t lamp_control_force_inactive(void)
{
    ++s_force_inactive_calls;
    return s_force_inactive_result;
}
