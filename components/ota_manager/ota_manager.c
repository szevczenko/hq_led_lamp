/**
 * @file ota_manager.c
 * @brief ThingsBoard OTA supervisor (see ota_manager.h).
 */

#include "ota_manager.h"

#include "tb_ota_manager.h"

static ota_manager_config_t s_config;

static void ota_adapter_on_state(tb_ota_manager_event_t event, void *ctx)
{
    (void)ctx;
    if (s_config.on_event != NULL)
    {
        s_config.on_event((ota_manager_event_t)event, s_config.ctx);
    }
}

static void ota_adapter_indicator(bool on, void *ctx)
{
    (void)ctx;
    if (s_config.on_indicator != NULL)
    {
        s_config.on_indicator(on, s_config.ctx);
    }
}

static void ota_adapter_restart(void *ctx)
{
    (void)ctx;
    s_config.restart();
}

static int ota_adapter_confirm(tb_client_t *client, void *ctx)
{
    (void)ctx;
    return s_config.confirm_health != NULL
               ? s_config.confirm_health(client, s_config.ctx)
               : 1;
}

ota_manager_status_t ota_manager_init(const ota_manager_config_t *config)
{
    tb_ota_manager_config_t platform_config;

    if ((config == NULL) || (config->title == NULL) ||
        (config->version == NULL) || (config->restart == NULL))
    {
        return OTA_MANAGER_ERR_INVALID_ARGUMENT;
    }

    platform_config = (tb_ota_manager_config_t){
        .title = config->title,
        .version = config->version,
        .chunk_size = config->chunk_size,
        .chunk_timeout_ms = config->chunk_timeout_ms,
        .chunk_retries = config->chunk_retries,
        .check_period_ms = config->check_period_ms,
        .reboot_delay_ms = config->reboot_delay_ms,
        .blink_period_ms = config->blink_period_ms,
        .init_retry_ms = config->init_retry_ms,
        .confirm_retry_ms = config->confirm_retry_ms,
        .now_ms = config->now_ms,
        .on_state = ota_adapter_on_state,
        .set_indicator = ota_adapter_indicator,
        .restart = ota_adapter_restart,
        .confirm_health = config->confirm_health != NULL
                      ? ota_adapter_confirm
                      : NULL,
        .ctx = config->ctx,
    };

    tb_ota_manager_status_t status = tb_ota_manager_init(&platform_config);
    if (status == TB_OTA_MANAGER_OK)
    {
        s_config = *config;
    }
    return (ota_manager_status_t)status;
}

void ota_manager_deinit(void)
{
    tb_ota_manager_deinit();
}

void ota_manager_on_connected(tb_client_t *client)
{
    tb_ota_manager_on_connected(client);
}

void ota_manager_on_disconnected(void)
{
    tb_ota_manager_on_disconnected();
}

void ota_manager_request_check(void)
{
    tb_ota_manager_request_check();
}

void ota_manager_request_image_confirmation(void)
{
    tb_ota_manager_request_image_confirmation();
}

void ota_manager_poll(void)
{
    tb_ota_manager_poll();
}

bool ota_manager_is_active(void)
{
    return tb_ota_manager_is_active();
}
