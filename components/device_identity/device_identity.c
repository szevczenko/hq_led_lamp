/**
 * @file device_identity.c
 * @brief Access-token device identity implementation (TASK-111)
 *
 * See device_identity.h for the normative contract.  Implementation notes:
 *
 *   - the load path validates the manufacturing record through the app_config
 *     service first ("load only validated manufacturing/configuration
 *     records") and only then reads the bounded identity record
 *     /config/identity.json through the portable OSAL file API,
 *   - the record is parsed with a strict cJSON schema: the whole input must
 *     be consumed, unknown and duplicate members are rejected, and every
 *     string value is bounds- and charset-checked before it is published,
 *   - the token is a bearer credential.  It is never logged (this module
 *     performs no logging at all; the application layer logs only status
 *     names).  Every temporary secret-bearing copy — the raw record buffer
 *     and the parser's value copy — is zeroized before being freed, so after
 *     a successful load exactly one copy remains, in module-owned storage,
 *     and device_identity_clear() zeroizes it,
 *   - every load failure forces the lamp output inactive through
 *     lamp_control_force_inactive() (fail-off contract): a missing or
 *     invalid identity is fatal for ThingsBoard initialization and never
 *     falls back to anonymous or plaintext connectivity.
 */

#include "device_identity.h"

#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "osal_error.h"
#include "osal_file.h"

#include "cJSON.h"

#include "lamp_control.h"

/* --------------------------------------------------------------------- */
/* Module-owned identity state                                            */
/* --------------------------------------------------------------------- */

static char s_client_id[DEVICE_IDENTITY_CLIENT_ID_MAX_LEN + 1U];
static char s_token[DEVICE_IDENTITY_TOKEN_MAX_LEN + 1U];
static bool s_loaded;

/**
 * @brief Zeroize a buffer in a way the compiler cannot optimize away.
 *
 * Used for the bounded temporary copies of the record (which contains the
 * token) and for the module-owned token on clear()/re-load.
 */
static void device_identity_zerobuf(char *buf, size_t len)
{
    if (buf == NULL)
    {
        return;
    }
    volatile char *p = (volatile char *)buf;
    while (len-- > 0u)
    {
        *p++ = (char)0;
    }
}

static void device_identity_invalidate(void)
{
    device_identity_zerobuf(s_token, sizeof(s_token));
    device_identity_zerobuf(s_client_id, sizeof(s_client_id));
    s_loaded = false;
}

/* --------------------------------------------------------------------- */
/* Status names                                                           */
/* --------------------------------------------------------------------- */

const char *device_identity_status_name(device_identity_status_t status)
{
    switch (status)
    {
        case DEVICE_IDENTITY_OK:                   return "ok";
        case DEVICE_IDENTITY_ERR_INVALID_ARGUMENT: return "invalid_argument";
        case DEVICE_IDENTITY_ERR_IO:               return "io";
        case DEVICE_IDENTITY_ERR_NOT_FOUND:        return "not_found";
        case DEVICE_IDENTITY_ERR_MALFORMED:        return "malformed";
        case DEVICE_IDENTITY_ERR_BOUNDS:           return "bounds";
        case DEVICE_IDENTITY_ERR_UNKNOWN_SCHEMA:   return "unknown_schema";
        case DEVICE_IDENTITY_ERR_UNPROVISIONED:    return "unprovisioned";
        case DEVICE_IDENTITY_ERR_EMPTY_CLIENT_ID:  return "empty_client_id";
        case DEVICE_IDENTITY_ERR_EMPTY_TOKEN:      return "empty_token";
        case DEVICE_IDENTITY_ERR_NOT_LOADED:       return "not_loaded";
        default:                                   return "unknown";
    }
}

/* --------------------------------------------------------------------- */
/* Validation helpers                                                     */
/* --------------------------------------------------------------------- */

