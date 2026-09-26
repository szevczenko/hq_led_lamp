/**
 * @file tb_firmware_update_mock.h
 * @brief Controllable tb_firmware_update double for the ota_manager tests.
 */

#ifndef TB_FIRMWARE_UPDATE_MOCK_H
#define TB_FIRMWARE_UPDATE_MOCK_H

#include "tb_firmware_update.h"

typedef struct fw_mock {
    int init_calls;
    int init_result;
    tb_firmware_update_config_t last_cfg;
    char last_title[128];
    char last_version[128];
    tb_client_t *last_client;
    int deinit_calls;
    int request_check_calls;
    int request_check_result;
    int poll_calls;
    uint32_t last_poll_now;
    tb_firmware_update_status_t status;
} fw_mock_t;

extern fw_mock_t g_fw_mock;

void fw_mock_reset(void);
void fw_mock_set_state(tb_firmware_update_state_t state, size_t downloaded,
                       size_t total, const char *error);
void fw_mock_fire_reboot_required(void);

#endif /* TB_FIRMWARE_UPDATE_MOCK_H */
