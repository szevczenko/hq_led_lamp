/**
 * @file hal_pwm_mock.h
 * @brief Test-only HAL PWM double for lamp-control host tests (TASK-106)
 *
 * Implements the public portable HAL PWM contract (hal_pwm.h) as an
 * in-memory test double so the lamp-control component can be exercised on
 * the host without any platform backend:
 *
 *   - the double tracks one initialization slot with the configured pin,
 *     frequency and polarity,
 *   - hal_pwm_set_duty() stores the normalized duty (percent) and resumes
 *     generation; hal_pwm_force_inactive() halts generation at the logical
 *     INACTIVE level; hal_pwm_deinit() releases the slot,
 *   - the raw electrical output level is derived with the portable
 *     polarity truth table (ACTIVE_HIGH: logical active -> physical HIGH,
 *     ACTIVE_LOW: logical active -> physical LOW), so tests can verify the
 *     "off" guarantees for both polarities through
 *     hal_pwm_mock_get_output(),
 *   - failure injection lets tests flip any single HAL call to a chosen
 *     hal_status_t so every lamp-control failure/fail-off path is
 *     reachable deterministically,
 *   - call counters expose how often each HAL operation was invoked, so
 *     tests can assert that a failure path also performed the fail-off
 *     (force-inactive) call.
 *
 * This file is a test double only: it is compiled solely into the
 * lamp-control host test binary and is never part of any production build.
 */

#ifndef HAL_PWM_MOCK_H
#define HAL_PWM_MOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_pwm.h"
#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The HAL call whose next invocation fails.
 */
typedef enum hal_pwm_mock_call {
    HAL_PWM_MOCK_CALL_INIT = 0,          /**< hal_pwm_init(). */
    HAL_PWM_MOCK_CALL_SET_DUTY = 1,      /**< hal_pwm_set_duty(). */
    HAL_PWM_MOCK_CALL_FORCE_INACTIVE = 2,/**< hal_pwm_force_inactive(). */
    HAL_PWM_MOCK_CALL_DEINIT = 3         /**< hal_pwm_deinit(). */
} hal_pwm_mock_call_t;

/**
 * @brief Reset all mock state: slot, fail injection and call counters.
 *
 * Call between independent test scenarios.
 */
void hal_pwm_mock_reset(void);

/**
 * @brief Make the next invocation of @p call return @p status.
 *
 * Multiple injections may be queued (up to #HAL_PWM_MOCK_FAIL_QUEUE_MAX);
 * each entry is consumed by the next HAL call of the matching type, which
 * lets a scenario fail an entire recovery/escalation sequence
 * deterministically.  Pass #HAL_OK to clear all pending injections.
 */
void hal_pwm_mock_fail_next(hal_pwm_mock_call_t call, hal_status_t status);

/** @brief Whether the double holds an initialized PWM slot. */
bool hal_pwm_mock_is_initialized(void);

/** @brief Number of hal_pwm_init() calls observed. */
uint32_t hal_pwm_mock_init_count(void);

/** @brief Number of hal_pwm_set_duty() calls observed. */
uint32_t hal_pwm_mock_set_duty_count(void);

/** @brief Number of hal_pwm_force_inactive() calls observed. */
uint32_t hal_pwm_mock_force_inactive_count(void);

/** @brief Number of hal_pwm_deinit() calls observed. */
uint32_t hal_pwm_mock_deinit_count(void);

/** @brief Whether the slot is currently held inactive by force_inactive(). */
bool hal_pwm_mock_is_forced_inactive(void);

/** @brief Polarity retained by the initialized slot. */
hal_polarity_t hal_pwm_mock_get_polarity(void);

/**
 * @brief Read the stored normalized duty (percent).
 *
 * Same contract as hal_posix_pwm_get_duty(): returns the duty value, which
 * is retained across hal_pwm_force_inactive().
 */
hal_status_t hal_pwm_mock_get_duty(float *duty_percent);

/**
 * @brief Read the frequency retained by the initialized slot.
 */
hal_status_t hal_pwm_mock_get_frequency(uint32_t *frequency_hz);

/**
 * @brief Read the raw electrical output level and generation state.
 *
 * Same contract as hal_posix_pwm_get_output_state():
 *   - while forced inactive, or duty == 0.0 %, the line is held at the raw
 *     level of the logical INACTIVE state for the configured polarity,
 *   - while generating with a non-zero duty, the line is at the raw level
 *     of the logical ACTIVE state (the active phase of the PWM cycle).
 *
 * @return #HAL_OK on success, #HAL_ERR_NOT_INITIALIZED when the slot is
 *         empty (i.e. the output does not exist and is therefore off).
 */
hal_status_t hal_pwm_mock_get_output(int *raw_level, bool *generating);

#ifdef __cplusplus
}
#endif

#endif /* HAL_PWM_MOCK_H */