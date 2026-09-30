/**
 * @file wifi_hal_mock_min.h
 * @brief Minimal test-only Wi-Fi HAL double for the wifi_ap.json overwrite
 *        host test (TASK-136)
 *
 * The overwrite host test compiles the REAL platform Wi-Fi manager
 * (wifi_managment.c) on the host.  The manager drives the radio through
 * the platform-agnostic HAL contract (wifi_hal_driver.h); this double
 * implements that contract deterministically so the manager's
 * connect/persist state machine runs without a radio:
 *
 *   - it records the station configuration handed to it, so the test can
 *     prove the successful connect carried exactly the newly submitted
 *     credential,
 *   - wifi_hal_connect() returns a test-controlled result (the stale
 *     credential is made to fail, the new one to be accepted),
 *   - wifi_hal_mock_min_inject_event() replays HAL events (GOT_IP) through
 *     the registered callback, which is how the manager observes a
 *     successful station connect,
 *   - the double is mutex-protected because the manager's worker task and
 *     the test thread call into it concurrently.
 *
 * The double never logs and never stores credential content beyond the
 * last station config snapshot used by the assertions.
 *
 * This file is a test double only: it is compiled solely into the
 * wifi_storage_overwrite host test binary and is never part of any
 * production build.
 */

#ifndef WIFI_HAL_MOCK_MIN_H
#define WIFI_HAL_MOCK_MIN_H

#include <stdbool.h>

#include "osal_error.h"
#include "wifi_hal_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset the double to a pristine state (not initialized, not
 *        started, connect accepted by default, counters zeroed).
 */
void wifi_hal_mock_min_reset(void);

/**
 * @brief Choose the result returned by wifi_hal_connect().
 *
 * The stale-credential phase uses OSAL_ERROR (the wrong AP/password is
 * rejected); the submitted-credential phase uses OSAL_SUCCESS.
 */
void wifi_hal_mock_min_set_connect_result(osal_status_t result);

/**
 * @brief Replay a HAL event through the registered event callback.
 *
 * Simulates the radio reporting e.g. WIFI_HAL_EVT_STA_GOT_IP; any IP info
 * carried by @p data is also recorded for wifi_hal_get_sta_ip_info().
 */
void wifi_hal_mock_min_inject_event(wifi_hal_event_t event,
                                    const wifi_hal_event_data_t *data);

/** @brief Number of wifi_hal_connect() invocations recorded. */
unsigned wifi_hal_mock_min_connect_calls(void);

/**
 * @brief Snapshot of the last station config handed to
 *        wifi_hal_set_sta_config() (owned copy).
 *
 * @param[out] out Receives the last config.
 * @return true when at least one config was applied.
 */
bool wifi_hal_mock_min_get_sta_config(wifi_hal_sta_config_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_HAL_MOCK_MIN_H */