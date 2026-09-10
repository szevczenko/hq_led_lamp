/**
 * @file lamp_control.c
 * @brief Hardware-independent LED lamp state policy (TASK-106)
 *
 * Single-lamp implementation of the lamp-control domain component.  It owns
 * the validated LED policy (requested/applied state, duty conversion,
 * fail-off) and drives the PWM output exclusively through the portable HAL
 * PWM contract (hal_pwm.h); it contains no ESP-IDF, LEDC or GPIO code.
 *
 * Fail-off contract
 * -----------------
 * The electrical output is off:
 *   - before any hal_pwm_init() (no output exists),
 *   - directly after a successful init (the HAL starts at the logical
 *     INACTIVE level and the post-init hold below keeps it there),
 *   - after every rejected or failed transition: invalid requested states
 *     and HAL failures all route through the fail-off helper, which forces
 *     the logical INACTIVE level (with a retry and a 0 % duty escalation)
 *     and only records the output as inactive once the HAL confirms it.
 *
 * When even the escalation cannot prove the output off, the failing
 * operation returns #LAMP_ERR_FAIL_OFF and the applied state keeps
 * `output_active` at its last known value instead of claiming a drop that
 * never happened; the lamp remains initialized so the fail-off and the
 * release can be retried.
 *
 * Concurrency
 * -----------
 * The module is a singleton (one physical lamp output).  It serializes all
 * state-touching operations internally (including force-inactive, which the
 * network adapter drives from the Wi-Fi worker on disconnect), so concurrent
 * calls can never corrupt the applied state or race the fail-off.
 *
 * Fail-off priority goes beyond serialization: a successful explicit
 * lamp_control_force_inactive() latches a fail-off barrier, and every
 * subsequent lamp_control_apply_state() is rejected with
 * #LAMP_ERR_BLOCKED_BY_FAIL_OFF (re-forcing the output inactive) until the
 * caller performs the explicit re-enable transition with
 * lamp_control_release_fail_off().  An apply that is already waiting for
 * the lock — or that arrives later — therefore can never re-energize the
 * output behind a disconnect's back; the barrier is the disconnect
 * endpoint, and only the documented re-enable transition lifts it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "lamp_control.h"

/* --------------------------------------------------------------------- */
/* Internal state                                                        */
/* --------------------------------------------------------------------- */

/**
 * @brief Runtime state of the single lamp output.
 */
typedef struct lamp_control_state {
    bool                 initialized; /**< PWM output initialized and not deinitialized. */
    hal_pin_t            pin;         /**< PWM pin; only valid while initialized. */
    lamp_applied_state_t applied;     /**< Last state successfully applied. */
    bool                 fail_off_barrier; /**< Fail-off barrier latched by the
                                            last successful explicit
                                            force-inactive; blocks apply-state
                                            until lamp_control_release_fail_off()
                                            (see lamp_control.h). */
} lamp_control_state_t;

static lamp_control_state_t s_lamp = {
    .initialized      = false,
    .pin              = HAL_PIN_NONE,
    .applied          = { false, LAMP_BRIGHTNESS_MIN, false },
    .fail_off_barrier = false,
};

/* --------------------------------------------------------------------- */
/* Internal serialization lock                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Internal lock serializing every state-touching operation.
 *
 * The singleton lamp API is called from multiple threads in the product
 * (the application/protocol path applies states while the network adapter
 * drives the immediate fail-off from the Wi-Fi worker on disconnect), so
 * all operations that read or write @c s_lamp — including
 * lamp_control_force_inactive() — serialize against this lock.  It is a
 * plain atomic spin lock: critical sections only touch the state struct
 * and issue at most one HAL call, so they are short by construction.
 *
 * Ordering guarantee: a fail-off that begins after an apply-state started
 * on another thread either waits for the apply to finish (and then forces
 * the output off with consistent bookkeeping) or completes entirely before
 * the apply starts.  A partially applied state is never observable, so a
 * disconnect racing a concurrent apply-state always ends with an inactive
 * output and uncorrupted applied state.
 */
static atomic_flag s_lamp_lock = ATOMIC_FLAG_INIT;

