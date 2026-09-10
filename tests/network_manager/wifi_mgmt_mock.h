/**
 * @file wifi_mgmt_mock.h
 * @brief Test-only Wi-Fi manager double for network-adapter host tests
 *        (TASK-109)
 *
 * Implements the platform Wi-Fi manager contract (wifi_managment.h) as an
 * in-memory test double so the product-owned network adapter
 * (components/network_manager) can be exercised on the host without any
 * platform Wi-Fi driver:
 *
 *   - the double tracks the manager lifecycle (init/start/stop/connect/
 *     disconnect) and the connected/ready state,
 *   - subscriptions are recorded per event; wifi_mgmt_mock_emit() delivers
 *     an event synchronously to every matching subscriber (the production
 *     manager dispatches from its worker task — the synchronous double
 *     still exercises the same adapter code paths deterministically),
 *   - failure injection lets tests flip wait-ready / connect / subscribe
 *     results so every adapter failure and rollback path is reachable,
 *   - call counters expose how often each manager operation was invoked so
 *     tests can assert the onboarding order (init -> subscribe -> start ->
 *     connect) and the stop behavior.
 *
 * Secrecy: the double contains no SSID/password API at all.  The adapter
 * under test must not reference credential persistence (wifi_config), and
 * these tests verify that the adapter never logs anything credential-like.
 *
 * This file is a test double only: it is compiled solely into the
 * network-manager host test binary and is never part of any production
 * build.
 */

#ifndef WIFI_MGMT_MOCK_H
#define WIFI_MGMT_MOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "wifi_managment.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Failure injection                                                      */
/* --------------------------------------------------------------------- */

typedef struct wifi_mock_config
{
  bool fail_wait_ready;    /**< wifi_mgmt_wait_ready() returns false.     */
  bool fail_connect;       /**< wifi_mgmt_connect() returns false.        */
  bool fail_subscribe;     /**< wifi_mgmt_subscribe() returns false.      */
  bool connected_state;    /**< Value reported by wifi_mgmt_is_connected. */
} wifi_mock_config_t;

/* --------------------------------------------------------------------- */
/* Test control API                                                       */
/* --------------------------------------------------------------------- */

/** @brief Reset the double to a fresh, never-initialized state. */
void wifi_mgmt_mock_reset(void);

/** @brief Apply a failure/state configuration (before start typically). */
void wifi_mgmt_mock_set_config(const wifi_mock_config_t *config);

/**
 * @brief Deliver an event synchronously to all matching subscribers.
 * @return Number of subscribers the event was delivered to.
 */
int wifi_mgmt_mock_emit(wifi_mgmt_event_t event);

/** @brief Set the connected state reported by wifi_mgmt_is_connected(). */
void wifi_mgmt_mock_set_connected(bool connected);

/**
 * @brief First callback currently subscribed to @p event (NULL if none).
 *
 * Lets a test invoke the adapter's handler directly after an unsubscribe,
 * which is exactly the documented late-callback race (the manager
 * snapshots subscriptions before dispatching).
 */
wifi_mgmt_event_cb_t wifi_mgmt_mock_get_subscribed_cb(wifi_mgmt_event_t event);

/* --------------------------------------------------------------------- */
// Call counters / observations
/* --------------------------------------------------------------------- */

typedef struct wifi_mock_counters
{
  unsigned set_type_calls;
  unsigned init_calls;
  unsigned start_calls;
  unsigned stop_calls;
  unsigned connect_calls;
  unsigned disconnect_calls;
  unsigned subscribe_calls;
  unsigned unsubscribe_calls;
  wifi_type_t last_type;
  bool start_before_connect; /**< start observed before first connect.     */
  unsigned init_before_start_violations;
} wifi_mock_counters_t;

/** @brief Snapshot of the accumulated call counters. */
wifi_mock_counters_t wifi_mgmt_mock_get_counters(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MGMT_MOCK_H */
