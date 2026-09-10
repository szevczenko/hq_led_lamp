/**
 * @file lamp_control.h
 * @brief Hardware-independent LED lamp state policy (TASK-106)
 *
 * Normative public API for the lamp-control domain component.
 *
 * Scope
 * -----
 * This component owns the validated LED policy for a single lamp output:
 *   - a validated requested state (`power` + `brightness_percent`),
 *   - the state that was actually applied to the PWM output,
 *   - conversion of brightness into a normalized PWM duty using
 *     overflow-safe integer arithmetic,
 *   - a fail-off contract: the output is off before initialization and
 *     after every rejected input or hardware failure.
 *
 * The component is hardware-independent: it speaks only the portable HAL
 * PWM contract (hal_pwm.h / hal_types.h) and never references ESP-IDF
 * LEDC/GPIO types, headers or driver symbols.  All target-specific details
 * (timer/channel allocation, pad inversion, frequency limits) stay inside
 * the HAL backend selected by the platform.
 *
 * State semantics
 * ---------------
 *   - `brightness_percent` is a closed interval `0..100`.
 *   - `power == false` always produces the electrical off duty cycle.
 *   - `power == true` with `brightness_percent == 0` also produces the off
 *     duty cycle, but the requested power state is preserved in the applied
 *     state so the protocol view stays consistent.
 *   - Invalid input at the boundary (values above 100, NULL pointers) is
 *     rejected with an error; it is never wrapped, clamped or truncated.
 *
 * Duty semantics
 * --------------
 * The normalized PWM duty is an unsigned fixed-point percentage in units of
 * `1/100` of a percent (see #LAMP_DUTY_SCALE):
 *
 *   - #LAMP_DUTY_MIN (0)     == 0.00 %  (electrically off),
 *   - #LAMP_DUTY_MAX (10000) == 100.00 % (electrically on for the whole
 *     period),
 *   - `duty / 100.0` is the normalized duty cycle percentage handed to
 *     hal_pwm_set_duty().
 *
 * The scale is an implementation detail of this component and is independent
 * of the ESP LEDC resolution; the resolution selected by the platform never
 * reaches this API.
 *
 * Lifecycle
 * ---------
 *   - The single lamp output is initialized once with lamp_control_init()
 *     before any other operation, and released with lamp_control_deinit().
 *   - Initialization starts the PWM output at the configured logical
 *     INACTIVE level and holds it there; the output is not enabled until the
 *     first valid lamp_control_apply_state() call.
 *   - Calling an operation before init (or after deinit) returns
 *     #LAMP_ERR_NOT_INITIALIZED.  Re-initialization after a successful
 *     deinit is allowed.
 *   - Every failure path (invalid input, HAL error) forces the output back
 *     to the logical INACTIVE level where electrically possible, so a lamp
 *     never stays on after a rejected or failed transition.  If even the
 *     fail-off escalation (retry + 0 % duty fallback) cannot prove the
 *     output off, the operation returns #LAMP_ERR_FAIL_OFF and the applied
 *     state does not clear `output_active`, so a failed safety action is
 *     never reported as a successful shutdown.
 *
 * Polarity
 * --------
 * The active polarity of the output is selected at initialization through
 * #lamp_control_config_t::polarity (part of the portable HAL polarity
 * contract).  The product layer derives it from the compile-time
 * `KLC_LED_PWM_ACTIVE_LOW` Kconfig option; this component stays free of
 * product-specific configuration so it remains host-testable as-is.
 */

#ifndef LAMP_CONTROL_H
#define LAMP_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_pwm.h"
#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* lamp_status_t - portable result/error type                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Portable result/error type returned by every public operation.
 *
 * #LAMP_OK (0) signals success; every other value is an error code.
 * Values are stable: callers and tests compare against the symbolic names
 * and must not hard-code raw integers other than the documented ones.
 */
typedef enum lamp_status {
    LAMP_OK                      = 0,   /**< Operation completed successfully. */
    LAMP_ERR_INVALID_ARGUMENT    = -1,  /**< Invalid argument (NULL pointer, out-of-range enum, ...). */
    LAMP_ERR_OUT_OF_RANGE        = -2,  /**< Numeric input outside its documented range (e.g. brightness). */
    LAMP_ERR_NOT_INITIALIZED     = -3,  /**< Operation on a lamp that was not initialized or was already deinitialized. */
    LAMP_ERR_ALREADY_INITIALIZED = -4,  /**< Initialization requested while already initialized. */
    LAMP_ERR_NOT_SUPPORTED       = -5,  /**< Requested configuration is not supported by the HAL backend. */
    LAMP_ERR_NO_RESOURCE         = -6,  /**< No HAL resource available (timer/channel). */
    LAMP_ERR_INTERNAL            = -7,  /**< Unexpected internal or HAL backend failure. */
    LAMP_ERR_FAIL_OFF            = -8,  /**< The fail-off (safety) action could not be completed; the output may still be active. */
    LAMP_ERR_BLOCKED_BY_FAIL_OFF = -9   /**< The request was rejected because a fail-off barrier is latched (see lamp_control_release_fail_off()). */
} lamp_status_t;

