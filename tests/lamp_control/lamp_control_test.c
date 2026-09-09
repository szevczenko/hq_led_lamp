/**
 * @file lamp_control_test.c
 * @brief Host unit tests for the lamp-control domain component (TASK-106)
 *
 * Exercises the hardware-independent lamp policy from
 * components/lamp_control against a deterministic HAL PWM test double
 * (hal_pwm_mock.c) that implements the portable hal_pwm.h contract plus
 * failure injection and output inspection.
 *
 * Coverage (per the TASK-106 definition of done):
 *   - brightness boundaries 0 and 100,
 *   - invalid boundary input rejection (never wrapped),
 *   - duty rounding (round-half-up inverse conversion),
 *   - both active polarities (raw electrical level truth table),
 *   - lifecycle failures (use-before-init, double-init, double-deinit,
 *     re-init, bad configuration),
 *   - fail-off: the electrical output is off before initialization, right
 *     after initialization and after every rejected input or HAL failure;
 *     a failed fail-off escalation is reported (#LAMP_ERR_FAIL_OFF) and is
 *     never recorded as a successful shutdown.
 *
 * The tests compile only the public lamp_control API; production code is
 * never modified for testability.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lamp_control.h"
#include "hal_pwm_mock.h"
#include "unity.h"

/* --------------------------------------------------------------------- */
/* Shared helpers                                                         */
/* --------------------------------------------------------------------- */

/** @brief Test pin used for every init scenario. */
#define TEST_PIN ((hal_pin_t)18u)

/** @brief Test frequency used for every init scenario. */
#define TEST_FREQUENCY_HZ 20000u

/** @brief Build a lamp configuration with the given polarity. */
static lamp_control_config_t lamp_config(hal_polarity_t polarity)
{
    lamp_control_config_t cfg;

    cfg.pin = TEST_PIN;
    cfg.frequency_hz = TEST_FREQUENCY_HZ;
    cfg.polarity = polarity;

    return cfg;
}

/** @brief Raw electrical level of the logical INACTIVE state. */
static int inactive_raw(hal_polarity_t polarity)
{
    return (polarity == HAL_POLARITY_ACTIVE_LOW) ? 1 : 0;
}

/** @brief Raw electrical level of the logical ACTIVE state. */
static int active_raw(hal_polarity_t polarity)
{
    return (polarity == HAL_POLARITY_ACTIVE_LOW) ? 0 : 1;
}

/**
 * @brief Assert that the mock reports the lamp output electrically off:
 * e.g. AFTER init/deinit/fail-off, without claiming the pin is released.
 */
static void assert_output_inactive(hal_polarity_t polarity)
{
    int raw = -1;
    bool generating = true;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_mock_get_output(&raw, &generating));
    TEST_ASSERT_EQUAL_INT(inactive_raw(polarity), raw);
}

/** @brief Assert that the mock reports the lamp output electrically on. */
static void assert_output_active(hal_polarity_t polarity)
{
    int raw = -1;
    bool generating = false;

    TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_mock_get_output(&raw, &generating));
    TEST_ASSERT_EQUAL_INT(active_raw(polarity), raw);
}

/** @brief Assert that no PWM output exists at the HAL level at all. */
static void assert_no_output_exists(void)
{
    int raw = -1;
    bool generating = true;

    TEST_ASSERT_EQUAL_INT(HAL_ERR_NOT_INITIALIZED,
                          (int)hal_pwm_mock_get_output(&raw, &generating));
}

/* --------------------------------------------------------------------- */
/* Duty conversion: 0, 100, midpoints, invalid inputs                    */
/* --------------------------------------------------------------------- */

static void test_duty_brightness_zero(void)
{
    lamp_duty_t duty = LAMP_DUTY_MAX;
    uint8_t brightness = 255u;

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_duty_from_brightness(0u, &duty));
    TEST_ASSERT_EQUAL_UINT32(LAMP_DUTY_MIN, duty);
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_brightness_from_duty(LAMP_DUTY_MIN, &brightness));
    TEST_ASSERT_EQUAL_UINT8(0u, brightness);
}

