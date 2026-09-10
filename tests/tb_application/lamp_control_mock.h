/**
 * @file lamp_control_mock.h
 * @brief Test-only lamp-control double for tb_application host tests
 *        (TASK-112 / TASK-113)
 *
 * The application module's lamp surface is:
 *
 *   - lamp_control_force_inactive()  — fail-off on every rejection path and
 *     on disconnect,
 *   - lamp_control_apply_state()     — the only way a complete valid
 *     desired state reaches the output,
 *   - lamp_control_release_fail_off()— the module clears its own latched
 *     barrier immediately before applying,
 *   - lamp_control_get_applied_state()— read back the applied hardware
 *     state (used by the server-RPC getState handler and as the unchanged
 *     base for single-field set methods).
 *
 * The double records every call in that order and exposes the last applied
 * state so tests can assert "no output is enabled before valid complete
 * synchronization" (apply count stays 0 until a complete valid state
 * arrives), that a rejected attempt only ever forces the output off, and
 * that an invalid RPC never modifies the applied state.
 */

#ifndef LAMP_CONTROL_MOCK_H
#define LAMP_CONTROL_MOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "lamp_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reset the double (counters zero, success results, no applied state). */
void lamp_mock_reset(void);

/** @brief Number of lamp_control_apply_state() calls recorded. */
unsigned lamp_mock_apply_calls(void);

/** @brief Number of lamp_control_force_inactive() calls recorded. */
unsigned lamp_mock_force_inactive_calls(void);

/** @brief Number of lamp_control_release_fail_off() calls recorded. */
unsigned lamp_mock_release_calls(void);

/** @brief Last state passed to lamp_control_apply_state() (true once applied). */
bool lamp_mock_state_applied(void);

/** @brief Last applied power value. */
bool lamp_mock_applied_power(void);

/** @brief Last applied brightness value. */
uint8_t lamp_mock_applied_brightness(void);

/** @brief Make lamp_control_apply_state() return @p status. */
void lamp_mock_set_apply_result(lamp_status_t status);

/** @brief Make lamp_control_force_inactive() return @p status. */
void lamp_mock_set_force_inactive_result(lamp_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* LAMP_CONTROL_MOCK_H */