/* --------------------------------------------------------------------- */
/* Brightness and duty constants                                         */
/* --------------------------------------------------------------------- */

/** @brief Minimum valid brightness (0 %; electrically off). */
#define LAMP_BRIGHTNESS_MIN 0u

/** @brief Maximum valid brightness (100 %; full electrical on). */
#define LAMP_BRIGHTNESS_MAX 100u

/** @brief Normalized duty type: fixed-point percentage in 1/100 % units. */
typedef uint32_t lamp_duty_t;

/** @brief Normalized duty units per percentage point (1/100 % per unit). */
#define LAMP_DUTY_SCALE 100u

/** @brief Minimum normalized duty (0.00 %; electrically off). */
#define LAMP_DUTY_MIN ((lamp_duty_t)0u)

/** @brief Maximum normalized duty (100.00 %; electrically on). */
#define LAMP_DUTY_MAX ((lamp_duty_t)(LAMP_BRIGHTNESS_MAX * LAMP_DUTY_SCALE))

/* --------------------------------------------------------------------- */
/* State types                                                           */
/* --------------------------------------------------------------------- */

/**
 * @brief Validated requested lamp state.
 *
 * The input to lamp_control_apply_state().  All fields are validated: any
 * `brightness_percent` outside `0..100` is rejected with
 * #LAMP_ERR_OUT_OF_RANGE and never wrapped or clamped.
 */
typedef struct lamp_state {
    bool      power;              /**< Requested power state (true = on). */
    uint8_t   brightness_percent; /**< Requested brightness, 0..100. */
} lamp_state_t;

/**
 * @brief State that was successfully applied to the PWM output.
 *
 * Returned/updated by lamp_control_apply_state() and readable at any time
 * with lamp_control_get_applied_state().  `power` and `brightness_percent`
 * mirror the last accepted requested state; `output_active` reports whether
 * the lamp output is *electrically* on, which is false both when
 * `power == false` and when `power == true` with brightness 0.
 */
typedef struct lamp_applied_state {
    bool      power;              /**< Last accepted requested power state. */
    uint8_t   brightness_percent; /**< Last accepted requested brightness, 0..100. */
    bool      output_active;      /**< Electrically on (power && brightness > 0). */
} lamp_applied_state_t;

/* --------------------------------------------------------------------- */
/* Configuration                                                         */
/* --------------------------------------------------------------------- */

/**
 * @brief Lamp-control initialization configuration.
 *
 * Supplied to lamp_control_init().  The product layer builds this from the
 * compile-time Kconfig options; this component only forwards the values to
 * the portable HAL PWM contract.
 */
typedef struct lamp_control_config {
    hal_pin_t      pin;          /**< PWM output pin.  Must not be #HAL_PIN_NONE. */
    uint32_t       frequency_hz; /**< PWM frequency in hertz.  Must be non-zero. */
    hal_polarity_t polarity;     /**< Active polarity of the output (HAL polarity contract). */
} lamp_control_config_t;

/* --------------------------------------------------------------------- */
/* Duty conversion                                                       */
/* --------------------------------------------------------------------- */

/**
 * @brief Convert a validated brightness percentage into a normalized duty.
 *
 * Exact integer conversion on the #LAMP_DUTY_SCALE fixed point:
 *
 *     duty = brightness_percent * LAMP_DUTY_SCALE
 *
 * computed with a 64-bit intermediate so the multiplication can never
 * overflow before the result is range-checked (overflow-safe integer
 * arithmetic; the result is always in #LAMP_DUTY_MIN..LAMP_DUTY_MAX).
 *
 * @param[in]  brightness_percent Brightness in percent, 0..100.
 * @param[out] duty_out           Non-NULL pointer receiving the normalized
 *                                duty in #LAMP_DUTY_SCALE units.
 *
 * @return
 *  - #LAMP_OK on success and @c *duty_out is valid,
 *  - #LAMP_ERR_INVALID_ARGUMENT if @p duty_out is NULL,
 *  - #LAMP_ERR_OUT_OF_RANGE if @p brightness_percent is outside `0..100`
 *    (rejected, never wrapped).
 */
