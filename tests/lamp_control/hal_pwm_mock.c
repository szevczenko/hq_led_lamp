/**
 * @file hal_pwm_mock.c
 * @brief Test-only HAL PWM double for lamp-control host tests (TASK-106)
 *
 * See hal_pwm_mock.h for the contract.  The double deliberately mirrors the
 * observable behavior of the real POSIX backend (src/hal/posix) and the
 * documented portable HAL contract, plus failure injection and call
 * counters that the real backend does not have.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_pwm_mock.h"

typedef struct hal_pwm_mock_slot {
    bool             initialized;
    hal_pin_t        pin;
    hal_polarity_t   polarity;
    uint32_t         frequency_hz;
    float            duty_percent;
    bool             forced_inactive;
} hal_pwm_mock_slot_t;

/**
 * @brief Single queued failure injection.
 *
 * Failures are queued so a scenario can make an entire escalation sequence
 * (e.g. force -> force retry -> 0 % duty) fail deterministically; each
 * entry is consumed by the next HAL call of the matching type.
 */
typedef struct hal_pwm_mock_fail_entry {
    hal_pwm_mock_call_t call;
    hal_status_t        status;
} hal_pwm_mock_fail_entry_t;

/** @brief Maximum number of queued failure injections. */
#define HAL_PWM_MOCK_FAIL_QUEUE_MAX 8u

typedef struct hal_pwm_mock_fail_queue {
    hal_pwm_mock_fail_entry_t entries[HAL_PWM_MOCK_FAIL_QUEUE_MAX];
    uint32_t                  count;
} hal_pwm_mock_fail_queue_t;

typedef struct hal_pwm_mock_counters {
    uint32_t init;
    uint32_t set_duty;
    uint32_t force_inactive;
    uint32_t deinit;
} hal_pwm_mock_counters_t;

static hal_pwm_mock_slot_t      s_slot;
static hal_pwm_mock_fail_queue_t s_failqueue;
static hal_pwm_mock_counters_t  s_counts;

static bool hal_pwm_mock_is_valid_duty(float duty_percent)
{
    /* NaN fails both comparisons; +-inf fail the bounds. */
    return (duty_percent >= HAL_PWM_DUTY_MIN_PERCENT) &&
           (duty_percent <= HAL_PWM_DUTY_MAX_PERCENT);
}

static int hal_pwm_mock_raw_for_logical(hal_polarity_t polarity, bool active)
{
    const bool invert = (polarity == HAL_POLARITY_ACTIVE_LOW);

    return (active != invert) ? 1 : 0;
}

static bool hal_pwm_mock_take_failure(hal_pwm_mock_call_t call,
                                      hal_status_t *status)
{
    uint32_t i;

    for (i = 0u; i < s_failqueue.count; i += 1u) {
        if (s_failqueue.entries[i].call != call) {
            continue;
        }
        *status = s_failqueue.entries[i].status;

        /* Remove the matched entry, compacting the queue. */
        for (; i + 1u < s_failqueue.count; i += 1u) {
            s_failqueue.entries[i] = s_failqueue.entries[i + 1u];
        }
        s_failqueue.count -= 1u;

        return true;
    }

    return false;
}

void hal_pwm_mock_reset(void)
{
    s_slot.initialized = false;
    s_slot.pin = HAL_PIN_NONE;
    s_slot.polarity = HAL_POLARITY_ACTIVE_HIGH;
    s_slot.frequency_hz = 0u;
    s_slot.duty_percent = HAL_PWM_DUTY_MIN_PERCENT;
    s_slot.forced_inactive = false;

    s_failqueue.count = 0u;

    s_counts.init = 0u;
    s_counts.set_duty = 0u;
    s_counts.force_inactive = 0u;
    s_counts.deinit = 0u;
}

void hal_pwm_mock_fail_next(hal_pwm_mock_call_t call, hal_status_t status)
{
    if (status == HAL_OK) {
        /* Documented escape hatch: clear all pending injections. */
        s_failqueue.count = 0u;
        return;
    }
    if (s_failqueue.count >= HAL_PWM_MOCK_FAIL_QUEUE_MAX) {
        return;
    }
    s_failqueue.entries[s_failqueue.count].call = call;
    s_failqueue.entries[s_failqueue.count].status = status;
    s_failqueue.count += 1u;
}

bool hal_pwm_mock_is_initialized(void)
{
    return s_slot.initialized;
}

uint32_t hal_pwm_mock_init_count(void)
{
    return s_counts.init;
}

uint32_t hal_pwm_mock_set_duty_count(void)
{
    return s_counts.set_duty;
}

