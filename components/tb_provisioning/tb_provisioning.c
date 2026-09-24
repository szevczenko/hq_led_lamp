#include "tb_provisioning.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "osal_file.h"
#include "osal_task.h"
#include "tb_provision.h"

#define IDENTITY_PATH "/config/identity.json"
#define IDENTITY_TMP_PATH "/config/identity.json.tmp"
#define IDENTITY_CLIENT_ID_MAX 64U
#define IDENTITY_TOKEN_MAX 128U

typedef struct {
    volatile bool ready;
    volatile bool valid;
    char token[IDENTITY_TOKEN_MAX + 1U];
} response_state_t;

static bool printable_value(const char *value, size_t max_len, bool no_space)
{
    size_t length;
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    length = strnlen(value, max_len + 1U);
    if (length == 0U || length > max_len) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (ch < 0x20U || ch > 0x7eU || (no_space && ch == ' ')) {
            return false;
        }
    }
    return true;
}

static bool exact_members(const cJSON *root)
{
    const char *const allowed[] = {
        "schema_version", "device_name", "provision_device_key",
        "provision_device_secret"
    };
    size_t count = 0U;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (item->string == NULL) {
            return false;
        }
        bool known = false;
        for (size_t i = 0U; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
            if (strcmp(item->string, allowed[i]) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            return false;
        }
        for (const cJSON *next = item->next; next != NULL; next = next->next) {
            if (next->string != NULL && strcmp(item->string, next->string) == 0) {
                return false;
            }
        }
        ++count;
    }
    return count == sizeof(allowed) / sizeof(allowed[0]);
}

const char *tb_provisioning_status_name(tb_provisioning_status_t status)
{
    switch (status) {
    case TB_PROVISIONING_OK: return "ok";
    case TB_PROVISIONING_NOT_FOUND: return "not_found";
    case TB_PROVISIONING_ERR_IO: return "io";
    case TB_PROVISIONING_ERR_MALFORMED: return "malformed";
    case TB_PROVISIONING_ERR_BOUNDS: return "bounds";
    case TB_PROVISIONING_ERR_UNKNOWN_SCHEMA: return "unknown_schema";
    case TB_PROVISIONING_ERR_REQUEST: return "request";
    case TB_PROVISIONING_ERR_RESPONSE: return "response";
    case TB_PROVISIONING_ERR_PERSIST: return "persist";
    default: return "unknown";
    }
}

static int read_file(const char *path, char *buffer, size_t capacity)
{
    osal_fstat_t stat = { 0 };
    osal_file_id_t file;
    int32_t count;
    if (osal_stat(path, &stat) != OSAL_SUCCESS || stat.file_size == 0U ||
        stat.file_size >= capacity) {
        return -1;
    }
    file = osal_open_create(path, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    if (file < 0) {
        return -1;
    }
    count = osal_read(file, buffer, stat.file_size);
    (void)osal_close(file);
    if (count <= 0 || (size_t)count >= capacity) {
        return -1;
    }
    buffer[count] = '\0';
    return count;
}

static bool parse_config(const char *json, tb_provisioning_config_t *config)
{
    cJSON *root = cJSON_ParseWithLengthOpts(json, strlen(json), NULL, false);
    cJSON *version;
    cJSON *name;
    cJSON *key;
    cJSON *secret;
    if (root == NULL || !cJSON_IsObject(root) || !exact_members(root)) {
        cJSON_Delete(root);
        return false;
    }
    version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    name = cJSON_GetObjectItemCaseSensitive(root, "device_name");
    key = cJSON_GetObjectItemCaseSensitive(root, "provision_device_key");
    secret = cJSON_GetObjectItemCaseSensitive(root, "provision_device_secret");
    if (!cJSON_IsNumber(version) || version->valuedouble != TB_PROVISIONING_SCHEMA_VERSION ||
        !cJSON_IsString(name) || !cJSON_IsString(key) || !cJSON_IsString(secret) ||
        !printable_value(name->valuestring, TB_PROVISIONING_MAX_STRING_LEN, false) ||
        !printable_value(key->valuestring, TB_PROVISIONING_MAX_STRING_LEN, true) ||
        !printable_value(secret->valuestring, TB_PROVISIONING_MAX_STRING_LEN, true)) {
        cJSON_Delete(root);
        return false;
    }
    memset(config, 0, sizeof(*config));
    (void)strncpy(config->device_name, name->valuestring, sizeof(config->device_name) - 1U);
    (void)strncpy(config->provision_device_key, key->valuestring,
                  sizeof(config->provision_device_key) - 1U);
    (void)strncpy(config->provision_device_secret, secret->valuestring,
                  sizeof(config->provision_device_secret) - 1U);
    cJSON_Delete(root);
    return true;
}

tb_provisioning_status_t tb_provisioning_load(tb_provisioning_config_t *config)
{
    char buffer[TB_PROVISIONING_MAX_FILE_BYTES + 1U];
    if (config == NULL) {
        return TB_PROVISIONING_ERR_MALFORMED;
    }
    if (osal_stat(TB_PROVISIONING_FILE_PATH, &(osal_fstat_t){ 0 }) != OSAL_SUCCESS) {
        return TB_PROVISIONING_NOT_FOUND;
    }
    if (read_file(TB_PROVISIONING_FILE_PATH, buffer, sizeof(buffer)) < 0) {
        return TB_PROVISIONING_ERR_IO;
    }
    if (!parse_config(buffer, config)) {
        memset(buffer, 0, sizeof(buffer));
        return TB_PROVISIONING_ERR_MALFORMED;
    }
    memset(buffer, 0, sizeof(buffer));
    return TB_PROVISIONING_OK;
}

static void response_callback(const char *json, void *user_data)
{
    response_state_t *state = (response_state_t *)user_data;
    cJSON *root;
    cJSON *status;
    cJSON *type;
    cJSON *value;
    if (json == NULL || state == NULL) {
        return;
    }
    root = cJSON_Parse(json);
    status = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "status");
    type = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "credentialsType");
    value = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "credentialsValue");
    if (cJSON_IsString(status) && strcmp(status->valuestring, "SUCCESS") == 0 &&
        cJSON_IsString(type) && strcmp(type->valuestring, "ACCESS_TOKEN") == 0 &&
        cJSON_IsString(value) && printable_value(value->valuestring, IDENTITY_TOKEN_MAX, true)) {
        (void)strncpy(state->token, value->valuestring, sizeof(state->token) - 1U);
        state->valid = true;
    }
    state->ready = true;
    cJSON_Delete(root);
}

