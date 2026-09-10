/**
 * @file mqtt_app_mock.c
 * @brief Test-only mqtt_app double (see mqtt_app_mock.h).
 */

#include "mqtt_app_mock.h"

#include <stdbool.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* Mock state                                                             */
/* --------------------------------------------------------------------- */

static unsigned s_init_calls;
static bool s_started;
static bool s_connected;
static bool s_pending_connect;
static bool s_pending_failure;
static mqtt_connect_failure_reason_t s_pending_failure_reason;
static mqtt_connect_callback_t s_connect_cb;
static mqtt_disconnect_callback_t s_disconnect_cb;
static mqtt_connect_failure_callback_t s_connect_failure_cb;
static mqtt_connect_callback_t s_safety_connect_cb;
static mqtt_disconnect_callback_t s_safety_disconnect_cb;
static mqtt_connect_failure_callback_t s_safety_connect_failure_cb;
static mqtt_config_validation_callback_t s_validation_cb;

/* --------------------------------------------------------------------- */
/* Mock control                                                           */
/* --------------------------------------------------------------------- */

void mqtt_app_mock_reset(void)
{
    s_init_calls = 0u;
    s_started = false;
    s_connected = false;
    s_pending_connect = false;
    s_pending_failure = false;
    s_pending_failure_reason = MQTT_CONNECT_FAILURE_REASON_TRANSPORT_ERROR;
    s_connect_cb = NULL;
    s_disconnect_cb = NULL;
    s_connect_failure_cb = NULL;
    s_safety_connect_cb = NULL;
    s_safety_disconnect_cb = NULL;
    s_safety_connect_failure_cb = NULL;
    s_validation_cb = NULL;
}

unsigned mqtt_app_mock_init_calls(void)
{
    return s_init_calls;
}

bool mqtt_app_mock_is_connected(void)
{
    return s_connected;
}

void mqtt_app_mock_simulate_connect(void)
{
    if (!s_started)
    {
        s_pending_connect = true;
        return;
    }
    s_connected = true;
    if (s_safety_connect_cb != NULL)
    {
        s_safety_connect_cb();
    }
    if (s_connect_cb != NULL)
    {
        s_connect_cb();
    }
}

void mqtt_app_mock_simulate_connect_failure(mqtt_connect_failure_reason_t reason)
{
    if (!s_started)
    {
        s_pending_failure = true;
        s_pending_failure_reason = reason;
        return;
    }
    if (s_safety_connect_failure_cb != NULL)
    {
        s_safety_connect_failure_cb(reason);
    }
    if (s_connect_failure_cb != NULL)
    {
        s_connect_failure_cb(reason);
    }
}

void mqtt_app_mock_simulate_disconnect(mqtt_disconnect_reason_t reason)
{
    s_connected = false;
    if (s_safety_disconnect_cb != NULL)
    {
        s_safety_disconnect_cb(reason);
    }
    if (s_disconnect_cb != NULL)
    {
        s_disconnect_cb(reason);
    }
}