static void test_duty_brightness_max(void)
{
    lamp_duty_t duty = LAMP_DUTY_MIN;
    uint8_t brightness = 0u;

    TEST_ASSERT_EQUAL_INT(LAMP_OK,
                          (int)lamp_duty_from_brightness(LAMP_BRIGHTNESS_MAX, &duty));
    TEST_ASSERT_EQUAL_UINT32(LAMP_DUTY_MAX, duty);
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_brightness_from_duty(LAMP_DUTY_MAX, &brightness));
    TEST_ASSERT_EQUAL_UINT8(LAMP_BRIGHTNESS_MAX, brightness);
}

static void test_duty_brightness_midpoints(void)
{
    lamp_duty_t duty = 0u;
    uint8_t brightness = 0u;

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_duty_from_brightness(1u, &duty));
    TEST_ASSERT_EQUAL_UINT32(100u, duty);
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_brightness_from_duty(100u, &brightness));
    TEST_ASSERT_EQUAL_UINT8(1u, brightness);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_duty_from_brightness(33u, &duty));
    TEST_ASSERT_EQUAL_UINT32(3300u, duty);
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_brightness_from_duty(3300u, &brightness));
    TEST_ASSERT_EQUAL_UINT8(33u, brightness);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_duty_from_brightness(50u, &duty));
    TEST_ASSERT_EQUAL_UINT32(5000u, duty);
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_brightness_from_duty(5000u, &brightness));
    TEST_ASSERT_EQUAL_UINT8(50u, brightness);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_duty_from_brightness(99u, &duty));
    TEST_ASSERT_EQUAL_UINT32(9900u, duty);
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_brightness_from_duty(9900u, &brightness));
    TEST_ASSERT_EQUAL_UINT8(99u, brightness);
}

static void test_duty_rejects_invalid_brightness(void)
{
    lamp_duty_t duty = 0u;

    /* Boundary violations are rejected, never wrapped into the valid range. */
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_OUT_OF_RANGE,
                          (int)lamp_duty_from_brightness(101u, &duty));
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_OUT_OF_RANGE,
                          (int)lamp_duty_from_brightness(255u, &duty));
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_INVALID_ARGUMENT,
                          (int)lamp_duty_from_brightness(50u, NULL));
}

/* --------------------------------------------------------------------- */
/* Duty rounding (round-half-up inverse conversion)                      */
/* --------------------------------------------------------------------- */

static void test_duty_rounding(void)
{
    uint8_t brightness = 0u;

    /* 0.01 % and 0.49 % round down to 0 %. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_brightness_from_duty(1u, &brightness));
    TEST_ASSERT_EQUAL_UINT8(0u, brightness);
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_brightness_from_duty(49u, &brightness));
    TEST_ASSERT_EQUAL_UINT8(0u, brightness);

    /* 0.50 % rounds half-up to 1 %. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_brightness_from_duty(50u, &brightness));
    TEST_ASSERT_EQUAL_UINT8(1u, brightness);

    /* 33.49 % -> 33 %, 33.50 % -> 34 %. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_brightness_from_duty(3349u, &brightness));
    TEST_ASSERT_EQUAL_UINT8(33u, brightness);
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_brightness_from_duty(3350u, &brightness));
    TEST_ASSERT_EQUAL_UINT8(34u, brightness);

    /* 99.99 % rounds up to 100 %, which is the valid maximum. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_brightness_from_duty(9999u, &brightness));
    TEST_ASSERT_EQUAL_UINT8(100u, brightness);

    /* Above the maximum duty is rejected, never wrapped. */
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_OUT_OF_RANGE,
                          (int)lamp_brightness_from_duty(10001u, &brightness));
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_OUT_OF_RANGE,
                          (int)lamp_brightness_from_duty(0xFFFFFFFFu, &brightness));
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_INVALID_ARGUMENT,
                          (int)lamp_brightness_from_duty(5000u, NULL));
}

/* --------------------------------------------------------------------- */
/* Lifecycle failures and invalid configuration                           */
/* --------------------------------------------------------------------- */

