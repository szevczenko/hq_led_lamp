/**
 * @file network_manager.c
 * @brief KLC compatibility adapter over the reusable hq_net platform API.
 *
 * KLC retains startup-mode policy and the synchronous lamp fail-off hook.
 * Lifecycle, session validation, and backend operations are owned by hq_net;
 * no platform types are exposed by network_manager.h.
 */

#include "network_manager.h"

#include <stddef.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#elif !defined(CONFIG_KLC_WIFI_DEFAULT_MODE_APSTA) && \
    !defined(CONFIG_KLC_WIFI_DEFAULT_MODE_CLIENT)
#define CONFIG_KLC_WIFI_DEFAULT_MODE_APSTA 1
#endif

#include "hq_net.h"
#include "lamp_control.h"
#include "osal_log.h"
#ifdef HQ_NET_TEST_OBSERVABILITY
#include "osal_mutex.h"
#endif

static void network_adapter_before_disconnected(void *context)
{
    (void)context;
    lamp_status_t status = lamp_control_force_inactive();
    if (status != LAMP_OK) {
        osal_log_error("[net] fail-off on network loss failed: %d",
                       (int)status);
    }
    osal_log_warning("[net] application network disconnected (lamp forced off)");
}

int network_manager_start(const network_callbacks_t *callbacks)
{
    if (callbacks == NULL || callbacks->on_connected == NULL ||
        callbacks->on_disconnected == NULL) {
        return NETWORK_ERR_INVALID_ARGUMENT;
    }

    hq_net_config_t config = {
        .backend = HQ_NET_BACKEND_WIFI,
#if defined(CONFIG_KLC_WIFI_DEFAULT_MODE_APSTA)
        .startup_mode = HQ_NET_STARTUP_MODE_PROVISIONING,
    .provisioning_ap_name = WIFI_AP_NAME,
    .provisioning_ap_password = WIFI_AP_PASSWORD,
#elif defined(CONFIG_KLC_WIFI_DEFAULT_MODE_CLIENT)
        .startup_mode = HQ_NET_STARTUP_MODE_STATION_ONLY,
#else
#error "Select CONFIG_KLC_WIFI_DEFAULT_MODE_APSTA or CONFIG_KLC_WIFI_DEFAULT_MODE_CLIENT"
#endif
    };
    hq_net_callbacks_t platform_callbacks = {
        .on_connected = callbacks->on_connected,
        .before_disconnected = network_adapter_before_disconnected,
        .on_disconnected = callbacks->on_disconnected,
        .context = callbacks->context,
    };

    return hq_net_start(&config, &platform_callbacks);
}

void network_manager_stop(void)
{
    hq_net_stop();
}

bool network_manager_is_connected(void)
{
    return hq_net_is_connected();
}

int network_manager_get_rssi(int *dbm)
{
    return hq_net_get_rssi(dbm) ? NETWORK_OK : NETWORK_ERR_START_FAILED;
}

bool network_manager_erase_credentials(void)
{
    return hq_net_forget_credentials();
}

int network_manager_reconnect(void)
{
    return hq_net_reconnect();
}

bool network_manager_wait_connected(uint32_t timeout_ms)
{
    return hq_net_wait_connected(timeout_ms);
}

#ifdef HQ_NET_TEST_OBSERVABILITY
extern osal_mutex_id_t hq_net_test_get_lock(void);

osal_mutex_id_t network_manager_test_get_lock(void)
{
    return hq_net_test_get_lock();
}
#endif