static bool write_identity(const tb_provisioning_config_t *config, const char *token)
{
    cJSON *root = cJSON_CreateObject();
    char *json;
    osal_file_id_t file;
    bool success = false;
    if (root == NULL || cJSON_AddNumberToObject(root, "schema_version", 1) == NULL ||
        cJSON_AddStringToObject(root, "client_id", config->device_name) == NULL ||
        cJSON_AddStringToObject(root, "access_token", token) == NULL) {
        cJSON_Delete(root);
        return false;
    }
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return false;
    }
    file = osal_open_create(IDENTITY_TMP_PATH,
                            OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
                            OSAL_WRITE_ONLY);
    if (file >= 0) {
        int32_t written = osal_write(file, json, strlen(json));
        (void)osal_close(file);
        success = written == (int32_t)strlen(json) &&
                  osal_rename(IDENTITY_TMP_PATH, IDENTITY_PATH) == OSAL_SUCCESS;
    }
    cJSON_free(json);
    return success;
}

tb_provisioning_status_t tb_provisioning_enroll(
    tb_client_t *client, const tb_provisioning_config_t *config,
    uint32_t timeout_ms)
{
    response_state_t response = { 0 };
    tb_provision_request_t request;
    uint32_t start;
    if (client == NULL || config == NULL || timeout_ms == 0U) {
        return TB_PROVISIONING_ERR_REQUEST;
    }
    request = (tb_provision_request_t){
        .device_name = config->device_name,
        .provision_device_key = config->provision_device_key,
        .provision_device_secret = config->provision_device_secret,
    };
    if (tb_provision_request(client, &request, response_callback, &response,
                             timeout_ms) != 0) {
        return TB_PROVISIONING_ERR_REQUEST;
    }
    start = osal_task_get_time_ms();
    while (!response.ready &&
           (uint32_t)(osal_task_get_time_ms() - start) < timeout_ms) {
        osal_task_delay_ms(50U);
    }
    if (!response.ready || !response.valid) {
        /* Unregister the callback before giving up: `response` is about to
         * go out of scope, and a reply that arrives after this point must
         * never be delivered into its (now stale) stack storage. */
        tb_provision_cancel(client);
        memset(response.token, 0, sizeof(response.token));
        return TB_PROVISIONING_ERR_RESPONSE;
    }
    if (!write_identity(config, response.token) ||
        osal_remove(TB_PROVISIONING_FILE_PATH) != OSAL_SUCCESS) {
        memset(response.token, 0, sizeof(response.token));
        return TB_PROVISIONING_ERR_PERSIST;
    }
    memset(response.token, 0, sizeof(response.token));
    return TB_PROVISIONING_OK;
}
