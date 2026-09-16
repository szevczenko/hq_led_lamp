/**
 * @file wifi_provisioning_mock.h
 * @brief Test-only platform provisioning application double (TASK-126)
 *
 * Implements the platform provisioning application contract
 * (wifi_http_provisioning.h) as an in-memory test double so the
 * product-owned provisioning adapter (components/wifi_provisioning_manager)
 * can be exercised on the host without the Mongoose portal:
 *
 *   - the double tracks the portal lifecycle (start/stop/state) exactly
 *     like the platform application (STOPPED -> STARTING -> RUNNING ->
 *     STOPPING -> STOPPED; failed start -> ERROR) and is idempotent,
 *   - the runtime listen-URL overrides (set_http_url/set_dns_url) are
 *     recorded per listener so tests can assert what the adapter hands the
 *     platform,
 *   - failure injection lets tests flip start/stop results so every adapter
 *     error path is reachable,
 *   - call counters expose how often each operation was invoked; a
 *     concurrency watermark proves the adapter serializes start/stop (the
 *     double is single-owner by contract, like the platform),
 *   - a "mock connect path" (submit_credentials) records the SSID/password
 *     a provisioning flow would have submitted, so the log-content
 *     regression can prove the adapter never logs credential content that
 *     exists in the system.
 *
 * Secrecy: the double stores credentials ONLY for the log-content
 * regression and never logs them.  This file is a test double only: it is
 * compiled solely into the provisioning-manager host test binary and is
 * never part of any production build.
 */

#ifndef WIFI_PROVISIONING_MOCK_H
#define WIFI_PROVISIONING_MOCK_H

#include <stdbool.h>

#include "wifi_http_provisioning.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Failure injection                                                      */
/* --------------------------------------------------------------------- */

typedef struct wifi_provisioning_mock_config
{
  bool fail_start; /**< start() returns false and moves the state to ERROR. */
  bool fail_stop;  /**< stop() returns false and moves the state to ERROR.  */
} wifi_provisioning_mock_config_t;

/* --------------------------------------------------------------------- */
/* Observations                                                           */
/* --------------------------------------------------------------------- */

typedef struct wifi_provisioning_mock_counters
{
  unsigned start_calls;         /**< start() invocations.                 */
  unsigned stop_calls;          /**< stop() invocations.                  */
  unsigned set_http_url_calls;  /**< set_http_url() invocations.          */
  unsigned set_dns_url_calls;   /**< set_dns_url() invocations.           */
  unsigned max_concurrency;     /**< Max simultaneous start/stop calls.   */
  unsigned set_http_before_start; /**< set_http_url() seen before the
                                       first start().                      */
  unsigned set_dns_before_start;  /**< set_dns_url() seen before the
                                       first start().                      */
  char     last_http_url[128];  /**< Last HTTP URL handed to the double.  */
  char     last_dns_url[128];   /**< Last DNS URL handed to the double.   */
} wifi_provisioning_mock_counters_t;

/* --------------------------------------------------------------------- */
/* Test control API                                                       */
/* --------------------------------------------------------------------- */

/** @brief Reset the double to a fresh, never-started state. */
void wifi_provisioning_mock_reset(void);

/** @brief Apply a failure configuration (before the exercised call). */
void wifi_provisioning_mock_set_config(
    const wifi_provisioning_mock_config_t *config);

/** @brief Snapshot of the accumulated call counters. */
wifi_provisioning_mock_counters_t wifi_provisioning_mock_get_counters(void);

/**
 * @brief Record a credential submission on the "mock connect path".
 *
 * Simulates the portal receiving submitted Wi-Fi credentials (POST
 * /api/v1/wifi/credentials) and connecting with them.  The double stores
 * the values purely so the log-content regression can assert that the
 * adapter's captured logs never contain them; the values are never logged
 * by the double itself.
 */
void wifi_provisioning_mock_submit_credentials(const char *ssid,
                                               const char *password);

/** @brief Accessor used by the log-content regression (owned buffer). */
const char *wifi_provisioning_mock_get_last_ssid(void);

/** @brief Accessor used by the log-content regression (owned buffer). */
const char *wifi_provisioning_mock_get_last_password(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PROVISIONING_MOCK_H */