lamp_status_t lamp_duty_from_brightness(uint8_t brightness_percent,
                                        lamp_duty_t *duty_out);

/**
 * @brief Convert a normalized duty back into a brightness percentage.
 *
 * Round-half-up inverse of lamp_duty_from_brightness():
 *
 *     brightness = (duty + LAMP_DUTY_SCALE / 2) / LAMP_DUTY_SCALE
 *
 * for example a duty of 3350 (33.50 %) rounds to 34 and a duty of 3349
 * (33.49 %) rounds to 33.  Used to report the brightness that corresponds
 * to a commanded duty.  `duty == LAMP_DUTY_MAX` maps to `100`.
 *
 * @param[in]  duty             Normalized duty in #LAMP_DUTY_SCALE units,
 *                              0..LAMP_DUTY_MAX.
 * @param[out] brightness_out   Non-NULL pointer receiving the brightness in
 *                              percent, 0..100.
 *
 * @return
 *  - #LAMP_OK on success and @c *brightness_out is valid,
 *  - #LAMP_ERR_INVALID_ARGUMENT if @p brightness_out is NULL,
 *  - #LAMP_ERR_OUT_OF_RANGE if @p duty is above #LAMP_DUTY_MAX (rejected,
 *    never wrapped).
 */
lamp_status_t lamp_brightness_from_duty(lamp_duty_t duty,
                                        uint8_t *brightness_out);

/* --------------------------------------------------------------------- */
/* Lifecycle and state operations                                        */
/* --------------------------------------------------------------------- */

/**
 * @brief Initialize the lamp PWM output.
 *
 * Initializes the PWM output through the portable HAL with the configured
 * frequency and polarity.  The HAL starts the output at the logical
 * INACTIVE level; this function additionally forces the INACTIVE level
 * before the output is enabled, so the electrical output is off directly
 * after a successful init and stays off until the first valid
 * lamp_control_apply_state() call.
 *
 * @param[in] config Non-NULL pointer to the lamp configuration.
 *
 * @return
 *  - #LAMP_OK on success; the lamp is initialized and its output is off,
 *  - #LAMP_ERR_INVALID_ARGUMENT if @p config is NULL, @c config->pin is
 *    #HAL_PIN_NONE, @c config->frequency_hz is zero, or the polarity is out
 *    of range,
 *  - #LAMP_ERR_ALREADY_INITIALIZED if already initialized (the existing
 *    configuration is left unchanged),
 *  - #LAMP_ERR_NOT_SUPPORTED / #LAMP_ERR_NO_RESOURCE / #LAMP_ERR_INTERNAL
 *    mapped from the underlying HAL failure; the output is off,
 *  - #LAMP_ERR_INTERNAL if the post-init INACTIVE hold fails: the strongest
 *    available fail-off is performed and the HAL init is rolled back with a
 *    deinit before returning.  If that rollback deinit also fails, the lamp
 *    remains in a cleanable state (`initialized` and `pin` are preserved)
 *    so a later lamp_control_deinit() retries the release.
 */
lamp_status_t lamp_control_init(const lamp_control_config_t *config);

/**
 * @brief Validate and apply a requested lamp state.
 *
 * Rejects any requested state whose brightness is outside `0..100` with
 * #LAMP_ERR_OUT_OF_RANGE (never wraps) and any NULL pointer with
 * #LAMP_ERR_INVALID_ARGUMENT; both leave the applied state unchanged and
 * force the output to the logical INACTIVE level.
 *
 * On success the requested state is converted to a normalized duty and
 * applied with hal_pwm_set_duty():
 *   - `power == false` or `brightness_percent == 0` selects duty 0.00 %
 *     (electrically off; the requested power state is still recorded),
 *   - otherwise the duty is `brightness_percent * LAMP_DUTY_SCALE`.
 * If the HAL reports a failure the output is forced INACTIVE (fail-off)
 * and the mapped error is returned.  The fail-off itself uses a best-effort
 * escalation (retry, then 0 % duty fallback); if that escalation cannot
 * prove the output off the operation returns #LAMP_ERR_FAIL_OFF instead.
 *
 * @param[in]  requested   Non-NULL pointer to the requested state.
 * @param[out] applied_out Optional pointer receiving the applied state on
 *                         success (may be NULL).
 *
 * @return
 *  - #LAMP_OK on success and @c *applied_out (when non-NULL) holds the
 *    applied state,
 *  - #LAMP_ERR_NOT_INITIALIZED if the lamp is not initialized,
 *  - #LAMP_ERR_INVALID_ARGUMENT if @p requested is NULL,
 *  - #LAMP_ERR_OUT_OF_RANGE if the requested brightness is outside 0..100;
 *    the output is forced INACTIVE,
 *  - a HAL error mapped to #lamp_status_t on a hardware failure; the
 *    output is forced INACTIVE,
 *  - #LAMP_ERR_FAIL_OFF if the fail-off escalation itself failed and the
 *    output could not be proven off (`output_active` is not cleared).
 */