/** @brief Acquire the internal lamp serialization lock. */
static void lamp_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_lamp_lock,
                                             memory_order_acquire)) {
        /* Spin: critical sections are single-HAL-call short. */
    }
}

/** @brief Release the internal lamp serialization lock. */
static void lamp_unlock(void)
{
    atomic_flag_clear_explicit(&s_lamp_lock, memory_order_release);
}

/* --------------------------------------------------------------------- */
/* Helpers                                                               */
/* --------------------------------------------------------------------- */

/**
 * @brief Map a portable HAL status onto the lamp-control status type.
 *
 * HAL error codes are faithfully forwarded so callers (and tests) can tell
 * an invalid configuration apart from a resource failure.  #HAL_ERR_INVALID_PIN
 * is folded into #LAMP_ERR_INVALID_ARGUMENT because the only pin the module
 * ever passes to the HAL comes from the validated configuration.
 */
static lamp_status_t lamp_map_hal_status(hal_status_t status)
{
    switch (status) {
    case HAL_OK:
        return LAMP_OK;
    case HAL_ERR_INVALID_ARGUMENT:
        return LAMP_ERR_INVALID_ARGUMENT;
    case HAL_ERR_INVALID_PIN:
        return LAMP_ERR_INVALID_ARGUMENT;
    case HAL_ERR_OUT_OF_RANGE:
        return LAMP_ERR_OUT_OF_RANGE;
    case HAL_ERR_NOT_INITIALIZED:
        return LAMP_ERR_NOT_INITIALIZED;
    case HAL_ERR_ALREADY_INITIALIZED:
        return LAMP_ERR_ALREADY_INITIALIZED;
    case HAL_ERR_NOT_SUPPORTED:
        return LAMP_ERR_NOT_SUPPORTED;
    case HAL_ERR_NO_RESOURCE:
        return LAMP_ERR_NO_RESOURCE;
    default:
        return LAMP_ERR_INTERNAL;
    }
}

/**
 * @brief Validate a requested state without touching the hardware.
 *
 * Only `brightness_percent` has a numeric range; `power` is a boolean and
 * cannot be out of range.  The lower bound (0) is implied by the unsigned
 * `uint8_t` field, so only the upper bound can be violated.  Boundary
 * violations are rejected, never wrapped.
 */
static bool lamp_state_is_valid(const lamp_state_t *state)
{
    return state->brightness_percent <= LAMP_BRIGHTNESS_MAX;
}

/**
 * @brief Internal fail-off used by every failure path.
 *
 * Best-effort escalation that drives the output to the logical INACTIVE
 * level and, only once the HAL has confirmed it, records the electrical
 * drop in the applied state (the last accepted requested power/brightness
 * are retained for protocol consistency):
 *   1. hal_pwm_force_inactive() — halt generation at the INACTIVE level,
 *   2. a single retry of the same call (transient-failure recovery),
 *   3. hal_pwm_set_duty(pin, 0.0 %) — the HAL contract guarantees that a
 *      0 % duty is permanently INACTIVE (electrically off).
 *
 * If every attempt fails the output cannot be proven off: `output_active`
 * is preserved at its last known value (a "may still be active" read) and
 * #LAMP_ERR_FAIL_OFF is returned.  This lets every caller distinguish a
 * failed safety action from a successfully inactive output instead of
 * swallowing a second HAL failure.
 */
static lamp_status_t lamp_fail_off(void)
{
    hal_status_t hs;

    hs = hal_pwm_force_inactive(s_lamp.pin);
    if (hs != HAL_OK) {
        hs = hal_pwm_force_inactive(s_lamp.pin);
    }
    if (hs != HAL_OK) {
        hs = hal_pwm_set_duty(s_lamp.pin, HAL_PWM_DUTY_MIN_PERCENT);
    }

    if (hs != HAL_OK) {
        return LAMP_ERR_FAIL_OFF;
    }

    s_lamp.applied.output_active = false;

    return LAMP_OK;
}

/* --------------------------------------------------------------------- */
/* Duty conversion                                                       */
/* --------------------------------------------------------------------- */