static void test_operations_before_init_fail(void)
{
    lamp_state_t st = {true, 50u};
    lamp_applied_state_t applied;

    TEST_ASSERT_EQUAL_INT(LAMP_ERR_NOT_INITIALIZED,
                          (int)lamp_control_apply_state(&st, &applied));
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_NOT_INITIALIZED,
                          (int)lamp_control_force_inactive());
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_NOT_INITIALIZED,
                          (int)lamp_control_deinit());
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_NOT_INITIALIZED,
                          (int)lamp_control_get_applied_state(&applied));

    /* No HAL traffic and no output exist yet. */
    TEST_ASSERT_EQUAL_UINT32(0u, hal_pwm_mock_init_count());
    TEST_ASSERT_EQUAL_UINT32(0u, hal_pwm_mock_set_duty_count());
    assert_no_output_exists();
}

static void test_init_twice_fails(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_ALREADY_INITIALIZED,
                          (int)lamp_control_init(&cfg));
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

static void test_init_rejects_bad_configuration(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);
    lamp_control_config_t bad;

    TEST_ASSERT_EQUAL_INT(LAMP_ERR_INVALID_ARGUMENT,
                          (int)lamp_control_init(NULL));

    bad = cfg;
    bad.pin = HAL_PIN_NONE;
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_INVALID_ARGUMENT,
                          (int)lamp_control_init(&bad));

    bad = cfg;
    bad.frequency_hz = 0u;
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_INVALID_ARGUMENT,
                          (int)lamp_control_init(&bad));

    bad = cfg;
    bad.polarity = (hal_polarity_t)99;
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_INVALID_ARGUMENT,
                          (int)lamp_control_init(&bad));

    /* Nothing was initialized by any of the rejected attempts. */
    assert_no_output_exists();
}

static void test_deinit_twice_and_reinit(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_NOT_INITIALIZED,
                          (int)lamp_control_deinit());
    assert_no_output_exists();

    /* A clean deinit frees the output for a fresh init cycle. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

/* --------------------------------------------------------------------- */
/* Output-off guarantees (shared by both polarities)                     */
/* --------------------------------------------------------------------- */

static void run_init_output_off(hal_polarity_t polarity)
{
    lamp_control_config_t cfg = lamp_config(polarity);
    lamp_applied_state_t applied;

    assert_no_output_exists();

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));

    /* Right after init the configured frequency reached the HAL and the
     * output is held at the logical INACTIVE level. */
    {
        uint32_t freq = 0u;
        float duty = -1.0f;

        TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_mock_get_frequency(&freq));
        TEST_ASSERT_EQUAL_UINT32(TEST_FREQUENCY_HZ, freq);
        TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_mock_get_duty(&duty));
        TEST_ASSERT_EQUAL_FLOAT(0.0f, duty);
    }
    assert_output_inactive(polarity);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_get_applied_state(&applied));
    TEST_ASSERT_FALSE(applied.power);
    TEST_ASSERT_EQUAL_UINT8(0u, applied.brightness_percent);
    TEST_ASSERT_FALSE(applied.output_active);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

static void test_init_output_off_active_high(void)
{
    run_init_output_off(HAL_POLARITY_ACTIVE_HIGH);
}

static void test_init_output_off_active_low(void)
{
    run_init_output_off(HAL_POLARITY_ACTIVE_LOW);
}

/* --------------------------------------------------------------------- */
/* Apply-state behavior (shared by both polarities)                      */
/* --------------------------------------------------------------------- */

