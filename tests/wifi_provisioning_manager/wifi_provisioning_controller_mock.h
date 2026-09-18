/**
 * @file wifi_provisioning_controller_mock.h
 * @brief Test-only platform fallback-controller double (TASK-132)
 *
 * Implements the platform automatic fallback controller contract
 * (wifi_provisioning_controller.h) as an in-memory test double so the
 * adapter's notification translation (TASK-132) can be exercised on the
 * host without a controller worker task:
 *
 *   - init_with_config records the registered notification hook and user
 *     context (and whether a per-device fallback-budget override was set,
 *     proving the adapter leaves the TASK-131 platform default untouched)
 *     instead of running a real policy worker,
 *   - deinit records the call and clears the registered hook,
 *   - a test-only fire(previous, current, session) hook delivers a
 *     state-change notification through the exact callback the adapter
 *     registered, so tests drive the translation table transition by
 *     transition with a caller-chosen lifecycle session/generation token.
 *
 * Secrecy: the double never stores or logs credential-like content.  This
 * file is a test double only: it is compiled solely into the
 * provisioning-manager host test binary and is never part of any production
 * build.
 */

#ifndef WIFI_PROVISIONING_CONTROLLER_MOCK_H
#define WIFI_PROVISIONING_CONTROLLER_MOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "wifi_provisioning_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Observations                                                           */
/* --------------------------------------------------------------------- */

typedef struct wifi_provisioning_controller_mock_counters
{
  unsigned init_with_config_calls;   /**< init_with_config() invocations. */
  unsigned deinit_calls;             /**< deinit() invocations. */
  bool     last_init_result;         /**< Result the double reports next. */
  wifi_provisioning_controller_state_cb_t registered_cb;
                                     /**< Callback the adapter registered. */
  void    *registered_user_ctx;      /**< User context the adapter passed. */
  bool     config_fallback_budget_set;
                                     /**< Was a per-device fallback-budget
                                          override requested? (Must stay
                                          false for the TASK-131 default.) */
  uint32_t config_fallback_budget;   /**< Requested override value. */
} wifi_provisioning_controller_mock_counters_t;

/* --------------------------------------------------------------------- */
/* Test control API                                                       */
/* --------------------------------------------------------------------- */

/** @brief Reset the double to a fresh, never-registered state. */
void wifi_provisioning_controller_mock_reset(void);

/** @brief Choose the result reported by the next init_with_config() call
 *         (default true). */
void wifi_provisioning_controller_mock_set_init_result(bool ok);

/** @brief Snapshot of the accumulated counters / registered hook. */
wifi_provisioning_controller_mock_counters_t
wifi_provisioning_controller_mock_get_counters(void);

/**
 * @brief Deliver a state-change notification through the registered hook.
 *
 * Invokes the callback registered by the adapter synchronously, exactly as
 * the controller worker (or the synchronous API-caller path) would, with
 * the given lifecycle session/generation token.
 */
void wifi_provisioning_controller_mock_fire(
    wifi_provisioning_controller_state_t previous,
    wifi_provisioning_controller_state_t current,
    uint32_t session);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PROVISIONING_CONTROLLER_MOCK_H */