lamp_status_t lamp_duty_from_brightness(uint8_t brightness_percent,
                                        lamp_duty_t *duty_out)
{
    uint64_t scaled;

    if (duty_out == NULL) {
        return LAMP_ERR_INVALID_ARGUMENT;
    }
    if (brightness_percent > LAMP_BRIGHTNESS_MAX) {
        return LAMP_ERR_OUT_OF_RANGE;
    }

    /* Overflow-safe integer arithmetic: the multiplication happens in 64-bit
     * and the result is range-checked before narrowing.  For the validated
     * brightness range the result is always in [0, 10000], so the cast below
     * can never truncate. */
    scaled = (uint64_t)brightness_percent * (uint64_t)LAMP_DUTY_SCALE;
    if (scaled > (uint64_t)LAMP_DUTY_MAX) {
        return LAMP_ERR_OUT_OF_RANGE;
    }

    *duty_out = (lamp_duty_t)scaled;

    return LAMP_OK;
}

lamp_status_t lamp_brightness_from_duty(lamp_duty_t duty,
                                        uint8_t *brightness_out)
{
    uint64_t rounded;

    if (brightness_out == NULL) {
        return LAMP_ERR_INVALID_ARGUMENT;
    }
    if (duty > LAMP_DUTY_MAX) {
        return LAMP_ERR_OUT_OF_RANGE;
    }

    /* Round-half-up on the fixed-point scale: a duty of 3350 (33.50 %)
     * reports 34, a duty of 3349 (33.49 %) reports 33.  `duty` is capped at
     * LAMP_DUTY_MAX so the 64-bit intermediate cannot overflow. */
    rounded = ((uint64_t)duty + (uint64_t)(LAMP_DUTY_SCALE / 2u)) /
              (uint64_t)LAMP_DUTY_SCALE;

    *brightness_out = (uint8_t)rounded;

    return LAMP_OK;
}

/* --------------------------------------------------------------------- */
/* Lifecycle and state operations                                        */
/* --------------------------------------------------------------------- */

static lamp_status_t lamp_control_init_locked(const lamp_control_config_t *config)
{
    hal_pwm_config_t pwm_cfg;
    hal_status_t hs;

    if (config == NULL) {
        return LAMP_ERR_INVALID_ARGUMENT;
    }
    if (s_lamp.initialized) {
        return LAMP_ERR_ALREADY_INITIALIZED;
    }

    /* Validate the whole boundary up front (reject, never wrap/truncate). */
    if (config->pin == HAL_PIN_NONE) {
        return LAMP_ERR_INVALID_ARGUMENT;
    }
    if (config->frequency_hz == 0u) {
        return LAMP_ERR_INVALID_ARGUMENT;
    }
    if (config->polarity != HAL_POLARITY_ACTIVE_HIGH &&
        config->polarity != HAL_POLARITY_ACTIVE_LOW) {
        return LAMP_ERR_INVALID_ARGUMENT;
    }

    pwm_cfg.pin = config->pin;
    pwm_cfg.frequency_hz = config->frequency_hz;
    pwm_cfg.polarity = config->polarity;

    hs = hal_pwm_init(&pwm_cfg);
    if (hs != HAL_OK) {
        return lamp_map_hal_status(hs);
    }

    s_lamp.initialized = true;
    s_lamp.pin = config->pin;
    s_lamp.applied.power = false;
    s_lamp.applied.brightness_percent = LAMP_BRIGHTNESS_MIN;
    s_lamp.applied.output_active = false;
    /* A fresh initialization clears any fail-off barrier latched before a
     * deinit: initialization is the re-enable transition for a
     * deinitialized lamp, so re-initialization starts clean. */
    s_lamp.fail_off_barrier = false;

    /* Initialize PWM at the configured inactive level before enabling it:
     * the HAL contract already starts the output at the logical INACTIVE
     * level, and this explicit hold guarantees the output stays off until
     * the first valid apply_state() call regardless of backend behavior.
     * If the hold fails the strongest available fail-off is performed and
     * the HAL init is then rolled back so a later init attempt starts from
     * a clean, fully-off state. */
    hs = hal_pwm_force_inactive(s_lamp.pin);
    if (hs != HAL_OK) {
        /* Best-effort fail-off before releasing the resource. */
        (void)lamp_fail_off();

        /* Roll the HAL init back.  If the rollback deinit also fails the
         * HAL resource is still held: preserve `initialized` and `pin` so a
         * later lamp_control_deinit() retries the release, and return the
         * original hold error (the call failed, but the lamp stays
         * cleanable instead of leaking the pin). */
        if (hal_pwm_deinit(s_lamp.pin) != HAL_OK) {
            return lamp_map_hal_status(hs);
        }

        s_lamp.initialized = false;
        s_lamp.pin = HAL_PIN_NONE;
        s_lamp.applied.power = false;
        s_lamp.applied.brightness_percent = LAMP_BRIGHTNESS_MIN;
        s_lamp.applied.output_active = false;
        return lamp_map_hal_status(hs);
    }

    return LAMP_OK;
}