static void run_apply_state(hal_polarity_t polarity)
{
    lamp_control_config_t cfg = lamp_config(polarity);
    lamp_state_t st;
    lamp_applied_state_t applied;

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));

    /* power=false always selects the electrical off duty; the requested
     * power state is recorded for protocol consistency. */
    st.power = false;
    st.brightness_percent = 100u;
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st, &applied));
    TEST_ASSERT_FALSE(applied.power);
    TEST_ASSERT_EQUAL_UINT8(100u, applied.brightness_percent);
    TEST_ASSERT_FALSE(applied.output_active);
    {
        float duty = -1.0f;
        TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_mock_get_duty(&duty));
        TEST_ASSERT_EQUAL_FLOAT(0.0f, duty);
    }
    assert_output_inactive(polarity);

    /* power=true with brightness=0 also produces the off duty but keeps
     * the requested power state. */
    st.power = true;
    st.brightness_percent = 0u;
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st, &applied));
    TEST_ASSERT_TRUE(applied.power);
    TEST_ASSERT_EQUAL_UINT8(0u, applied.brightness_percent);
    TEST_ASSERT_FALSE(applied.output_active);
    assert_output_inactive(polarity);

    /* Minimum non-zero brightness drives a small active fraction. */
    st.power = true;
    st.brightness_percent = 1u;
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st, &applied));
    TEST_ASSERT_TRUE(applied.output_active);
    assert_output_active(polarity);
    {
        float duty = -1.0f;
        TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_mock_get_duty(&duty));
        TEST_ASSERT_EQUAL_FLOAT(1.0f, duty);
    }

    /* Midpoint brightness drives exactly 50.00 % normalized duty. */
    st.power = true;
    st.brightness_percent = 50u;
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st, &applied));
    TEST_ASSERT_TRUE(applied.power);
    TEST_ASSERT_EQUAL_UINT8(50u, applied.brightness_percent);
    TEST_ASSERT_TRUE(applied.output_active);
    assert_output_active(polarity);
    {
        float duty = -1.0f;
        TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_mock_get_duty(&duty));
        TEST_ASSERT_EQUAL_FLOAT(50.0f, duty);
    }

    /* Maximum brightness drives 100.00 % normalized duty. */
    st.power = true;
    st.brightness_percent = 100u;
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st, &applied));
    TEST_ASSERT_TRUE(applied.output_active);
    assert_output_active(polarity);
    {
        float duty = -1.0f;
        TEST_ASSERT_EQUAL_INT(HAL_OK, (int)hal_pwm_mock_get_duty(&duty));
        TEST_ASSERT_EQUAL_FLOAT(100.0f, duty);
    }

    /* Round trip: an off request after an on request turns the output off. */
    st.power = false;
    st.brightness_percent = 0u;
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st, &applied));
    TEST_ASSERT_FALSE(applied.output_active);
    assert_output_inactive(polarity);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

static void test_apply_state_active_high(void)
{
    run_apply_state(HAL_POLARITY_ACTIVE_HIGH);
}

static void test_apply_state_active_low(void)
{
    run_apply_state(HAL_POLARITY_ACTIVE_LOW);
}

/* --------------------------------------------------------------------- */
/* Invalid apply-state input and fail-off                                */
/* --------------------------------------------------------------------- */

static void test_apply_state_rejects_invalid_brightness(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);
    lamp_state_t st;
    lamp_applied_state_t applied;

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));

    /* First put the lamp into a known-active state. */
    st.power = true;
    st.brightness_percent = 80u;
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st, &applied));
    assert_output_active(HAL_POLARITY_ACTIVE_HIGH);
    TEST_ASSERT_TRUE(applied.output_active);

    /* A brightness boundary violation is rejected (never wrapped) and the
     * output is forced off. */
    st.brightness_percent = 101u;
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_OUT_OF_RANGE,
                          (int)lamp_control_apply_state(&st, &applied));

    assert_output_inactive(HAL_POLARITY_ACTIVE_HIGH);
    TEST_ASSERT_TRUE(hal_pwm_mock_is_forced_inactive());
    TEST_ASSERT_TRUE(hal_pwm_mock_force_inactive_count() >= 1u);

    /* The applied record keeps the last accepted request, minus the
     * electrical flag dropped by the fail-off. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_get_applied_state(&applied));
    TEST_ASSERT_TRUE(applied.power);
    TEST_ASSERT_EQUAL_UINT8(80u, applied.brightness_percent);
    TEST_ASSERT_FALSE(applied.output_active);

    /* The worst-case uint8 also fails instead of wrapping to a tiny value. */
    st.brightness_percent = 255u;
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_OUT_OF_RANGE,
                          (int)lamp_control_apply_state(&st, &applied));
    assert_output_inactive(HAL_POLARITY_ACTIVE_HIGH);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