uint32_t hal_pwm_mock_force_inactive_count(void)
{
    return s_counts.force_inactive;
}

uint32_t hal_pwm_mock_deinit_count(void)
{
    return s_counts.deinit;
}

bool hal_pwm_mock_is_forced_inactive(void)
{
    return s_slot.forced_inactive;
}

hal_polarity_t hal_pwm_mock_get_polarity(void)
{
    return s_slot.polarity;
}

hal_status_t hal_pwm_mock_get_duty(float *duty_percent)
{
    if (duty_percent == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (!s_slot.initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    *duty_percent = s_slot.duty_percent;

    return HAL_OK;
}

hal_status_t hal_pwm_mock_get_frequency(uint32_t *frequency_hz)
{
    if (frequency_hz == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (!s_slot.initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    *frequency_hz = s_slot.frequency_hz;

    return HAL_OK;
}

hal_status_t hal_pwm_mock_get_output(int *raw_level, bool *generating)
{
    bool active;

    if (raw_level == NULL || generating == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (!s_slot.initialized) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    *generating = !s_slot.forced_inactive;
    active = (*generating) && (s_slot.duty_percent > HAL_PWM_DUTY_MIN_PERCENT);
    *raw_level = hal_pwm_mock_raw_for_logical(s_slot.polarity, active);

    return HAL_OK;
}

/* --------------------------------------------------------------------- */
/* Portable HAL PWM public API                                            */
/* --------------------------------------------------------------------- */

hal_status_t hal_pwm_init(const hal_pwm_config_t *config)
{
    hal_status_t status;

    s_counts.init += 1u;

    if (hal_pwm_mock_take_failure(HAL_PWM_MOCK_CALL_INIT, &status)) {
        return status;
    }

    if (config == NULL) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (config->frequency_hz == 0u) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (config->polarity != HAL_POLARITY_ACTIVE_HIGH &&
        config->polarity != HAL_POLARITY_ACTIVE_LOW) {
        return HAL_ERR_INVALID_ARGUMENT;
    }
    if (config->pin == HAL_PIN_NONE) {
        return HAL_ERR_INVALID_PIN;
    }
    if (s_slot.initialized) {
        return HAL_ERR_ALREADY_INITIALIZED;
    }

    s_slot.initialized = true;
    s_slot.pin = config->pin;
    s_slot.polarity = config->polarity;
    s_slot.frequency_hz = config->frequency_hz;
    /* The output starts in the logical INACTIVE state. */
    s_slot.duty_percent = HAL_PWM_DUTY_MIN_PERCENT;
    s_slot.forced_inactive = false;

    return HAL_OK;
}

hal_status_t hal_pwm_set_duty(hal_pin_t pin, float duty_percent)
{
    hal_status_t status;

    s_counts.set_duty += 1u;

    if (hal_pwm_mock_take_failure(HAL_PWM_MOCK_CALL_SET_DUTY, &status)) {
        return status;
    }

    if (!hal_pwm_mock_is_valid_duty(duty_percent)) {
        return HAL_ERR_OUT_OF_RANGE;
    }
    if (!s_slot.initialized || s_slot.pin != pin) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    s_slot.duty_percent = duty_percent;
    s_slot.forced_inactive = false;

    return HAL_OK;
}

hal_status_t hal_pwm_force_inactive(hal_pin_t pin)
{
    hal_status_t status;

    s_counts.force_inactive += 1u;

    if (hal_pwm_mock_take_failure(HAL_PWM_MOCK_CALL_FORCE_INACTIVE, &status)) {
        return status;
    }

    if (!s_slot.initialized || s_slot.pin != pin) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    /* The previously set duty is retained; only generation is halted. */
    s_slot.forced_inactive = true;

    return HAL_OK;
}

hal_status_t hal_pwm_deinit(hal_pin_t pin)
{
    hal_status_t status;

    s_counts.deinit += 1u;

    if (hal_pwm_mock_take_failure(HAL_PWM_MOCK_CALL_DEINIT, &status)) {
        return status;
    }

    if (!s_slot.initialized || s_slot.pin != pin) {
        return HAL_ERR_NOT_INITIALIZED;
    }

    s_slot.initialized = false;
    s_slot.pin = HAL_PIN_NONE;
    s_slot.polarity = HAL_POLARITY_ACTIVE_HIGH;
    s_slot.frequency_hz = 0u;
    s_slot.duty_percent = HAL_PWM_DUTY_MIN_PERCENT;
    s_slot.forced_inactive = false;

    return HAL_OK;
}