/**
 * @brief Printable-ASCII check with a bounded scan.
 *
 * The fixed-size arrays must be NUL-terminated within their capacity before
 * any unbounded read; strnlen() therefore scans at most max_len + 1 bytes
 * and a missing terminator yields a length above max_len (rejected).
 *
 * @param strict_whitespace When true, whitespace (0x20) is also rejected —
 *                          used for the token, which must be a single
 *                          whitespace-free value.
 */
static bool device_identity_is_printable(const char *str, size_t max_len,
                                         bool strict_whitespace)
{
    size_t len;

    if (str == NULL || str[0] == '\0')
    {
        return false;
    }

    len = strnlen(str, max_len + 1u);
    if (len > max_len)
    {
        return false;
    }

    for (size_t i = 0u; i < len; ++i)
    {
        unsigned char ch = (unsigned char)str[i];
        if (ch < 0x20u || ch > 0x7Eu)
        {
            return false;
        }
        if (strict_whitespace && ch == 0x20u)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Reject unknown members, unnamed members and duplicate keys.
 *
 * Together with the "every required member must be present" rule in the
 * parser this forces the object to contain exactly the schema's key set,
 * so no secret or policy override can be smuggled into the record.
 */
static bool device_identity_check_member_keys(const cJSON *object,
                                              const char *const *allowed,
                                              size_t allowed_count)
{
    const cJSON *child = NULL;
    size_t members = 0u;

    cJSON_ArrayForEach(child, object)
    {
        if ((child->string == NULL) || (child->string[0] == '\0'))
        {
            return false;
        }

        bool known = false;
        for (size_t i = 0u; i < allowed_count; ++i)
        {
            if (strcmp(child->string, allowed[i]) == 0)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            return false;
        }

        /* Duplicate-key detection: cJSON keeps duplicate members as separate
         * linked-list items, so a repeated key is only caught by comparing
         * each key against every later member (the record size is bounded,
         * making the O(n^2) scan trivial here). */
        for (const cJSON *probe = child->next; probe != NULL;
             probe = probe->next)
        {
            if ((probe->string != NULL) &&
                (strcmp(probe->string, child->string) == 0))
            {
                return false;
            }
        }
        ++members;
    }

    return members <= allowed_count;
}

/** @brief Escape-scan: an escaped NUL decodes into an embedded 0 byte. */
static bool device_identity_raw_json_has_nul_escape(const char *json)
{
    bool escaped = false;

    for (const char *p = json; *p != '\0'; ++p)
    {
        if (escaped)
        {
            if (*p == 'u' && p[1] == '0' && p[2] == '0' && p[3] == '0' &&
                p[4] == '0')
            {
                return true;
            }
            escaped = false;
            continue;
        }
        if (*p == '\\')
        {
            escaped = true;
        }
    }
    return false;
}

/* --------------------------------------------------------------------- */
/* Manufacturing-record gate                                               */
/* --------------------------------------------------------------------- */

static device_identity_status_t device_identity_map_app_config(
    app_config_status_t status)
{
    switch (status)
    {
        case APP_CONFIG_OK:
        case APP_CONFIG_OK_RECOVERED:
            return DEVICE_IDENTITY_OK;
        case APP_CONFIG_ERR_INVALID_ARGUMENT:
            return DEVICE_IDENTITY_ERR_INVALID_ARGUMENT;
        case APP_CONFIG_ERR_IO:
        case APP_CONFIG_ERR_UNSUPPORTED:
            return DEVICE_IDENTITY_ERR_IO;
        case APP_CONFIG_ERR_NOT_FOUND:
            /* A device without a manufacturing record is simply not
             * provisioned; identity is unavailable, not corrupted. */
            return DEVICE_IDENTITY_ERR_UNPROVISIONED;
        case APP_CONFIG_ERR_MALFORMED:
        case APP_CONFIG_ERR_MIGRATION:
            return DEVICE_IDENTITY_ERR_MALFORMED;
        case APP_CONFIG_ERR_BOUNDS:
            return DEVICE_IDENTITY_ERR_BOUNDS;
        case APP_CONFIG_ERR_UNKNOWN_SCHEMA:
            return DEVICE_IDENTITY_ERR_UNKNOWN_SCHEMA;
        default:
            return DEVICE_IDENTITY_ERR_MALFORMED;
    }
}

/**
 * @brief Validate the enrolled manufacturing record.
 *
 * Only a provisioned device with an enrolled credential mode may use an
 * access token; anything else is refused before the secret record is even
 * touched (an unprovisioned device must not read credentials).
 */
static device_identity_status_t device_identity_check_manufacturing(void)
{
    app_config_manufacturing_doc_t manufacturing;
    app_config_status_t status = app_config_load_manufacturing(&manufacturing);
    device_identity_status_t mapped;

    if (status == APP_CONFIG_OK || status == APP_CONFIG_OK_RECOVERED)
    {
        if (manufacturing.manufacturing_state !=
            APP_CONFIG_MFG_STATE_PROVISIONED)
        {
            return DEVICE_IDENTITY_ERR_UNPROVISIONED;
        }
        if (manufacturing.credential_mode == APP_CONFIG_CRED_MODE_NONE)
        {
            return DEVICE_IDENTITY_ERR_UNPROVISIONED;
        }
        return DEVICE_IDENTITY_OK;
    }

    mapped = device_identity_map_app_config(status);
    return (mapped == DEVICE_IDENTITY_OK) ? DEVICE_IDENTITY_ERR_UNPROVISIONED
                                          : mapped;
}

/* --------------------------------------------------------------------- */
/* Record loading                                                         */
/* --------------------------------------------------------------------- */

/**
 * @brief Read the whole identity record, enforcing the size bound.
 *
 * The returned buffer is heap-allocated and contains the raw record text
 * (including the token); the caller must zeroize and free it.
 *
 * @return The NUL-terminated buffer, or NULL on failure (missing, empty,
 *         oversized or unreadable file).
 */
static char *device_identity_read_record(void)
{
    osal_fstat_t st = { 0 };
    char *buffer = NULL;

    if (osal_stat(DEVICE_IDENTITY_FILE_PATH, &st) != OSAL_SUCCESS)
    {
        return NULL;
    }

    if (st.file_size == 0 || (size_t)st.file_size >=
                                 DEVICE_IDENTITY_MAX_FILE_BYTES)
    {
        return NULL;
    }

    osal_file_id_t fd = osal_open_create(
        DEVICE_IDENTITY_FILE_PATH, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    if (fd < 0)
    {
        return NULL;
    }

    buffer = (char *)malloc((size_t)st.file_size + 1u);
    if (buffer != NULL)
    {
        int32_t n = osal_read(fd, buffer, (size_t)st.file_size);
        if (n >= 0 && (size_t)n == (size_t)st.file_size)
        {
            buffer[n] = '\0';
        }
        else
        {
            device_identity_zerobuf(buffer, (size_t)st.file_size + 1u);
            free(buffer);
            buffer = NULL;
        }
    }

    (void)osal_close(fd);
    return buffer;
}

/**
 * @brief Zeroize the parser's value copy of the token before the tree is
 *        deleted, so the secret does not outlive the parse in heap memory.
 */
static void device_identity_wipe_token_node(const cJSON *root)
{
    const cJSON *token_item = cJSON_GetObjectItemCaseSensitive(
        root, "access_token");

    if (token_item != NULL && token_item->valuestring != NULL)
    {
        device_identity_zerobuf(token_item->valuestring,
                                strlen(token_item->valuestring) + 1u);
    }
}

/**
 * @brief Parse and validate the strict v1 identity record.
 *
 * On success copies the validated values into the module-owned buffers and
 * zeroizes every temporary secret-bearing copy.  The caller owns @p root
 * and the raw @p json buffer and must free them after this returns (the
 * token node already has its value wiped).
 */
static device_identity_status_t device_identity_parse_record(
    const cJSON *root, const char *json)
{
    static const char *const DEVICE_IDENTITY_KEYS[] = {
        "schema_version",
        "client_id",
        "access_token",
    };
#define DEVICE_IDENTITY_KEY_COUNT \
    (sizeof(DEVICE_IDENTITY_KEYS) / sizeof(DEVICE_IDENTITY_KEYS[0]))

    const cJSON *item;

    if (!device_identity_check_member_keys(root, DEVICE_IDENTITY_KEYS,
                                           DEVICE_IDENTITY_KEY_COUNT))
    {
        return DEVICE_IDENTITY_ERR_MALFORMED;
    }
    if (device_identity_raw_json_has_nul_escape(json))
    {
        return DEVICE_IDENTITY_ERR_MALFORMED;
    }

    /* schema_version: exactly the current version. */
    item = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(item))
    {
        return DEVICE_IDENTITY_ERR_MALFORMED;
    }
    if (item->valuedouble > (double)DEVICE_IDENTITY_SCHEMA_VERSION)
    {
        return DEVICE_IDENTITY_ERR_UNKNOWN_SCHEMA;
    }
    if (item->valuedouble != (double)DEVICE_IDENTITY_SCHEMA_VERSION)
    {
        /* Older schema versions have no migration path in release 1; an
         * identity record must be re-issued by the manufacturing flow. */
        return DEVICE_IDENTITY_ERR_MALFORMED;
    }

    /* client_id: bounded, printable, non-empty (stable client ID). */
    item = cJSON_GetObjectItemCaseSensitive(root, "client_id");
    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return DEVICE_IDENTITY_ERR_MALFORMED;
    }
    if (strlen(item->valuestring) > DEVICE_IDENTITY_CLIENT_ID_MAX_LEN)
    {
        return DEVICE_IDENTITY_ERR_BOUNDS;
    }
    if (item->valuestring[0] == '\0')
    {
        return DEVICE_IDENTITY_ERR_EMPTY_CLIENT_ID;
    }
    if (!device_identity_is_printable(item->valuestring,
                                      DEVICE_IDENTITY_CLIENT_ID_MAX_LEN,
                                      false))
    {
        return DEVICE_IDENTITY_ERR_MALFORMED;
    }

    /* access_token: bounded, printable-without-whitespace, non-empty. */
    item = cJSON_GetObjectItemCaseSensitive(root, "access_token");
    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return DEVICE_IDENTITY_ERR_MALFORMED;
    }
    if (strlen(item->valuestring) > DEVICE_IDENTITY_TOKEN_MAX_LEN)
    {
        return DEVICE_IDENTITY_ERR_BOUNDS;
    }
    if (item->valuestring[0] == '\0')
    {
        return DEVICE_IDENTITY_ERR_EMPTY_TOKEN;
    }
    if (!device_identity_is_printable(item->valuestring,
                                      DEVICE_IDENTITY_TOKEN_MAX_LEN, true))
    {
        return DEVICE_IDENTITY_ERR_MALFORMED;
    }

    /* Publish only validated values.  The token copy below is the single
     * module-owned copy that outlives the load. */
    strncpy(s_client_id, cJSON_GetObjectItemCaseSensitive(root,
                                                          "client_id")->valuestring,
            sizeof(s_client_id) - 1u);
    s_client_id[sizeof(s_client_id) - 1u] = '\0';
    strncpy(s_token, item->valuestring, sizeof(s_token) - 1u);
    s_token[sizeof(s_token) - 1u] = '\0';

    /* Wipe the parser's value copy so no second token copy survives. */
    device_identity_wipe_token_node(root);

    s_loaded = true;
    return DEVICE_IDENTITY_OK;
}