static void test_apply_state_rejects_null(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);
    lamp_state_t st_on = {true, 75u};
    lamp_applied_state_t applied;

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));

    /* NULL is rejected after the lamp is already active; the rejection must
     * still invoke fail-off rather than leaving the previous output on. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK,
                          (int)lamp_control_apply_state(&st_on, &applied));
    TEST_ASSERT_TRUE(applied.output_active);
    assert_output_active(HAL_POLARITY_ACTIVE_HIGH);

    TEST_ASSERT_EQUAL_INT(LAMP_ERR_INVALID_ARGUMENT,
                          (int)lamp_control_apply_state(NULL, NULL));
    TEST_ASSERT_TRUE(hal_pwm_mock_is_forced_inactive());
    assert_output_inactive(HAL_POLARITY_ACTIVE_HIGH);

    /* The accepted state is retained, while the electrical output is known
     * to be inactive after the rejected request. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK,
                          (int)lamp_control_get_applied_state(&applied));
    TEST_ASSERT_TRUE(applied.power);
    TEST_ASSERT_EQUAL_UINT8(75u, applied.brightness_percent);
    TEST_ASSERT_FALSE(applied.output_active);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

/* --------------------------------------------------------------------- */
/* HAL failure paths keep the output off (fail-off)                      */
/* --------------------------------------------------------------------- */

static void run_apply_hal_failure_keeps_off(hal_status_t injected)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);
    lamp_state_t st = {true, 60u};
    lamp_applied_state_t applied;
    lamp_status_t expected;

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));

    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_SET_DUTY, injected);
    expected = (injected == HAL_ERR_OUT_OF_RANGE) ? LAMP_ERR_OUT_OF_RANGE :
               (injected == HAL_ERR_NOT_INITIALIZED) ? LAMP_ERR_NOT_INITIALIZED :
               LAMP_ERR_INTERNAL;
    TEST_ASSERT_EQUAL_INT(expected, (int)lamp_control_apply_state(&st, &applied));

    /* The failed set_duty triggered the fail-off path. */
    TEST_ASSERT_TRUE(hal_pwm_mock_is_forced_inactive());
    TEST_ASSERT_TRUE(hal_pwm_mock_force_inactive_count() >= 1u);
    assert_output_inactive(HAL_POLARITY_ACTIVE_HIGH);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

static void test_apply_hal_error_fails_off(void)
{
    run_apply_hal_failure_keeps_off(HAL_ERROR);
}

static void test_apply_hal_out_of_range_fails_off(void)
{
    run_apply_hal_failure_keeps_off(HAL_ERR_OUT_OF_RANGE);
}

static void test_apply_hal_internal_fails_off(void)
{
    run_apply_hal_failure_keeps_off(HAL_ERR_INTERNAL);
}

static void test_apply_fail_off_failure_reports(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);
    lamp_state_t st_on = {true, 60u};
    lamp_state_t st_bad;
    lamp_applied_state_t applied;

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st_on, NULL));
    assert_output_active(HAL_POLARITY_ACTIVE_HIGH);

    /* The invalid input is rejected, but the fail-off escalation also
     * exhausts every attempt: the operation must report the failed safety
     * action instead of an innocent out-of-range error, and the applied
     * record must not claim the output is off. */
    st_bad.power = true;
    st_bad.brightness_percent = 150u;
    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_FORCE_INACTIVE, HAL_ERR_INTERNAL);
    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_FORCE_INACTIVE, HAL_ERR_INTERNAL);
    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_SET_DUTY, HAL_ERR_INTERNAL);
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_FAIL_OFF,
                          (int)lamp_control_apply_state(&st_bad, &applied));

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_get_applied_state(&applied));
    TEST_ASSERT_TRUE(applied.output_active);
    assert_output_active(HAL_POLARITY_ACTIVE_HIGH);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

