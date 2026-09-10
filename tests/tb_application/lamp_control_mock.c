/**
 * @file lamp_control_mock.c
 * @brief Test-only lamp-control double for tb_application host tests
 *        (TASK-112 / TASK-113) — see lamp_control_mock.h.
 *
 * Only the four functions the application module uses are implemented; they
 * are the exact extern symbols the production component references, so
 * linking this double INSTEAD of the real lamp_control component satisfies
 * every reference in tb_application.c without pulling in the HAL.
 */

#include "lamp_control_mock.h"

#include <string.h>

static unsigned      s_apply_calls;
static unsigned      s_force_inactive_calls;
static unsigned      s_release_calls;
static lamp_state_t  s_last_applied;
static bool          s_state_applied;
static lamp_status_t s_apply_result = LAMP_OK;
static lamp_status_t s_force_inactive_result = LAMP_OK;

void lamp_mock_reset(void)
{
    s_apply_calls = 0U;
    s_force_inactive_calls = 0U;
    s_release_calls = 0U;
    memset(&s_last_applied, 0, sizeof(s_last_applied));
    s_state_applied = false;
    s_apply_result = LAMP_OK;
    s_force_inactive_result = LAMP_OK;
}

unsigned lamp_mock_apply_calls(void)
{
    return s_apply_calls;
}

unsigned lamp_mock_force_inactive_calls(void)
{
    return s_force_inactive_calls;
}

unsigned lamp_mock_release_calls(void)
{
    return s_release_calls;
}

bool lamp_mock_state_applied(void)
{
    return s_state_applied;
}

bool lamp_mock_applied_power(void)
{
    return s_last_applied.power;
}

uint8_t lamp_mock_applied_brightness(void)
{
    return s_last_applied.brightness_percent;
}

void lamp_mock_set_apply_result(lamp_status_t status)
{
    s_apply_result = status;
}

void lamp_mock_set_force_inactive_result(lamp_status_t status)
{
    s_force_inactive_result = status;
}

/* --------------------------------------------------------------------- */
/* Implementations of the lamp_control surface the synchronizer uses      */
/* --------------------------------------------------------------------- */

lamp_status_t lamp_control_force_inactive(void)
{
    ++s_force_inactive_calls;
    return s_force_inactive_result;
}

lamp_status_t lamp_control_apply_state(const lamp_state_t *requested,
                                       lamp_applied_state_t *applied_out)
{
    if (requested == NULL)
    {
        return LAMP_ERR_INVALID_ARGUMENT;
    }

    ++s_apply_calls;
    if (s_apply_result != LAMP_OK)
    {
        return s_apply_result;
    }

    s_last_applied = *requested;
    s_state_applied = true;
    if (applied_out != NULL)
    {
        applied_out->power = s_last_applied.power;
        applied_out->brightness_percent = s_last_applied.brightness_percent;
        applied_out->output_active =
            s_last_applied.power && (s_last_applied.brightness_percent > 0u);
    }
    return LAMP_OK;
}

lamp_status_t lamp_control_get_applied_state(lamp_applied_state_t *applied_out)
{
    if (applied_out == NULL)
    {
        return LAMP_ERR_INVALID_ARGUMENT;
    }

    applied_out->power = s_last_applied.power;
    applied_out->brightness_percent = s_last_applied.brightness_percent;
    applied_out->output_active =
        s_last_applied.power && (s_last_applied.brightness_percent > 0u);
    return LAMP_OK;
}

lamp_status_t lamp_control_release_fail_off(void)
{
    ++s_release_calls;
    return LAMP_OK;
}