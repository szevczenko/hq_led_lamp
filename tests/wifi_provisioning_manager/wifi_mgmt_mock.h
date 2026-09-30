/**
 * @file wifi_mgmt_mock.h
 * @brief Test-only Wi-Fi manager double for provisioning-adapter host tests
 *        (TASK-126)
 *
 * The provisioning adapter touches exactly one Wi-Fi manager API:
 * wifi_mgmt_is_read_data(), the saved-credentials query that feeds the
 * adapter's has_saved_credentials() product decision.  This minimal double
 * implements that contract with a configurable value plus a call counter.
 *
 * Secrecy: the double contains no credential content at all.  This file is
 * a test double only: it is compiled solely into the provisioning-manager
 * host test binary and is never part of any production build.
 */

#ifndef WIFI_MGMT_MOCK_H
#define WIFI_MGMT_MOCK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reset the double to "no saved credentials" with zero counters. */
void wifi_mgmt_mock_reset(void);

/** @brief Set the value reported by wifi_mgmt_is_read_data(). */
void wifi_mgmt_mock_set_saved_credentials(bool saved);

/** @brief Number of wifi_mgmt_is_read_data() invocations recorded. */
unsigned wifi_mgmt_mock_is_read_data_calls(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MGMT_MOCK_H */