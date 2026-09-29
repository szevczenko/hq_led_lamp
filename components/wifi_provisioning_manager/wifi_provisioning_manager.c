/**
 * @file wifi_provisioning_manager.c
 * @brief Product-owned Wi-Fi provisioning adapter (TASK-126)
 *
 * Implementation of the documented wifi_provisioning_manager.h product
 * contract on top of the platform provisioning application
 * (wifi_http_provisioning.h), the shared Mongoose process
 * (mongoose_process.h) and the platform Wi-Fi manager saved-credentials
 * query (wifi_managment.h).
 *
 * Design rules enforced here:
 *
 *   1. Narrow surface: the ONLY platform headers included are
 *      wifi_http_provisioning.h, mongoose_process.h and wifi_managment.h,
 *      and only the operations the product contract needs are used
 *      (start_ex/stop/get_state/is_reachable, the runtime URL overrides
 *      under the test-only guard, MongooseProcess_IsRunning,
 *      wifi_mgmt_is_read_data).  The adapter starts the portal through the
 *      enum API wifi_http_provisioning_start_ex() so every documented
 *      platform failure mode is mapped to a distinct product status; the
 *      platform's bool wifi_http_provisioning_start() wrapper is left
 *      untouched for the fallback controller and any other platform-side
 *      caller (see rule 5 and wifi_provisioning_controller.c).  None of
 *      these platform types or include paths appear in the public header
 *      (include/wifi_provisioning_manager.h).
 *
 *   2. Secrecy: nothing that could contain an SSID, a password, a token or
 *      a provisioning URL with embedded credentials is ever accepted,
 *      stored or logged here.  The adapter logs state transitions, listener
 *      bind outcomes and error codes only.  TASK-135 adds three bounded,
 *      credential-free observability signatures: the portal-reachable edge
 *      ("provisioning AP up; portal reachable"), one line per distinct
 *      start failure carrying the mapped adapter error code (TASK-134), and
 *      a rate-limited (once per 30 s) "portal up; station not yet
 *      connected" line while the controller waits with the portal up.
 *
 *   3. Mongoose ownership: the portal rides the shared Mongoose process
 *      that also hosts MQTT/TLS.  start() checks MongooseProcess_IsRunning()
 *      up front and fails cleanly when the process is missing, and this
 *      module never calls MongooseProcess_Init()/Deinit() — stop() and
 *      deinit() close only the portal's own listeners and never tear the
 *      shared process down.
 *
 *   4. Serialization: init/deinit/start/stop are serialized by one OSAL
 *      mutex, so concurrent lifecycle calls can never interleave on the
 *      portal state or the test-only URL override buffers.  The adapter
 *      never executes its own logic on the Mongoose poll thread; the
 *      platform start/stop dispatch their listener work to the poll thread
 *      via MongooseProcess_Invoke() and block this CALLER until the poll
 *      thread completes it (the poll thread itself never blocks on the
 *      adapter).
 *
 *   5. Controller notification translation (TASK-132): init() registers a
 *      product notification handler with the platform automatic fallback
 *      controller (wifi_provisioning_controller_init_with_config(), the
 *      platform fallback-budget default from TASK-131 is left untouched).
 *      The handler honors the controller's session/generation token (a
 *      notification captured in a superseded lifecycle - after adapter
 *      deinit/re-init - is discarded) and translates transitions with a
 *      small adapter-private table into pending product events
 *      (STARTED/SUCCEEDED/FAILED) stored under the SAME adapter mutex; the
 *      supervisor consumes them via wifi_provisioning_manager_poll_event()
 *      (TASK-133).  The notification handler never calls back into the
 *      controller, never blocks on anything but the short adapter lock and
 *      never logs a credential, SSID, token or URL - only state codes.
 */

#include "wifi_provisioning_manager.h"

#include "osal_log.h"
#include "wifi_provisioning.h"

