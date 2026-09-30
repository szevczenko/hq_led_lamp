# lamp_control host tests (TASK-106)

Host unit tests for the hardware-independent LED lamp state policy in
`components/lamp_control`.

## What is tested

The production component is compiled unmodified against a deterministic
test-only HAL PWM double (`hal_pwm_mock.c`), which implements the portable
`hal_pwm.h` contract plus failure injection and raw output inspection.
Coverage per the TASK-106 definition of done:

- brightness boundaries `0` and `100` (duty conversion forward and back),
- invalid boundary input rejection (`> 100`, `255`, out-of-range duty) —
  inputs are rejected, never wrapped,
- duty rounding (round-half-up inverse conversion: 33.49 % -> 33,
  33.50 % -> 34),
- both active polarities (raw electrical level truth table for
  `HAL_POLARITY_ACTIVE_HIGH` and `HAL_POLARITY_ACTIVE_LOW`),
- lifecycle failures (use-before-init, double-init, double-deinit,
  re-init after deinit, invalid configuration),
- fail-off: the electrical output is off before initialization, right after
  initialization, and after every rejected input or injected HAL failure.

## Running

```sh
cmake -S tests/lamp_control -B build-lamp-control-tests
cmake --build build-lamp-control-tests
ctest --test-dir build-lamp-control-tests --output-on-failure
```

or run the binary directly:

```sh
./build-lamp-control-tests/lamp_control_tests
```

## Layout

- `lamp_control_test.c` — the test cases (Unity runner).
- `hal_pwm_mock.{c,h}` — test-only HAL PWM double with failure injection.
- `unity/` — vendored Unity test framework (MIT, ThrowTheSwitch), see
  `unity/UNITY_LICENSE.txt`.
- `CMakeLists.txt` — standalone host test project (CTest).