lamp_status_t lamp_control_init(const lamp_control_config_t *config)
{
    lamp_status_t status;

    lamp_lock();
    status = lamp_control_init_locked(config);
    lamp_unlock();

    return status;
}

static lamp_status_t lamp_control_apply_state_locked(
    const lamp_state_t *requested, lamp_applied_state_t *applied_out)
{
    lamp_duty_t duty;
    float duty_percent;
    hal_status_t hs;

    if (!s_lamp.initialized) {
        return LAMP_ERR_NOT_INITIALIZED;
    }
    if (requested == NULL) {
        /* A rejected NULL request is still a failure path.  Once the PWM
         * resource exists, fail safe before returning the argument error so
         * an already-active lamp cannot remain energized. */
        if (lamp_fail_off() != LAMP_OK) {
            return LAMP_ERR_FAIL_OFF;
        }
        return LAMP_ERR_INVALID_ARGUMENT;
    }
    if (!lamp_state_is_valid(requested)) {
        /* Reject the boundary input (never wrap it) and fail off.  If the
         * fail-off escalation cannot prove the output off, that failure is
         * reported so the caller never mistakes an active lamp for a
         * safely-failed one. */
        if (lamp_fail_off() != LAMP_OK) {
            return LAMP_ERR_FAIL_OFF;
        }
        return LAMP_ERR_OUT_OF_RANGE;
    }

    /* Both power-off and zero brightness select the electrical off duty;
     * the requested power state is still recorded below. */
    if (!requested->power || requested->brightness_percent == LAMP_BRIGHTNESS_MIN) {
        duty = LAMP_DUTY_MIN;
    } else {
        (void)lamp_duty_from_brightness(requested->brightness_percent, &duty);
    }

    duty_percent = (float)duty / (float)LAMP_DUTY_SCALE;

    hs = hal_pwm_set_duty(s_lamp.pin, duty_percent);
    if (hs != HAL_OK) {
        /* Failure path: force the output off before propagating the error.
         * If the fail-off escalation itself fails, report that instead —
         * the state must not claim the output is off when it may be on. */
        if (lamp_fail_off() != LAMP_OK) {
            return LAMP_ERR_FAIL_OFF;
        }
        return lamp_map_hal_status(hs);
    }

    s_lamp.applied.power = requested->power;
    s_lamp.applied.brightness_percent = requested->brightness_percent;
    s_lamp.applied.output_active =
        requested->power && (requested->brightness_percent > LAMP_BRIGHTNESS_MIN);

    if (applied_out != NULL) {
        *applied_out = s_lamp.applied;
    }

    return LAMP_OK;
}

lamp_status_t lamp_control_apply_state(const lamp_state_t *requested,
                                       lamp_applied_state_t *applied_out)
{
    lamp_status_t status;

    lamp_lock();
    if (!s_lamp.initialized) {
        /* Lifecycle check precedes the barrier check: deinit does not clear
         * the latched barrier, so a deinitialized lamp must still report the
         * documented not-initialized status, not the barrier status. */
        status = LAMP_ERR_NOT_INITIALIZED;
    } else if (s_lamp.fail_off_barrier) {
        /* Fail-off barrier (review finding 2): an explicit force-inactive
         * (the disconnect path) has latched the barrier, so this apply —
         * whether it was already waiting for the lock or arrives later —
         * must not re-energize the output.  Re-force the output inactive
         * and reject until the caller performs the explicit re-enable
         * transition (lamp_control_release_fail_off()); this preserves the
         * "fail-off wins over any concurrent/later apply" priority. */
        if (lamp_fail_off() != LAMP_OK) {
            /* The re-force could not prove the output off: propagate the
             * safety failure instead of masking it with the barrier status. */
            status = LAMP_ERR_FAIL_OFF;
        } else {
            status = LAMP_ERR_BLOCKED_BY_FAIL_OFF;
        }
    } else {
        status = lamp_control_apply_state_locked(requested, applied_out);
    }
    lamp_unlock();

    return status;
}

