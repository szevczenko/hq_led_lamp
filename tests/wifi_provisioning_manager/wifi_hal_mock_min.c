/**
 * @file wifi_hal_mock_min.c
 * @brief Minimal test-only Wi-Fi HAL double (see wifi_hal_mock_min.h).
 */

#include "wifi_hal_mock_min.h"

#include <pthread.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* Internal state                                                         */
/* --------------------------------------------------------------------- */

typedef struct hal_mock_ctx
{
  bool            initialized;
  bool            started;
  wifi_hal_mode_t mode;
  wifi_hal_sta_config_t sta_cfg;
  bool            sta_cfg_set;
  wifi_hal_event_cb_t event_cb;
  void           *user_data;
  osal_status_t   connect_result;
  unsigned        connect_calls;
  unsigned        set_sta_config_calls;
  wifi_hal_ip_info_t ip_info;
} hal_mock_ctx_t;

static hal_mock_ctx_t   s_ctx;
static pthread_mutex_t  s_lock = PTHREAD_MUTEX_INITIALIZER;

/* --------------------------------------------------------------------- */
/* Helpers                                                                */
/* --------------------------------------------------------------------- */

static void mock_lock(void)
{
    (void)pthread_mutex_lock(&s_lock);
}

static void mock_unlock(void)
{
    (void)pthread_mutex_unlock(&s_lock);
}

/* --------------------------------------------------------------------- */
/* Test control API                                                       */
/* --------------------------------------------------------------------- */

void wifi_hal_mock_min_reset(void)
{
    mock_lock();
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.connect_result = OSAL_SUCCESS;
    mock_unlock();
}

void wifi_hal_mock_min_set_connect_result(osal_status_t result)
{
    mock_lock();
    s_ctx.connect_result = result;
    mock_unlock();
}

void wifi_hal_mock_min_inject_event(wifi_hal_event_t event,
                                    const wifi_hal_event_data_t *data)
{
    wifi_hal_event_cb_t cb;
    void               *user_data;
    wifi_hal_ip_info_t  ip_info;
    bool                has_ip;

    mock_lock();
    cb        = s_ctx.event_cb;
    user_data = s_ctx.user_data;
    has_ip    = (data != NULL);
    if (has_ip)
    {
        ip_info = data->ip_info;
    }
    mock_unlock();

    if (cb == NULL)
    {
        return;
    }

    cb(event, data, user_data);

    if (has_ip)
    {
        mock_lock();
        s_ctx.ip_info = ip_info;
        mock_unlock();
    }
}

unsigned wifi_hal_mock_min_connect_calls(void)
{
    unsigned calls;

    mock_lock();
    calls = s_ctx.connect_calls;
    mock_unlock();
    return calls;
}

bool wifi_hal_mock_min_get_sta_config(wifi_hal_sta_config_t *out)
{
    bool set;

    if (out == NULL)
    {
        return false;
    }
    mock_lock();
    set = s_ctx.sta_cfg_set;
    if (set)
    {
        *out = s_ctx.sta_cfg;
    }
    mock_unlock();
    return set;
}

/* --------------------------------------------------------------------- */
/* wifi_hal_driver.h contract                                             */
/* --------------------------------------------------------------------- */

osal_status_t wifi_hal_init(const wifi_hal_init_t *init)
{
    if (init == NULL || init->event_cb == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    mock_lock();
    s_ctx.initialized = true;
    s_ctx.event_cb    = init->event_cb;
    s_ctx.user_data   = init->user_data;
    mock_unlock();
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_deinit(void)
{
    mock_lock();
    s_ctx.initialized = false;
    s_ctx.started     = false;
    s_ctx.event_cb    = NULL;
    s_ctx.user_data   = NULL;
    mock_unlock();
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_start(wifi_hal_mode_t mode)
{
    mock_lock();
    s_ctx.started = true;
    s_ctx.mode    = mode;
    mock_unlock();
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_mode(wifi_hal_mode_t mode)
{
    mock_lock();
    s_ctx.mode = mode;
    mock_unlock();
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_stop(void)
{
    mock_lock();
    s_ctx.started = false;
    mock_unlock();
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_sta_config(const wifi_hal_sta_config_t *config)
{
    if (config == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    mock_lock();
    s_ctx.sta_cfg = *config;
    s_ctx.sta_cfg_set = true;
    ++s_ctx.set_sta_config_calls;
    mock_unlock();
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_ap_config(const wifi_hal_ap_config_t *config)
{
    if (config == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    /* Not observed by the overwrite assertions; accepted unconditionally. */
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_connect(void)
{
    osal_status_t result;

    mock_lock();
    ++s_ctx.connect_calls;
    result = s_ctx.connect_result;
    mock_unlock();
    return result;
}

osal_status_t wifi_hal_disconnect(void)
{
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_start_scan(bool block)
{
    (void)block;
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_scanned_ap(wifi_hal_ap_record_t *records,
                                      uint16_t *in_out_count)
{
    if (records == NULL || in_out_count == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    *in_out_count = 0U;
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_ip_info(wifi_hal_ip_info_t *out_info)
{
    if (out_info == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    mock_lock();
    *out_info = s_ctx.ip_info;
    mock_unlock();
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_rssi(int *out_rssi)
{
    if (out_rssi == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    *out_rssi = -50;
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_power_save(bool enabled)
{
    (void)enabled;
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_default_mac(uint8_t mac[6])
{
    if (mac == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    mac[0] = 0xAA;
    mac[1] = 0xBB;
    mac[2] = 0xCC;
    mac[3] = 0xDD;
    mac[4] = 0xEE;
    mac[5] = 0x01;
    return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_client_count(uint32_t *out_client_count)
{
    if (out_client_count == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    *out_client_count = 0U;
    return OSAL_SUCCESS;
}