static void test_init_hal_failure_leaves_off(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);

    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_INIT, HAL_ERR_NO_RESOURCE);
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_NO_RESOURCE, (int)lamp_control_init(&cfg));
    assert_no_output_exists();

    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_INIT, HAL_ERR_NOT_SUPPORTED);
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_NOT_SUPPORTED, (int)lamp_control_init(&cfg));
    assert_no_output_exists();

    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_INIT, HAL_ERR_INTERNAL);
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_INTERNAL, (int)lamp_control_init(&cfg));
    assert_no_output_exists();
}

static void test_init_hold_failure_rolls_back(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);
    uint32_t init_calls;
    uint32_t deinit_calls;

    /* The post-init INACTIVE hold fails: init must roll the HAL back and
     * leave the lamp fully off and re-initializable. */
    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_FORCE_INACTIVE, HAL_ERR_INTERNAL);
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_INTERNAL, (int)lamp_control_init(&cfg));

    init_calls = hal_pwm_mock_init_count();
    deinit_calls = hal_pwm_mock_deinit_count();
    TEST_ASSERT_TRUE(init_calls >= 1u);
    TEST_ASSERT_TRUE(deinit_calls >= 1u);
    assert_no_output_exists();

    /* A subsequent clean init succeeds. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));
    assert_output_inactive(HAL_POLARITY_ACTIVE_HIGH);
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

static void test_init_hold_failure_rollback_deinit_failure_keeps_cleanup(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);

    /* The post-init INACTIVE hold fails and the rollback deinit also fails:
     * the HAL resource is still held, so lamp-control must preserve its
     * initialized state and pin instead of pretending the release happened.
     * The rollback still performs the strongest available fail-off first. */
    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_FORCE_INACTIVE, HAL_ERR_INTERNAL);
    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_DEINIT, HAL_ERR_INTERNAL);
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_INTERNAL, (int)lamp_control_init(&cfg));

    /* The hold-failure fail-off left the output inactive. */
    assert_output_inactive(HAL_POLARITY_ACTIVE_HIGH);

    /* Still initialized: a later deinit retries and completes the release. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
    assert_no_output_exists();
}

static void test_force_inactive_transient_failure_recovers(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);
    lamp_state_t st = {true, 70u};

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st, NULL));
    assert_output_active(HAL_POLARITY_ACTIVE_HIGH);

    /* A single transient force failure is retried: the safety action
     * succeeds and the output ends off. */
    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_FORCE_INACTIVE, HAL_ERR_INTERNAL);
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_force_inactive());
    assert_output_inactive(HAL_POLARITY_ACTIVE_HIGH);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

static void test_force_inactive_failure_escalates_and_reports(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);
    lamp_state_t st = {true, 70u};
    lamp_applied_state_t applied;

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st, NULL));

    /* The force, its retry and the 0 % duty escalation all fail: the
     * explicit safety action cannot prove the output off, is reported as
     * failed, and the applied state must not claim the lamp is off. */
    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_FORCE_INACTIVE, HAL_ERR_INTERNAL);
    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_FORCE_INACTIVE, HAL_ERR_INTERNAL);
    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_SET_DUTY, HAL_ERR_INTERNAL);
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_FAIL_OFF, (int)lamp_control_force_inactive());

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_get_applied_state(&applied));
    TEST_ASSERT_TRUE(applied.output_active);
    assert_output_active(HAL_POLARITY_ACTIVE_HIGH);

    /* The lamp stays initialized: the safety action and the release remain
     * available instead of the failure being treated as complete. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_force_inactive());
    assert_output_inactive(HAL_POLARITY_ACTIVE_HIGH);
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

static void test_deinit_failure_fails_off(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);
    lamp_state_t st = {true, 60u};

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st, NULL));
    assert_output_active(HAL_POLARITY_ACTIVE_HIGH);

    /* A failing deinit must still leave the output off. */
    hal_pwm_mock_fail_next(HAL_PWM_MOCK_CALL_DEINIT, HAL_ERR_INTERNAL);
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_INTERNAL, (int)lamp_control_deinit());
    assert_output_inactive(HAL_POLARITY_ACTIVE_HIGH);
    TEST_ASSERT_TRUE(hal_pwm_mock_is_forced_inactive());

    /* The lamp is still initialized, so it can be deinitialized cleanly. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
    assert_no_output_exists();
}

/* --------------------------------------------------------------------- */
/* force_inactive and applied-state reporting                             */
/* --------------------------------------------------------------------- */