static lamp_status_t lamp_control_force_inactive_locked(void)
{
    if (!s_lamp.initialized) {
        return LAMP_ERR_NOT_INITIALIZED;
    }

    /* Explicit safety operation: escalate through a retry and the 0 % duty
     * fallback, and refuse to report success unless the output is proven
     * off (see lamp_fail_off()).  The requested power/brightness are
     * retained for protocol consistency; only the electrical flag drops,
     * and only when the HAL confirms the inactive output. */
    return lamp_fail_off();
}

lamp_status_t lamp_control_force_inactive(void)
{
    lamp_status_t status;

    /* Serialized against apply-state/deinit so a fail-off driven from a
     * foreign thread (network adapter, Wi-Fi worker) can never interleave
     * with a concurrent state application on the lamp. */
    lamp_lock();
    status = lamp_control_force_inactive_locked();
    if (status == LAMP_OK) {
        /* Latch the fail-off barrier (review finding 2): the disconnect
         * endpoint is "output off AND no later apply may re-enable it".
         * lamp_control_apply_state() rejects until the explicit re-enable
         * transition (lamp_control_release_fail_off()) clears the barrier. */
        s_lamp.fail_off_barrier = true;
    }
    lamp_unlock();

    return status;
}

lamp_status_t lamp_control_release_fail_off(void)
{
    /* Explicit re-enable transition: clears the fail-off barrier latched by
     * lamp_control_force_inactive().  Idempotent and always safe — clearing
     * a barrier that is not latched is a no-op; it never touches the
     * hardware or the applied state. */
    lamp_lock();
    s_lamp.fail_off_barrier = false;
    lamp_unlock();

    return LAMP_OK;
}

static lamp_status_t lamp_control_deinit_locked(void)
{
    hal_status_t hs;

    if (!s_lamp.initialized) {
        return LAMP_ERR_NOT_INITIALIZED;
    }

    hs = hal_pwm_deinit(s_lamp.pin);
    if (hs != HAL_OK) {
        /* Failure path: keep the output off even though the release failed,
         * and stay initialized so the release can be retried.  If the
         * fail-off escalation itself fails, report that instead of claiming
         * the output is off. */
        if (lamp_fail_off() != LAMP_OK) {
            return LAMP_ERR_FAIL_OFF;
        }
        return lamp_map_hal_status(hs);
    }

    s_lamp.initialized = false;
    s_lamp.pin = HAL_PIN_NONE;
    s_lamp.applied.power = false;
    s_lamp.applied.brightness_percent = LAMP_BRIGHTNESS_MIN;
    s_lamp.applied.output_active = false;

    return LAMP_OK;
}

lamp_status_t lamp_control_deinit(void)
{
    lamp_status_t status;

    lamp_lock();
    status = lamp_control_deinit_locked();
    lamp_unlock();

    return status;
}

static lamp_status_t lamp_control_get_applied_state_locked(
    lamp_applied_state_t *applied_out)
{
    if (!s_lamp.initialized) {
        return LAMP_ERR_NOT_INITIALIZED;
    }
    if (applied_out == NULL) {
        return LAMP_ERR_INVALID_ARGUMENT;
    }

    *applied_out = s_lamp.applied;

    return LAMP_OK;
}

lamp_status_t lamp_control_get_applied_state(lamp_applied_state_t *applied_out)
{
    lamp_status_t status;

    lamp_lock();
    status = lamp_control_get_applied_state_locked(applied_out);
    lamp_unlock();

    return status;
}