static wifi_provisioning_manager_status_t map_status(
    wifi_provisioning_status_t status,
    wifi_provisioning_manager_status_t fallback)
{
    switch (status)
    {
        case WIFI_PROVISIONING_OK:
            return WIFI_PROVISIONING_MANAGER_OK;
        case WIFI_PROVISIONING_ERR_NOT_INITIALIZED:
            return WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED;
        case WIFI_PROVISIONING_ERR_MONGOOSE_NOT_RUNNING:
            return WIFI_PROVISIONING_MANAGER_ERR_MONGOOSE_NOT_RUNNING;
        case WIFI_PROVISIONING_ERR_DEPENDENCY:
            return WIFI_PROVISIONING_MANAGER_ERR_DEPENDENCY;
        case WIFI_PROVISIONING_ERR_MODE_TRANSITION:
            return WIFI_PROVISIONING_MANAGER_ERR_MODE_TRANSITION;
        case WIFI_PROVISIONING_ERR_HTTP_BIND:
            return WIFI_PROVISIONING_MANAGER_ERR_HTTP_BIND;
        case WIFI_PROVISIONING_ERR_DNS_BIND:
            return WIFI_PROVISIONING_MANAGER_ERR_DNS_BIND;
        case WIFI_PROVISIONING_ERR_NO_AP:
            return WIFI_PROVISIONING_MANAGER_ERR_AP_NOT_UP;
        case WIFI_PROVISIONING_ERR_START_FAILED:
            return WIFI_PROVISIONING_MANAGER_ERR_START_FAILED;
        case WIFI_PROVISIONING_ERR_STOP_FAILED:
        case WIFI_PROVISIONING_ERR_RESOURCE:
        default:
            return fallback;
    }
}

wifi_provisioning_manager_status_t wifi_provisioning_manager_init(void)
{
    return map_status(wifi_provisioning_init(),
                      WIFI_PROVISIONING_MANAGER_ERR_START_FAILED);
}

wifi_provisioning_manager_status_t wifi_provisioning_manager_deinit(void)
{
    return map_status(wifi_provisioning_deinit(),
                      WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED);
}

wifi_provisioning_manager_status_t wifi_provisioning_manager_start(void)
{
    const wifi_provisioning_status_t platform_status = wifi_provisioning_start();
    const wifi_provisioning_manager_status_t status = map_status(
        platform_status, WIFI_PROVISIONING_MANAGER_ERR_START_FAILED);

    if (status != WIFI_PROVISIONING_MANAGER_OK)
    {
        osal_log_error("[prov_mgr] provisioning portal start failed "
                       "(adapter_status=%d)", (int)status);
    }
    return status;
}

wifi_provisioning_manager_status_t wifi_provisioning_manager_stop(void)
{
    return map_status(wifi_provisioning_stop(),
                      WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED);
}

wifi_provisioning_manager_state_t wifi_provisioning_manager_get_state(void)
{
    switch (wifi_provisioning_get_state())
    {
        case WIFI_PROVISIONING_STATE_STARTING:
            return WIFI_PROVISIONING_MANAGER_STARTING;
        case WIFI_PROVISIONING_STATE_RUNNING:
            return WIFI_PROVISIONING_MANAGER_RUNNING;
        case WIFI_PROVISIONING_STATE_STOPPING:
            return WIFI_PROVISIONING_MANAGER_STOPPING;
        case WIFI_PROVISIONING_STATE_ERROR:
            return WIFI_PROVISIONING_MANAGER_ERROR;
        case WIFI_PROVISIONING_STATE_STOPPED:
        default:
            return WIFI_PROVISIONING_MANAGER_STOPPED;
    }
}

bool wifi_provisioning_manager_is_active(void)
{
    return wifi_provisioning_is_active();
}

bool wifi_provisioning_manager_has_saved_credentials(void)
{
    return wifi_provisioning_has_saved_credentials();
}

wifi_provisioning_manager_event_t wifi_provisioning_manager_poll_event(void)
{
    switch (wifi_provisioning_poll_event())
    {
        case WIFI_PROVISIONING_EVENT_STARTED:
            return WIFI_PROVISIONING_MANAGER_EVENT_STARTED;
        case WIFI_PROVISIONING_EVENT_SUCCEEDED:
            return WIFI_PROVISIONING_MANAGER_EVENT_SUCCEEDED;
        case WIFI_PROVISIONING_EVENT_FAILED:
            return WIFI_PROVISIONING_MANAGER_EVENT_FAILED;
        case WIFI_PROVISIONING_EVENT_NONE:
        default:
            return WIFI_PROVISIONING_MANAGER_EVENT_NONE;
    }
}

#ifdef WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY
bool wifi_provisioning_manager_set_http_url(const char *url)
{
    return wifi_provisioning_set_http_url(url);
}

bool wifi_provisioning_manager_set_dns_url(const char *url)
{
    return wifi_provisioning_set_dns_url(url);
}
#endif