/* --------------------------------------------------------------------- */
/* Fail-off contract                                                      */
/* --------------------------------------------------------------------- */

static void device_identity_fail_off(void)
{
    lamp_status_t status = lamp_control_force_inactive();
    if (status != LAMP_OK)
    {
        /* The fail-off barrier is a safety mechanism; a failed barrier
         * latching is logged by the caller (this module never logs). */
        (void)status;
    }
}

/* --------------------------------------------------------------------- */
/* Public lifecycle                                                       */
/* --------------------------------------------------------------------- */

device_identity_status_t device_identity_load(void)
{
    char *json;
    cJSON *root = NULL;
    device_identity_status_t status;

    /* Re-load replaces the previous state atomically from the caller's
     * perspective: the module buffers are only written after full
     * validation.  Invalidate first so a failure never leaves a stale
     * identity armed (fail closed). */
    device_identity_invalidate();

    status = device_identity_check_manufacturing();
    if (status != DEVICE_IDENTITY_OK)
    {
        device_identity_fail_off();
        return status;
    }

    json = device_identity_read_record();
    if (json == NULL)
    {
        /* Distinguish a missing record from unreadable/oversized data. */
        osal_fstat_t st = { 0 };
        status = (osal_stat(DEVICE_IDENTITY_FILE_PATH, &st) == OSAL_SUCCESS)
                     ? DEVICE_IDENTITY_ERR_BOUNDS
                     : DEVICE_IDENTITY_ERR_NOT_FOUND;
        device_identity_fail_off();
        return status;
    }

    /* Strict parse: the whole input must be consumed; trailing garbage or a
     * second concatenated object is rejected as malformed. */
    root = cJSON_ParseWithLengthOpts(json, strlen(json) + 1u, NULL, 1);
    if (root == NULL || !cJSON_IsObject(root))
    {
        status = DEVICE_IDENTITY_ERR_MALFORMED;
    }
    else
    {
        status = device_identity_parse_record(root, json);
        if (status != DEVICE_IDENTITY_OK)
        {
            /* Kill any remaining token copy held by the parser before it is
             * freed (counters a parse failure after the token node was
             * created, e.g. a bounds reject on a later field). */
            device_identity_wipe_token_node(root);
        }
    }

    if (root != NULL)
    {
        cJSON_Delete(root);
    }

    /* The raw record text (which contains the token) must not outlive the
     * load: zeroize and free it before returning. */
    device_identity_zerobuf(json, strlen(json) + 1u);
    free(json);

    if (status != DEVICE_IDENTITY_OK)
    {
        device_identity_invalidate();
        device_identity_fail_off();
    }
    return status;
}

