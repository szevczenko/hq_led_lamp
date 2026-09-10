/**
 * @file mqtt_app_mock.h
 * @brief Test-only mqtt_app double for mqtt_cfg host tests (TASK-110)
 *
 * mqtt_cfg's verified TLS connection path (mqtt_cfg_connect()) drives the
 * Mongoose MQTT application layer through the small public surface in
 * mqtt_app.h.  This double records that surface and lets tests simulate
 * connect success, connect failure and the never-connects case so the
 * fail-off contract can be verified deterministically.
 *
 * Real TLS handshake coverage (trusted CA / unknown CA / hostname
 * mismatch) lives in mqtt_cfg_tls_it_test.c, which compiles the real
 * mqtt_app/mongoose transport against an in-process TLS MQTT broker.
 */

#ifndef MQTT_APP_MOCK_H
#define MQTT_APP_MOCK_H

#include "mqtt_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reset the double (no callbacks, disconnected, zero init calls). */
void mqtt_app_mock_reset(void);

/** @brief Number of mqtt_app_init() calls recorded. */
unsigned mqtt_app_mock_init_calls(void);

/** @brief Report whether mqtt_app_is_connected() would return true. */
bool mqtt_app_mock_is_connected(void);

/**
 * @brief Simulate the transport finishing its verified TLS connection.
 *
 * Marks the transport connected and invokes the registered connect
 * callback exactly like the real mqtt_app does from the Mongoose
 * event-loop context.
 */
void mqtt_app_mock_simulate_connect(void);

/**
 * @brief Simulate a transport/TLS connect failure.
 *
 * Invokes the registered connect-failure callback with @p reason.
 */
void mqtt_app_mock_simulate_connect_failure(
    mqtt_connect_failure_reason_t reason);

/** @brief Simulate a disconnect after an established MQTT session. */
void mqtt_app_mock_simulate_disconnect(mqtt_disconnect_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_APP_MOCK_H */