void mqtt_app_mock_simulate_apply_config(void)
{
    mqtt_config_snapshot_t snapshot;
    bool ssl = false;
    bool skip = false;
    mqtt_cert_source_t source = MQTT_CERT_SOURCE_NONE;
    const char *value = NULL;
    bool accepted = false;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.address = mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
    if (mqtt_config_get_bool(&ssl, MQTT_CONFIG_VALUE_SSL))
    {
        snapshot.ssl_enabled = ssl;
    }
    if (mqtt_config_get_bool(&skip, MQTT_CONFIG_VALUE_SKIP_VERIFY))
    {
        snapshot.skip_verify = skip;
    }
    if (mqtt_config_get_cert_source(&source, &value, MQTT_CONFIG_VALUE_CERT))
    {
        snapshot.cert_source = source;
        snapshot.cert_value = value;
    }
    if (mqtt_config_get_cert_source(&source, &value,
                                    MQTT_CONFIG_VALUE_CLIENT_CERT))
    {
        snapshot.client_cert_source = source;
        snapshot.client_cert_value = value;
    }
    if (mqtt_config_get_cert_source(&source, &value,
                                    MQTT_CONFIG_VALUE_CLIENT_KEY))
    {
        snapshot.client_key_source = source;
        snapshot.client_key_value = value;
    }

    /* Mirror the real apply-config handler (mqtt_app.c): the reconnect only
     * happens when a registered validation callback approves the exact
     * snapshot of values that would be used; otherwise the transport is left
     * disconnected and the failure observer is notified. */
    if (s_validation_cb != NULL)
    {
        accepted = s_validation_cb(&snapshot);
    }

    if (accepted)
    {
        mqtt_app_mock_simulate_connect();
    }
    else
    {
        if (s_connected)
        {
            mqtt_app_mock_simulate_disconnect(
                MQTT_DISCONNECT_REASON_EXPLICIT);
        }
        if (s_safety_connect_failure_cb != NULL)
        {
            s_safety_connect_failure_cb(
                MQTT_CONNECT_FAILURE_REASON_CONFIG_REJECTED);
        }
        if (s_connect_failure_cb != NULL)
        {
            s_connect_failure_cb(
                MQTT_CONNECT_FAILURE_REASON_CONFIG_REJECTED);
        }
    }
}

/* --------------------------------------------------------------------- */
/* mqtt_app implementation (the surface mqtt_cfg uses)                    */
/* --------------------------------------------------------------------- */

void mqtt_app_init(void)
{
    ++s_init_calls;
    s_started = true;
    /* A test may describe the transport result before mqtt_cfg_connect()
     * installs its observer.  Replay that result as an event after init,
     * matching the asynchronous real application layer. */
    if (s_pending_connect)
    {
        s_pending_connect = false;
        mqtt_app_mock_simulate_connect();
    }
    if (s_pending_failure)
    {
        mqtt_connect_failure_reason_t reason = s_pending_failure_reason;
        s_pending_failure = false;
        mqtt_app_mock_simulate_connect_failure(reason);
    }
}

void mqtt_app_deinit(void)
{
    s_started = false;
    s_connected = false;
}

bool mqtt_app_is_connected(void)
{
    return s_connected;
}

void mqtt_app_set_connect_callback(mqtt_connect_callback_t cb)
{
    s_connect_cb = cb;
}

void mqtt_app_set_disconnect_callback(mqtt_disconnect_callback_t cb)
{
    s_disconnect_cb = cb;
}

void mqtt_app_set_connect_failure_callback(
    mqtt_connect_failure_callback_t cb)
{
    s_connect_failure_cb = cb;
}

void mqtt_app_set_safety_callbacks(mqtt_connect_callback_t connect_cb,
                                   mqtt_disconnect_callback_t disconnect_cb,
                                   mqtt_connect_failure_callback_t failure_cb)
{
    s_safety_connect_cb = connect_cb;
    s_safety_disconnect_cb = disconnect_cb;
    s_safety_connect_failure_cb = failure_cb;
}

void mqtt_app_set_config_validation_callback(
    mqtt_config_validation_callback_t cb)
{
    s_validation_cb = cb;
}

bool mqtt_app_post_data(const char *topic, const char *message, int qos)
{
    (void)topic;
    (void)message;
    (void)qos;
    return false;
}

bool mqtt_app_subscribe(const char *topic, int qos,
                        mqtt_message_callback_t callback,
                        uint32_t timeout_ms)
{
    (void)topic;
    (void)qos;
    (void)callback;
    (void)timeout_ms;
    return false;
}

bool mqtt_app_unsubscribe(const char *topic, uint32_t timeout_ms)
{
    (void)topic;
    (void)timeout_ms;
    return false;
}

void mqtt_app_set_connection_policy(const mqtt_connection_policy_t *policy)
{
    (void)policy;
}