static void test_force_inactive_retains_requested_state(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);
    lamp_state_t st = {true, 90u};
    lamp_applied_state_t applied;

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st, &applied));
    TEST_ASSERT_TRUE(applied.output_active);
    assert_output_active(HAL_POLARITY_ACTIVE_HIGH);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_force_inactive());
    assert_output_inactive(HAL_POLARITY_ACTIVE_HIGH);

    /* Requested power/brightness survive; only the electrical flag drops. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_get_applied_state(&applied));
    TEST_ASSERT_TRUE(applied.power);
    TEST_ASSERT_EQUAL_UINT8(90u, applied.brightness_percent);
    TEST_ASSERT_FALSE(applied.output_active);

    /* Double force-inactive is allowed and keeps the output off. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_force_inactive());
    assert_output_inactive(HAL_POLARITY_ACTIVE_HIGH);

    /* A new apply resumes PWM generation from the retained state. */
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_apply_state(&st, &applied));
    TEST_ASSERT_TRUE(applied.output_active);
    assert_output_active(HAL_POLARITY_ACTIVE_HIGH);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

static void test_get_applied_state_validation(void)
{
    lamp_control_config_t cfg = lamp_config(HAL_POLARITY_ACTIVE_HIGH);

    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_init(&cfg));
    TEST_ASSERT_EQUAL_INT(LAMP_ERR_INVALID_ARGUMENT,
                          (int)lamp_control_get_applied_state(NULL));
    TEST_ASSERT_EQUAL_INT(LAMP_OK, (int)lamp_control_deinit());
}

/* --------------------------------------------------------------------- */
/* Unity runner                                                           */
/* --------------------------------------------------------------------- */

void setUp(void)
{
    hal_pwm_mock_reset();
}

void tearDown(void)
{
    /* Each test leaves the mock clean; a leaking initialized output shows
     * up as a failing assertion in the next scenario. */
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_duty_brightness_zero);
    RUN_TEST(test_duty_brightness_max);
    RUN_TEST(test_duty_brightness_midpoints);
    RUN_TEST(test_duty_rejects_invalid_brightness);
    RUN_TEST(test_duty_rounding);

    RUN_TEST(test_operations_before_init_fail);
    RUN_TEST(test_init_twice_fails);
    RUN_TEST(test_init_rejects_bad_configuration);
    RUN_TEST(test_deinit_twice_and_reinit);

    RUN_TEST(test_init_output_off_active_high);
    RUN_TEST(test_init_output_off_active_low);

    RUN_TEST(test_apply_state_active_high);
    RUN_TEST(test_apply_state_active_low);
    RUN_TEST(test_apply_state_rejects_invalid_brightness);
    RUN_TEST(test_apply_state_rejects_null);

    RUN_TEST(test_apply_hal_error_fails_off);
    RUN_TEST(test_apply_hal_out_of_range_fails_off);
    RUN_TEST(test_apply_hal_internal_fails_off);
    RUN_TEST(test_init_hal_failure_leaves_off);
    RUN_TEST(test_init_hold_failure_rolls_back);
    RUN_TEST(test_init_hold_failure_rollback_deinit_failure_keeps_cleanup);
    RUN_TEST(test_force_inactive_transient_failure_recovers);
    RUN_TEST(test_force_inactive_failure_escalates_and_reports);
    RUN_TEST(test_deinit_failure_fails_off);
    RUN_TEST(test_apply_fail_off_failure_reports);

    RUN_TEST(test_force_inactive_retains_requested_state);
    RUN_TEST(test_get_applied_state_validation);

    return UNITY_END();
}