bool device_identity_is_loaded(void)
{
    return s_loaded;
}

void device_identity_clear(void)
{
    device_identity_invalidate();
}

/* --------------------------------------------------------------------- */
/* Stable client-ID and token access                                      */
/* --------------------------------------------------------------------- */

device_identity_status_t device_identity_client_id(const char **out_client_id)
{
    if (out_client_id == NULL)
    {
        return DEVICE_IDENTITY_ERR_INVALID_ARGUMENT;
    }
    if (!s_loaded)
    {
        *out_client_id = NULL;
        return DEVICE_IDENTITY_ERR_NOT_LOADED;
    }
    *out_client_id = s_client_id;
    return DEVICE_IDENTITY_OK;
}

device_identity_status_t device_identity_token(const char **out_token)
{
    if (out_token == NULL)
    {
        return DEVICE_IDENTITY_ERR_INVALID_ARGUMENT;
    }
    if (!s_loaded)
    {
        *out_token = NULL;
        return DEVICE_IDENTITY_ERR_NOT_LOADED;
    }
    *out_token = s_token;
    return DEVICE_IDENTITY_OK;
}

/**
 * @brief Token-provider callback implementation.
 *
 * Returns the module-owned token while a validated identity is loaded and
 * NULL otherwise — a consumer invoking the provider always receives the
 * validated value or a clear "not available", never a fallback.
 */
static const char *device_identity_provide_token(void)
{
    return s_loaded ? s_token : NULL;
}

device_identity_token_provider_t device_identity_token_provider(void)
{
    return device_identity_provide_token;
}