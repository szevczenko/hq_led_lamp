# lamp_control (TASK-106)

Hardware-independent LED lamp state policy for the Kitchen LED Controller.

## What it owns

- Validated requested state: `lamp_state_t { power, brightness_percent }`.
- Applied state: `lamp_applied_state_t { power, brightness_percent,
  output_active }` (what reached the PWM output and whether it is
  electrically on).
- Normalized duty conversion (`lamp_duty_t`, 1/100 % fixed point,
  `0..10000`) with overflow-safe integer arithmetic and round-half-up
  reporting.
- The fail-off contract: the output is off before initialization, right
  after initialization, and after every rejected input or HAL failure.
- Fail-off escalation: every failure path tries `hal_pwm_force_inactive()`
  (with a retry) and then a 0 % duty fallback, and only records the output
  as inactive once the HAL confirms it.  If the output cannot be proven
  off, the failing operation returns `LAMP_ERR_FAIL_OFF`, keeps
  `output_active` at its last known value and stays initialized so the
  fail-off / release can be retried.

## How it stays hardware-independent

`lamp_control.c` calls only the portable HAL PWM API (`hal_pwm.h` /
`hal_types.h` from `platform/hq_platform/src/hal/include`). There is no
ESP-IDF, LEDC or GPIO code in this component; the ESP-IDF LEDC backend is
provided by the `hq_hal` component at the platform layer.

## Using it from the application layer

1. Build the configuration from the compile-time Kconfig options:

   ```c
   lamp_control_config_t cfg;
   cfg.pin          = CONFIG_KLC_LED_PWM_GPIO;
   cfg.frequency_hz = CONFIG_KLC_LED_PWM_FREQUENCY_HZ;
   cfg.polarity     = CONFIG_KLC_LED_PWM_ACTIVE_LOW
                          ? HAL_POLARITY_ACTIVE_LOW
                          : HAL_POLARITY_ACTIVE_HIGH;
   ```

2. `lamp_control_init(&cfg)` — starts the PWM at the configured inactive
   level; the LED stays off until the first valid
   `lamp_control_apply_state()`.
3. On every authoritative `power`/`brightness` update (ThingsBoard sync,
   RPC, ...) call `lamp_control_apply_state()`.
4. On boot, Wi-Fi/MQTT loss, TLS failure, sync timeout or fatal error call
   `lamp_control_force_inactive()` (the fail-off path).

## Tests

Host unit tests live in `tests/lamp_control/` — see
`tests/lamp_control/README.md` for how to build and run them.