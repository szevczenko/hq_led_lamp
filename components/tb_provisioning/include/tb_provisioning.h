#ifndef TB_PROVISIONING_H
#define TB_PROVISIONING_H

#include <stdbool.h>
#include <stdint.h>

#include "tb_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_PROVISIONING_SCHEMA_VERSION 1U
#define TB_PROVISIONING_MAX_FILE_BYTES 2048U
#define TB_PROVISIONING_MAX_STRING_LEN 64U
#define TB_PROVISIONING_FILE_PATH "/config/provisioning.json"

typedef enum {
    TB_PROVISIONING_OK = 0,
    TB_PROVISIONING_NOT_FOUND = -1,
    TB_PROVISIONING_ERR_IO = -2,
    TB_PROVISIONING_ERR_MALFORMED = -3,
    TB_PROVISIONING_ERR_BOUNDS = -4,
    TB_PROVISIONING_ERR_UNKNOWN_SCHEMA = -5,
    TB_PROVISIONING_ERR_REQUEST = -6,
    TB_PROVISIONING_ERR_RESPONSE = -7,
    TB_PROVISIONING_ERR_PERSIST = -8,
} tb_provisioning_status_t;

typedef struct {
    char device_name[TB_PROVISIONING_MAX_STRING_LEN + 1U];
    char provision_device_key[TB_PROVISIONING_MAX_STRING_LEN + 1U];
    char provision_device_secret[TB_PROVISIONING_MAX_STRING_LEN + 1U];
} tb_provisioning_config_t;

const char *tb_provisioning_status_name(tb_provisioning_status_t status);
tb_provisioning_status_t tb_provisioning_load(tb_provisioning_config_t *config);
tb_provisioning_status_t tb_provisioning_enroll(
    tb_client_t *client, const tb_provisioning_config_t *config,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
