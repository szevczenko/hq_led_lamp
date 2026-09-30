#include "device_identity.h"

#include "app_config.h"
#include "lamp_control.h"
#include "tb_identity.h"

static bool s_loaded;

const char *device_identity_status_name(device_identity_status_t status)
{
    switch (status) {
        case DEVICE_IDENTITY_OK: return "ok";
        case DEVICE_IDENTITY_ERR_INVALID_ARGUMENT: return "invalid_argument";
        case DEVICE_IDENTITY_ERR_IO: return "io";
        case DEVICE_IDENTITY_ERR_NOT_FOUND: return "not_found";
        case DEVICE_IDENTITY_ERR_MALFORMED: return "malformed";
        case DEVICE_IDENTITY_ERR_BOUNDS: return "bounds";
        case DEVICE_IDENTITY_ERR_UNKNOWN_SCHEMA: return "unknown_schema";
        case DEVICE_IDENTITY_ERR_UNPROVISIONED: return "unprovisioned";
        case DEVICE_IDENTITY_ERR_EMPTY_CLIENT_ID: return "empty_client_id";
        case DEVICE_IDENTITY_ERR_EMPTY_TOKEN: return "empty_token";
        case DEVICE_IDENTITY_ERR_NOT_LOADED: return "not_loaded";
        default: return "unknown";
    }
}

static void device_identity_fail_off(void)
{
    (void)lamp_control_force_inactive();
}

static device_identity_status_t device_identity_check_provisioning(void)
{
    app_config_manufacturing_doc_t manufacturing;
    app_config_status_t status = app_config_load_manufacturing(&manufacturing);
    if (status != APP_CONFIG_OK && status != APP_CONFIG_OK_RECOVERED) {
        if (status == APP_CONFIG_ERR_NOT_FOUND) {
            return DEVICE_IDENTITY_ERR_UNPROVISIONED;
        }
        if (status == APP_CONFIG_ERR_BOUNDS) {
            return DEVICE_IDENTITY_ERR_BOUNDS;
        }
        if (status == APP_CONFIG_ERR_UNKNOWN_SCHEMA) {
            return DEVICE_IDENTITY_ERR_UNKNOWN_SCHEMA;
        }
        if (status == APP_CONFIG_ERR_IO || status == APP_CONFIG_ERR_UNSUPPORTED) {
            return DEVICE_IDENTITY_ERR_IO;
        }
        if (status == APP_CONFIG_ERR_INVALID_ARGUMENT) {
            return DEVICE_IDENTITY_ERR_INVALID_ARGUMENT;
        }
        return DEVICE_IDENTITY_ERR_MALFORMED;
    }
    if (manufacturing.manufacturing_state != APP_CONFIG_MFG_STATE_PROVISIONED ||
        manufacturing.credential_mode == APP_CONFIG_CRED_MODE_NONE) {
        return DEVICE_IDENTITY_ERR_UNPROVISIONED;
    }
    return DEVICE_IDENTITY_OK;
}

static device_identity_status_t device_identity_map_store_status(
    tb_identity_status_t status)
{
    switch (status) {
        case TB_IDENTITY_OK: return DEVICE_IDENTITY_OK;
        case TB_IDENTITY_ERR_ARGUMENT: return DEVICE_IDENTITY_ERR_INVALID_ARGUMENT;
        case TB_IDENTITY_ERR_IO: return DEVICE_IDENTITY_ERR_IO;
        case TB_IDENTITY_ERR_NOT_FOUND: return DEVICE_IDENTITY_ERR_NOT_FOUND;
        case TB_IDENTITY_ERR_MALFORMED: return DEVICE_IDENTITY_ERR_MALFORMED;
        case TB_IDENTITY_ERR_BOUNDS: return DEVICE_IDENTITY_ERR_BOUNDS;
        case TB_IDENTITY_ERR_UNKNOWN_SCHEMA: return DEVICE_IDENTITY_ERR_UNKNOWN_SCHEMA;
        case TB_IDENTITY_ERR_EMPTY_CLIENT_ID: return DEVICE_IDENTITY_ERR_EMPTY_CLIENT_ID;
        case TB_IDENTITY_ERR_EMPTY_TOKEN: return DEVICE_IDENTITY_ERR_EMPTY_TOKEN;
        default: return DEVICE_IDENTITY_ERR_IO;
    }
}

device_identity_status_t device_identity_load(void)
{
    device_identity_clear();
    device_identity_status_t status = device_identity_check_provisioning();
    if (status == DEVICE_IDENTITY_OK) {
        status = device_identity_map_store_status(
            tb_identity_load(DEVICE_IDENTITY_FILE_PATH));
        s_loaded = status == DEVICE_IDENTITY_OK;
    }
    if (status != DEVICE_IDENTITY_OK) {
        tb_identity_clear();
        s_loaded = false;
        device_identity_fail_off();
    }
    return status;
}

bool device_identity_is_loaded(void)
{
    return s_loaded && tb_identity_is_loaded();
}

void device_identity_clear(void)
{
    tb_identity_clear();
    s_loaded = false;
}

device_identity_status_t device_identity_client_id(const char **out_client_id)
{
    if (out_client_id == NULL) {
        return DEVICE_IDENTITY_ERR_INVALID_ARGUMENT;
    }
    const tb_identity_credentials_t *credentials = tb_identity_get();
    if (!s_loaded || credentials == NULL) {
        *out_client_id = NULL;
        return DEVICE_IDENTITY_ERR_NOT_LOADED;
    }
    *out_client_id = credentials->client_id;
    return DEVICE_IDENTITY_OK;
}

device_identity_status_t device_identity_token(const char **out_token)
{
    if (out_token == NULL) {
        return DEVICE_IDENTITY_ERR_INVALID_ARGUMENT;
    }
    const tb_identity_credentials_t *credentials = tb_identity_get();
    if (!s_loaded || credentials == NULL) {
        *out_token = NULL;
        return DEVICE_IDENTITY_ERR_NOT_LOADED;
    }
    *out_token = credentials->access_token;
    return DEVICE_IDENTITY_OK;
}

static const char *device_identity_provide_token(void)
{
    const tb_identity_credentials_t *credentials = tb_identity_get();
    return s_loaded && credentials != NULL ? credentials->access_token : NULL;
}

device_identity_token_provider_t device_identity_token_provider(void)
{
    return device_identity_provide_token;
}