lamp_status_t lamp_control_apply_state(const lamp_state_t *requested,
                                       lamp_applied_state_t *applied_out);

/**
 * @brief Force the lamp output to the logical INACTIVE level.
 *
 * Halts PWM generation at the configured logical INACTIVE level
 * (fail-off).  The last accepted requested power/brightness are retained in
 * the applied state (only `output_active` drops to false), so a later
 * lamp_control_apply_state() can resume from a consistent protocol view.
 * The fail-off is best-effort with escalation (retry, then 0 % duty
 * fallback); only when the HAL proves the output inactive is #LAMP_OK
 * returned.
 *
 * Fail-off barrier (disconnect endpoint): on success this operation also
 * latches a barrier so that every subsequent lamp_control_apply_state()
 * is rejected with #LAMP_ERR_BLOCKED_BY_FAIL_OFF — an apply that was
 * already waiting for the lock or that arrives later can never re-energize
 * the output behind the fail-off's back.  Only the explicit re-enable
 * transition lamp_control_release_fail_off() (or a fresh
 * lamp_control_init() after lamp_control_deinit()) lifts the barrier.
 *
 * @return
 *  - #LAMP_OK on success; the output is off and the fail-off barrier is
 *    latched until lamp_control_release_fail_off(),
 *  - #LAMP_ERR_NOT_INITIALIZED if the lamp is not initialized,
 *  - #LAMP_ERR_FAIL_OFF if the fail-off escalation failed: the output could
 *    not be proven off, `output_active` is not cleared (it keeps its last
 *    known value), the barrier stays unchanged and the lamp stays
 *    initialized so the fail-off or the release can be retried.
 */
lamp_status_t lamp_control_force_inactive(void);

/**
 * @brief Explicit re-enable transition after a fail-off barrier.
 *
 * Clears the fail-off barrier latched by lamp_control_force_inactive(), so
 * subsequent lamp_control_apply_state() calls are accepted again.  This is
 * the documented reconnect/reenable transition: the owner of the lamp
 * state (the application state machine, e.g. on network reconnect) decides
 * when the output may be energized again; a fail-off always wins over any
 * concurrent or later apply until this function runs.
 *
 * The function is idempotent and does not touch the hardware or the
 * applied state: it only lifts the barrier.  After it returns, the
 * caller applies a fresh desired state with lamp_control_apply_state().
 *
 * @return #LAMP_OK always (the release cannot fail).
 */
lamp_status_t lamp_control_release_fail_off(void);

/**
 * @brief Deinitialize the lamp and release the PWM output.
 *
 * Releases the HAL PWM resource.  After a successful deinit the output is
 * off (the HAL releases the pin) and the lamp must be initialized again
 * before any other operation.  If the HAL deinit fails, the output is
 * forced INACTIVE (best-effort escalation) before the mapped error is
 * returned, and the lamp stays initialized so the release can be retried.
 *
 * @return
 *  - #LAMP_OK on success; the lamp is no longer initialized,
 *  - #LAMP_ERR_NOT_INITIALIZED if the lamp is not initialized,
 *  - a HAL error mapped to #lamp_status_t on a hardware failure; the
 *    output is forced INACTIVE,
 *  - #LAMP_ERR_FAIL_OFF if the fail-off escalation failed; the output could
 *    not be proven off and the lamp stays initialized.
 */
lamp_status_t lamp_control_deinit(void);

/**
 * @brief Read the state currently applied to the lamp output.
 *
 * @param[out] applied_out Non-NULL pointer receiving the applied state.
 *
 * @return
 *  - #LAMP_OK on success and @c *applied_out is valid,
 *  - #LAMP_ERR_INVALID_ARGUMENT if @p applied_out is NULL,
 *  - #LAMP_ERR_NOT_INITIALIZED if the lamp is not initialized.
 */
lamp_status_t lamp_control_get_applied_state(lamp_applied_state_t *applied_out);

#ifdef __cplusplus
}
#endif

#endif /* LAMP_CONTROL_H */