/**
 * @file tb_firmware_update_mock.c
 * @brief tb_firmware_update + minimal OSAL doubles for the ota_manager tests.
 */

#include "tb_firmware_update_mock.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "osal_log.h"
#include "osal_task.h"

fw_mock_t g_fw_mock;

void fw_mock_reset(void)
{
    memset(&g_fw_mock, 0, sizeof(g_fw_mock));
}

void fw_mock_set_state(tb_firmware_update_state_t state, size_t downloaded,
                       size_t total, const char *error)
{
    g_fw_mock.status.state = state;
    g_fw_mock.status.downloaded_size = downloaded;
    g_fw_mock.status.total_size = total;
    snprintf(g_fw_mock.status.target_title,
             sizeof(g_fw_mock.status.target_title), "%s",
             "kitchen_led_controller");
    snprintf(g_fw_mock.status.target_version,
             sizeof(g_fw_mock.status.target_version), "%s", "1.0.1");
    snprintf(g_fw_mock.status.last_error,
             sizeof(g_fw_mock.status.last_error), "%s",
             error != NULL ? error : "");
}

void fw_mock_fire_reboot_required(void)
{
    if (g_fw_mock.last_cfg.on_reboot_required != NULL)
    {
        g_fw_mock.last_cfg.on_reboot_required("kitchen_led_controller",
                                              "1.0.1",
                                              g_fw_mock.last_cfg.user_data);
    }
}

int tb_firmware_update_init(tb_client_t *client,
                            const tb_firmware_update_config_t *config)
{
    g_fw_mock.init_calls++;
    g_fw_mock.last_client = client;
    g_fw_mock.last_cfg = *config;
    snprintf(g_fw_mock.last_title, sizeof(g_fw_mock.last_title), "%s",
             config->current_title);
    snprintf(g_fw_mock.last_version, sizeof(g_fw_mock.last_version), "%s",
             config->current_version);
    return g_fw_mock.init_result;
}

int tb_firmware_update_request_check(tb_client_t *client)
{
    (void)client;
    g_fw_mock.request_check_calls++;
    return g_fw_mock.request_check_result;
}

int tb_firmware_update_confirm_health(tb_client_t *client)
{
    (void)client;
    return 0;
}

bool tb_firmware_update_is_in_progress(void)
{
    return g_fw_mock.status.state == TB_FIRMWARE_UPDATE_STATE_DOWNLOADING;
}

void tb_firmware_update_poll(tb_client_t *client, uint32_t now_ms)
{
    (void)client;
    g_fw_mock.poll_calls++;
    g_fw_mock.last_poll_now = now_ms;
}

int tb_firmware_update_get_status(tb_firmware_update_status_t *status)
{
    *status = g_fw_mock.status;
    return 0;
}

const char *tb_firmware_update_state_name(tb_firmware_update_state_t state)
{
    static const char *const names[] = {
        "IDLE", "DOWNLOADING", "DOWNLOADED", "VERIFIED",
        "UPDATING", "UPDATED", "FAILED",
    };
    return ((unsigned)state < (sizeof(names) / sizeof(names[0])))
               ? names[state] : "FAILED";
}

void tb_firmware_update_deinit(tb_client_t *client)
{
    (void)client;
    g_fw_mock.deinit_calls++;
}

void osal_log_printf(const char *level, const char *format, ...)
{
    va_list args;

    printf("[%s]: ", level);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

uint32_t osal_task_get_time_ms(void)
{
    return 0u;
}
