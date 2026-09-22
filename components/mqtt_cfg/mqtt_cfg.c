/**
 * @file mqtt_cfg.c
 * @brief MQTT/TLS broker configuration implementation (TASK-110)
 *
 * See mqtt_cfg.h for the normative contract.  Implementation notes:
 *
 *   - the document (/config/mqtt.json) is read with the portable OSAL file
 *     API, bounded at file and field level, parsed with a strict cJSON
 *     schema (unknown and duplicate members rejected) and then validated
 *     against the verified-TLS-only policy,
 *   - the transport is configured exclusively through the existing
 *     Mongoose configuration API (mqtt_config.h): address, SSL, skip-verify
 *     and certificate sources.  mqtt_config resolves the logical /cert
 *     paths through the OSAL backend onto the LittleFS mount, so the
 *     component never touches sockets or the TLS stack directly,
 *   - every configuration/application/connection failure forces the lamp
 *     output inactive (fail-off) before the error is returned.
 */

#include "mqtt_cfg.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_identity.h"
#include "lamp_control.h"
#include "mqtt_app.h"
#include "mqtt_config.h"
#include "osal_error.h"
#include "osal_file.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_task.h"

/* --------------------------------------------------------------------- */
/* Internal constants                                                     */
/* --------------------------------------------------------------------- */

/** @brief Poll interval [ms] while waiting for the verified TLS connect. */
#define MQTT_CFG_CONNECT_POLL_MS 25u

/** @brief Format of the applied broker address (verified TLS only). */
#define MQTT_CFG_ADDRESS_FMT "mqtts://%s:%u"

/** @brief Verified-TLS address scheme prefix (the only accepted scheme). */
#define MQTT_CFG_MQTTS_SCHEME "mqtts://"

/**
 * @brief True only after mqtt_cfg_apply() completed every setter and
 *        self-check successfully.
 *
 * Cleared before every apply attempt and on every failure; set only on the
 * all-success path.  mqtt_cfg_connect() requires it, so neither a partial
 * apply nor an externally manufactured mqtt_config state can start the
 * verified transport.
 */
static bool s_verified_applied = false;

/**
 * @brief Immutable-in-module snapshot of the configuration that was applied.
 *
 * The public mqtt_config API is intentionally mutable because other product
 * users need it.  Consequently the latch alone is not sufficient: a caller
 * can mutate the Mongoose values after apply().  The snapshot is copied only
 * after all setters and self-checks succeed and is compared immediately before
 * every connect attempt.
 */
static mqtt_cfg_t s_applied_cfg;

/** @brief Material captured from the LittleFS-backed config at apply time. */
typedef struct {
    size_t length;
    char bytes[MQTT_CERT_MAX_SIZE];
} mqtt_cfg_cert_snapshot_t;

static mqtt_cfg_cert_snapshot_t s_applied_ca;
static mqtt_cfg_cert_snapshot_t s_applied_client_cert;
static mqtt_cfg_cert_snapshot_t s_applied_client_key;

/**
 * @brief Module-owned lock serializing the verified-transport lifecycle.
 *
 * Guards the verified-apply latch, the applied snapshot and every verified
 * transport check against the values the transport consumes (TASK-110
 * finding 2): apply() publishes the verified values under this lock and
 * connect() performs its entry check and its success-time re-check under
 * the same lock, so a concurrent mqtt_cfg apply/connect can never observe
 * or consume a half-written configuration.  The lock is never held across
 * the blocking mqtt_app_deinit()/mqtt_app_init() calls, and the Mongoose
 * safety callbacks plus the config-validation gate only touch the atomics
 * below (never this lock), so no deadlock can form with the transport
 * thread.
 *
 * Created lazily on first use (publish-once pattern); never deleted, so a
 * published handle stays valid for the process lifetime.
 */
static _Atomic(osal_mutex_id_t) s_lock;

/**
 * @brief Apply/connection generations prevent an old session from satisfying
 *        a newly applied configuration.
 *
 * Written by the apply/connect paths and read (and written) by the Mongoose
 * safety callbacks running on the poll thread, so all four fields are C11
 * atomics with acquire/release ordering: a connect or failure event
 * published on the transport thread is never lost by the connect poll loop
 * (TASK-110 finding 3).  The wait/generation state is established under the
 * module lock BEFORE mqtt_app_init() starts the asynchronous connect;
 * callbacks publish with release ordering and the poll loop consumes with
 * acquire ordering.
 */
static atomic_uint s_apply_generation;
static atomic_uint s_connected_generation;
static atomic_bool s_waiting_for_connection;
static atomic_bool s_connect_failed;

/* --------------------------------------------------------------------- */
/* Local helpers                                                          */
/* --------------------------------------------------------------------- */

/**
 * @brief Lazily publish the module lock (created once, never deleted).
 *
 * @return true when a lock handle is available (created or already
 *         published); false only when creation itself failed.
 */
static bool mqtt_cfg_lock_ensure(void)
{
    osal_mutex_id_t lock =
        atomic_load_explicit(&s_lock, memory_order_acquire);
    if (lock != NULL)
    {
        return true;
    }

    osal_mutex_id_t created = NULL;
    if (osal_mutex_create(&created, "mqtt_cfg") != OSAL_SUCCESS)
    {
        return false;
    }

    osal_mutex_id_t expected = NULL;
    if (atomic_compare_exchange_strong_explicit(
            &s_lock, &expected, created, memory_order_release,
            memory_order_acquire))
    {
        /* This thread adopted its mutex; it was published exactly once. */
        return true;
    }

    /* Another thread won the creation race; drop the duplicate. */
    (void)osal_mutex_delete(created);
    return true;
}

/** @brief Take the module lock (blocking; no-op-safe when unavailable). */
static void mqtt_cfg_lock(void)
{
    if (mqtt_cfg_lock_ensure())
    {
        (void)osal_mutex_take(s_lock);
    }
}

/** @brief Release the module lock taken by mqtt_cfg_lock(). */
static void mqtt_cfg_unlock(void)
{
    if (atomic_load_explicit(&s_lock, memory_order_acquire) != NULL)
    {
        (void)osal_mutex_give(s_lock);
    }
}

/**
 * @brief Force the lamp output inactive.
 *
 * The contract is idempotent: repeated fail-off calls are safe while the
 * output is already off.
 */
static void mqtt_cfg_fail_off(void)
{
    lamp_status_t status = lamp_control_force_inactive();
    if (status != LAMP_OK)
    {
        osal_log_error("mqtt_cfg: fail-off could not force output off: %d",
                       (int)status);
    }
}

/**
 * @brief Read a whole file into a NUL-terminated heap buffer.
 *
 * @param[in]  path    Logical OSAL path.
 * @param[in]  max_len Upper bound for the file size (exclusive).
 * @param[out] out_len Received byte count (excluding the terminator), may
 *                     be NULL.
 *
 * @return Pointer to the buffer (caller frees) or NULL on failure.
 */
static char *mqtt_cfg_read_file(const char *path, size_t max_len,
                                size_t *out_len)
{
    osal_fstat_t st = { 0 };
    char *buffer = NULL;
    size_t len = 0u;

    if (out_len != NULL)
    {
        *out_len = 0u;
    }

    if (osal_stat(path, &st) != OSAL_SUCCESS)
    {
        return NULL;
    }

    if (st.file_size == 0 || (size_t)st.file_size >= max_len)
    {
        return NULL;
    }

    osal_file_id_t fd = osal_open_create(
        path, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
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
            len = (size_t)n;
        }
        else
        {
            free(buffer);
            buffer = NULL;
        }
    }

    (void)osal_close(fd);
    if (out_len != NULL)
    {
        *out_len = len;
    }
    return buffer;
}

/**
 * @brief Reject unknown members, unnamed members and duplicate keys.
 *
 * Together with the "every required member must be present" rule in the
 * parser this forces the object to contain exactly the schema's key set.
 */
static bool mqtt_cfg_check_member_keys(const cJSON *object,
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
         * each key against every later member (the document size is bounded,
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
static bool mqtt_cfg_raw_json_has_nul_escape(const char *json)
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

/**
 * @brief Validate a single string field against a printable-ASCII charset.
 *
 * Bounded-terminator rule (TASK-110 finding 1): the caller's fixed-size
 * arrays must be NUL-terminated within their capacity before any unbounded
 * string operation.  strnlen() therefore never scans past
 * max_len + 1 bytes (the array size including the terminator slot); a
 * missing terminator yields a length above max_len and is rejected as an
 * invalid argument instead of causing an out-of-bounds read.
 */
static bool mqtt_cfg_is_printable(const char *str, size_t max_len)
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
    }
    return true;
}

/**
 * @brief Check that a logical certificate path stays under /cert.
 *
 * Rules: must start with "/cert/" (a bare "/cert" is a directory, not a
 * certificate file), must not contain ".." (no traversal), and must not
 * carry the LittleFS mount-point prefix (logical paths only — the OSAL
 * backend resolves them onto the mount consistently).
 */
static bool mqtt_cfg_path_under_cert(const char *path)
{
    const char *prefix = MQTT_CFG_CERT_DIR "/";
    size_t prefix_len = strlen(prefix);

    if (path == NULL || path[0] == '\0')
    {
        return false;
    }
    /* Bounded-terminator rule (TASK-110 finding 1): the fixed-size path
     * array must be NUL-terminated within its capacity before the unbounded
     * strstr() scan below can run. */
    if (strnlen(path, MQTT_CFG_PATH_MAX_LEN + 1u) > MQTT_CFG_PATH_MAX_LEN)
    {
        return false;
    }
    if (strncmp(path, prefix, prefix_len) != 0)
    {
        return false;
    }
    if (path[prefix_len] == '\0')
    {
        return false; /* "/cert/" alone names no file */
    }
    if (strstr(path, "..") != NULL)
    {
        return false;
    }
    return true;
}

static bool mqtt_cfg_is_alnum(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9');
}

static bool mqtt_cfg_is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

/**
 * @brief Validate a broker hostname.
 *
 * Accepts DNS-shaped names only: labels of letters/digits/'_'/'-' that are
 * separated by single dots, never start or end with a dot, and whose first
 * and last character are alphanumeric ('-'/'_' may only appear inside a
 * label).  IP literals — IPv4 dotted quads (all-digit labels) and any
 * ':'-containing IPv6 text — are rejected: the endpoint is a stable DNS
 * name, never a changing raw IP.
 */
static bool mqtt_cfg_hostname_valid(const char *hostname)
{
    size_t len;
    bool last_was_dot = false;
    bool label_start = true;
    bool label_has_alnum = false;
    bool ipv4_literal = true;
    char prev = '\0';

    if (!mqtt_cfg_is_printable(hostname, MQTT_CFG_HOSTNAME_MAX_LEN))
    {
        return false;
    }

    len = strlen(hostname);
    if (len == 0u || hostname[0] == '.' || hostname[len - 1u] == '.')
    {
        return false;
    }
    if (strchr(hostname, ':') != NULL)
    {
        return false; /* IPv6 literal */
    }

    for (size_t i = 0u; i < len; ++i)
    {
        char ch = hostname[i];
        bool alnum = mqtt_cfg_is_alnum(ch);

        if (ch == '.')
        {
            if (last_was_dot || !label_has_alnum || !mqtt_cfg_is_alnum(prev))
            {
                return false; /* empty label or label ending with '-/_' */
            }
            last_was_dot = true;
            label_start = true;
            label_has_alnum = false;
            prev = ch;
            continue;
        }

        bool ok_char = alnum || ch == '-' || ch == '_';
        if (!ok_char)
        {
            return false;
        }
        if (!mqtt_cfg_is_digit(ch))
        {
            /* Any non-digit (letter, '-' or '_') in a label makes the
             * name a DNS name, not an IPv4 dotted quad. */
            ipv4_literal = false;
        }
        if (label_start && !alnum)
        {
            return false; /* a label must start with an alphanumeric */
        }
        label_start = false;
        label_has_alnum = true;
        last_was_dot = false;
        prev = ch;
    }

    if (last_was_dot || !label_has_alnum || !mqtt_cfg_is_alnum(prev))
    {
        return false; /* trailing dot, empty/'-'-led last label */
    }
    if (ipv4_literal)
    {
        return false; /* a changing raw IP is never the endpoint */
    }
    return true;
}

/* --------------------------------------------------------------------- */
/* Status names                                                           */
/* --------------------------------------------------------------------- */

const char *mqtt_cfg_status_name(mqtt_cfg_status_t status)
{
    switch (status)
    {
        case MQTT_CFG_OK:                   return "ok";
        case MQTT_CFG_ERR_INVALID_ARGUMENT: return "invalid_argument";
        case MQTT_CFG_ERR_IO:               return "io";
        case MQTT_CFG_ERR_NOT_FOUND:        return "not_found";
        case MQTT_CFG_ERR_MALFORMED:        return "malformed";
        case MQTT_CFG_ERR_BOUNDS:           return "bounds";
        case MQTT_CFG_ERR_UNKNOWN_SCHEMA:   return "unknown_schema";
        case MQTT_CFG_ERR_PLAINTEXT:        return "plaintext_rejected";
        case MQTT_CFG_ERR_SKIP_VERIFY:      return "skip_verify_rejected";
        case MQTT_CFG_ERR_CA_PATH:          return "ca_path_invalid";
        case MQTT_CFG_ERR_CERT_PATH:        return "cert_path_invalid";
        case MQTT_CFG_ERR_HOSTNAME:         return "hostname_invalid";
        case MQTT_CFG_ERR_APPLY:            return "apply_failed";
        case MQTT_CFG_ERR_CONNECT:          return "connect_failed";
        case MQTT_CFG_ERR_NOT_APPLIED:      return "not_applied";
        default:                            return "unknown";
    }
}

/* --------------------------------------------------------------------- */
/* Parsing (strict v1 schema)                                             */
/* --------------------------------------------------------------------- */

static const char *const MQTT_CFG_KEYS[] = {
    "schema_version", "hostname", "port", "tls_mode", "ca_path",
    "client_cert_path", "client_key_path", "client_id", "auth_mode",
    "skip_verify",
};

#define MQTT_CFG_KEY_COUNT \
    (sizeof(MQTT_CFG_KEYS) / sizeof(MQTT_CFG_KEYS[0]))

static mqtt_cfg_status_t mqtt_cfg_get_string_member(
    const cJSON *root, const char *key, char *out, size_t out_cap)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    out[0] = '\0';
    if (item == NULL)
    {
        return MQTT_CFG_OK; /* optional member absent */
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return MQTT_CFG_ERR_MALFORMED;
    }
    if (strlen(item->valuestring) >= out_cap)
    {
        return MQTT_CFG_ERR_BOUNDS;
    }
    strncpy(out, item->valuestring, out_cap - 1u);
    out[out_cap - 1u] = '\0';
    return MQTT_CFG_OK;
}

static mqtt_cfg_status_t mqtt_cfg_parse_root(const cJSON *root,
                                             mqtt_cfg_t *out)
{
    const cJSON *item;
    double port = 0.0;

    memset(out, 0, sizeof(*out));

    if (!mqtt_cfg_check_member_keys(root, MQTT_CFG_KEYS, MQTT_CFG_KEY_COUNT))
    {
        return MQTT_CFG_ERR_MALFORMED;
    }

    /* schema_version */
    item = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(item) || item->valuedouble != MQTT_CFG_SCHEMA_VERSION)
    {
        if (cJSON_IsNumber(item) &&
            item->valuedouble > MQTT_CFG_SCHEMA_VERSION)
        {
            return MQTT_CFG_ERR_UNKNOWN_SCHEMA;
        }
        return MQTT_CFG_ERR_MALFORMED;
    }
    out->schema_version = MQTT_CFG_SCHEMA_VERSION;

    /* hostname */
    mqtt_cfg_status_t status = mqtt_cfg_get_string_member(
        root, "hostname", out->hostname, sizeof(out->hostname));
    if (status != MQTT_CFG_OK)
    {
        return status;
    }
    if (!mqtt_cfg_hostname_valid(out->hostname))
    {
        return MQTT_CFG_ERR_HOSTNAME;
    }

    /* port */
    item = cJSON_GetObjectItemCaseSensitive(root, "port");
    if (!cJSON_IsNumber(item))
    {
        return MQTT_CFG_ERR_MALFORMED;
    }
    port = item->valuedouble;
    if (port < (double)MQTT_CFG_PORT_MIN || port > (double)MQTT_CFG_PORT_MAX ||
        port != (double)(uint32_t)port)
    {
        return MQTT_CFG_ERR_BOUNDS;
    }
    out->port = (uint16_t)port;

    /* client_id */
    status = mqtt_cfg_get_string_member(
        root, "client_id", out->client_id, sizeof(out->client_id));
    if (status != MQTT_CFG_OK)
    {
        return status;
    }
    if (!mqtt_cfg_is_printable(out->client_id, MQTT_CFG_CLIENT_ID_MAX_LEN))
    {
        return MQTT_CFG_ERR_BOUNDS;
    }

    /* tls_mode — only "mqtts" is representable/accepted */
    item = cJSON_GetObjectItemCaseSensitive(root, "tls_mode");
    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return MQTT_CFG_ERR_MALFORMED;
    }
    if (strcmp(item->valuestring, "mqtts") != 0)
    {
        return MQTT_CFG_ERR_PLAINTEXT;
    }
    out->tls_mode = MQTT_CFG_TLS_MODE_MQTTS;

    /* skip_verify: absent or false only */
    item = cJSON_GetObjectItemCaseSensitive(root, "skip_verify");
    if (item != NULL)
    {
        if (!cJSON_IsBool(item))
        {
            return MQTT_CFG_ERR_MALFORMED;
        }
        if (cJSON_IsTrue(item))
        {
            return MQTT_CFG_ERR_SKIP_VERIFY;
        }
    }

    /* ca_path */
    status = mqtt_cfg_get_string_member(
        root, "ca_path", out->ca_path, sizeof(out->ca_path));
    if (status != MQTT_CFG_OK)
    {
        return status;
    }
    if (!mqtt_cfg_path_under_cert(out->ca_path))
    {
        return MQTT_CFG_ERR_CA_PATH;
    }

    /* optional mTLS paths */
    status = mqtt_cfg_get_string_member(
        root, "client_cert_path", out->client_cert_path,
        sizeof(out->client_cert_path));
    if (status != MQTT_CFG_OK)
    {
        return status;
    }
    status = mqtt_cfg_get_string_member(
        root, "client_key_path", out->client_key_path,
        sizeof(out->client_key_path));
    if (status != MQTT_CFG_OK)
    {
        return status;
    }

    /* auth_mode */
    item = cJSON_GetObjectItemCaseSensitive(root, "auth_mode");
    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return MQTT_CFG_ERR_MALFORMED;
    }
    if (strcmp(item->valuestring, "none") == 0)
    {
        out->auth_mode = MQTT_CFG_AUTH_NONE;
    }
    else if (strcmp(item->valuestring, "access_token") == 0)
    {
        out->auth_mode = MQTT_CFG_AUTH_ACCESS_TOKEN;
    }
    else if (strcmp(item->valuestring, "mtls") == 0)
    {
        out->auth_mode = MQTT_CFG_AUTH_MTLS;
    }
    else
    {
        return MQTT_CFG_ERR_MALFORMED;
    }

    return MQTT_CFG_OK;
}

/* --------------------------------------------------------------------- */
/* Validation                                                             */
/* --------------------------------------------------------------------- */

static mqtt_cfg_status_t mqtt_cfg_validate_fields(const mqtt_cfg_t *cfg)
{
    if (cfg == NULL)
    {
        return MQTT_CFG_ERR_INVALID_ARGUMENT;
    }

    if (cfg->schema_version != MQTT_CFG_SCHEMA_VERSION)
    {
        return MQTT_CFG_ERR_UNKNOWN_SCHEMA;
    }
    if (!mqtt_cfg_hostname_valid(cfg->hostname))
    {
        return MQTT_CFG_ERR_HOSTNAME;
    }
    /* The field is uint16_t, so the upper bound (MQTT_CFG_PORT_MAX == 65535)
     * is guaranteed by the type itself; only the lower bound can be
     * violated by a hand-built struct.  The parse path enforces both. */
    if (cfg->port < MQTT_CFG_PORT_MIN)
    {
        return MQTT_CFG_ERR_BOUNDS;
    }
    if (cfg->tls_mode != MQTT_CFG_TLS_MODE_MQTTS)
    {
        return MQTT_CFG_ERR_PLAINTEXT;
    }
    if (!mqtt_cfg_is_printable(cfg->client_id, MQTT_CFG_CLIENT_ID_MAX_LEN))
    {
        return MQTT_CFG_ERR_BOUNDS;
    }
    if (cfg->auth_mode != MQTT_CFG_AUTH_NONE &&
        cfg->auth_mode != MQTT_CFG_AUTH_ACCESS_TOKEN &&
        cfg->auth_mode != MQTT_CFG_AUTH_MTLS)
    {
        /* A hand-built struct with an unknown authentication enum is a
         * malformed configuration (the parse path already maps unknown
         * auth_mode strings to the same error).  Checked before the
         * mTLS-specific rules so an invalid mode can never be treated as a
         * non-mTLS configuration. */
        return MQTT_CFG_ERR_MALFORMED;
    }
    if (!mqtt_cfg_path_under_cert(cfg->ca_path))
    {
        return MQTT_CFG_ERR_CA_PATH;
    }

    bool cert_path_present = cfg->client_cert_path[0] != '\0';
    bool key_path_present = cfg->client_key_path[0] != '\0';

    if (cfg->auth_mode == MQTT_CFG_AUTH_MTLS)
    {
        if (!cert_path_present || !key_path_present)
        {
            return MQTT_CFG_ERR_CERT_PATH;
        }
    }
    if (cert_path_present &&
        !mqtt_cfg_path_under_cert(cfg->client_cert_path))
    {
        return MQTT_CFG_ERR_CERT_PATH;
    }
    if (key_path_present && !mqtt_cfg_path_under_cert(cfg->client_key_path))
    {
        return MQTT_CFG_ERR_CERT_PATH;
    }

    return MQTT_CFG_OK;
}

/**
 * @brief Verify that the mutable mqtt_config state is still the applied state.
 *
 * mqtt_config exposes setters for the whole product, so an applied latch by
 * itself cannot protect the transport.  This check deliberately compares all
 * transport/security fields against the accepted snapshot, including the
 * logical certificate paths and resolved certificate material.  A mismatch
 * invalidates the latch at the caller and requires a fresh validated apply.
 */
static bool mqtt_cfg_applied_cert_matches(
    mqtt_config_value_t key, const char *expected_path, bool required,
    const mqtt_cfg_cert_snapshot_t *expected_material)
{
    mqtt_cert_source_t source = MQTT_CERT_SOURCE_NONE;
    const char *value = NULL;
    const char *resolved;
    size_t resolved_len;

    if (!mqtt_config_get_cert_source(&source, &value, key))
    {
        return false;
    }

    if (!required)
    {
        return source == MQTT_CERT_SOURCE_NONE &&
               (value == NULL || value[0] == '\0');
    }

    resolved = mqtt_config_get_cert(key);
    if (source != MQTT_CERT_SOURCE_FILE_PATH || value == NULL ||
        !mqtt_cfg_path_under_cert(value) || expected_path == NULL ||
        strcmp(value, expected_path) != 0 || resolved == NULL ||
        expected_material == NULL || resolved[0] == '\0')
    {
        return false;
    }

    /* mqtt_config exposes the resolved LittleFS material, not merely its
     * logical path.  Compare the complete bounded buffer captured after the
     * successful apply so replacing a file at the same path cannot bypass
     * the verified-apply gate. */
    resolved_len = strlen(resolved);
    return resolved_len == expected_material->length &&
           memcmp(resolved, expected_material->bytes, resolved_len) == 0;
}

static bool mqtt_cfg_capture_cert(mqtt_config_value_t key,
                                  mqtt_cfg_cert_snapshot_t *snapshot)
{
    const char *resolved;
    size_t length;

    if (snapshot == NULL)
    {
        return false;
    }
    resolved = mqtt_config_get_cert(key);
    if (resolved == NULL || resolved[0] == '\0')
    {
        return false;
    }
    length = strlen(resolved);
    if (length >= sizeof(snapshot->bytes))
    {
        return false;
    }
    memcpy(snapshot->bytes, resolved, length + 1u);
    snapshot->length = length;
    return true;
}

static bool mqtt_cfg_applied_transport_matches(void)
{
    char expected_address[MQTT_CONFIG_STR_SIZE];
    const char *address;
    const char *client_id;
    bool ssl = false;
    bool skip_verify = true;
    int expected_len = -1;

    if (mqtt_cfg_validate_fields(&s_applied_cfg) != MQTT_CFG_OK)
    {
        return false;
    }
    /* The live address is stored by the platform in an MQTT_CONFIG_STR_SIZE
     * buffer, so format the expected address into a buffer of exactly that
     * bound; a formatted address that would not round-trip through the
     * platform storage can never match and is treated as a mismatch. */
    expected_len = snprintf(expected_address, sizeof(expected_address),
                            MQTT_CFG_ADDRESS_FMT, s_applied_cfg.hostname,
                            (unsigned)s_applied_cfg.port);
    if (expected_len < 0 || (size_t)expected_len >= sizeof(expected_address))
    {
        return false;
    }

    address = mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
    client_id = mqtt_config_get_string(MQTT_CONFIG_VALUE_CLIENT_ID);
    if (address == NULL || strcmp(address, expected_address) != 0 ||
        client_id == NULL || strcmp(client_id, s_applied_cfg.client_id) != 0 ||
        !mqtt_config_get_bool(&ssl, MQTT_CONFIG_VALUE_SSL) || !ssl ||
        !mqtt_config_get_bool(&skip_verify, MQTT_CONFIG_VALUE_SKIP_VERIFY) ||
        skip_verify)
    {
        return false;
    }

    if (!mqtt_cfg_applied_cert_matches(MQTT_CONFIG_VALUE_CERT,
                                       s_applied_cfg.ca_path, true,
                                       &s_applied_ca))
    {
        return false;
    }

    if (s_applied_cfg.auth_mode == MQTT_CFG_AUTH_MTLS)
    {
        return mqtt_cfg_applied_cert_matches(
                   MQTT_CONFIG_VALUE_CLIENT_CERT,
                   s_applied_cfg.client_cert_path, true,
                   &s_applied_client_cert) &&
               mqtt_cfg_applied_cert_matches(
                   MQTT_CONFIG_VALUE_CLIENT_KEY,
                   s_applied_cfg.client_key_path, true,
                   &s_applied_client_key);
    }

    return mqtt_cfg_applied_cert_matches(MQTT_CONFIG_VALUE_CLIENT_CERT,
                                         NULL, false, NULL) &&
           mqtt_cfg_applied_cert_matches(MQTT_CONFIG_VALUE_CLIENT_KEY,
                                         NULL, false, NULL);
}

/**
 * @brief Verified-owner validation gate for the generic apply-config path
 *        (TASK-121 platform gate; TASK-110 finding 4).
 *
 * The platform invokes this callback on the Mongoose poll thread from the
 * mqtt_config_save() -> MQTT_CMD_TYPE_APPLY_CONFIG handler with an owned
 * snapshot of the exact values the reconnect would use.  Only a candidate
 * identical to the module's verified applied state is approved; anything
 * else — SSL off, skip-verify on, raw or relocated certificate material, a
 * different (e.g. plaintext or IP) address, a different client identifier
 * — is rejected, so no caller can make the transport reconnect from
 * unverified values.  The platform fails closed when no callback is
 * registered, and on rejection it leaves the transport disconnected and
 * notifies the failure observer, which fails the lamp off (see
 * mqtt_cfg_on_connect_failure()).
 */
static bool mqtt_cfg_config_gate(const mqtt_config_snapshot_t *candidate)
{
    char expected_address[MQTT_CONFIG_STR_SIZE];
    bool accepted = false;
    int expected_len = -1;

    mqtt_cfg_lock();
    if (candidate != NULL && s_verified_applied &&
        mqtt_cfg_validate_fields(&s_applied_cfg) == MQTT_CFG_OK)
    {
        /* The candidate address is an owned copy of the platform's own
         * MQTT_CONFIG_STR_SIZE storage, so the expected address is formatted
         * into a buffer of exactly that bound.  A formatted address that
         * would not round-trip through the platform storage (snprintf return
         * >= buffer size) can never match a candidate and is rejected
         * explicitly instead of silently truncating the local copy. */
        expected_len = snprintf(expected_address, sizeof(expected_address),
                                MQTT_CFG_ADDRESS_FMT, s_applied_cfg.hostname,
                                (unsigned)s_applied_cfg.port);

        if (expected_len >= 0 &&
            (size_t)expected_len < sizeof(expected_address) &&
            candidate->address != NULL &&
            strcmp(candidate->address, expected_address) == 0 &&
            candidate->client_id != NULL &&
            strcmp(candidate->client_id, s_applied_cfg.client_id) == 0 &&
            candidate->ssl_enabled && !candidate->skip_verify &&
            candidate->cert_source == MQTT_CERT_SOURCE_FILE_PATH &&
            candidate->cert_value != NULL &&
            mqtt_cfg_path_under_cert(candidate->cert_value) &&
            strcmp(candidate->cert_value, s_applied_cfg.ca_path) == 0)
        {
            if (s_applied_cfg.auth_mode == MQTT_CFG_AUTH_MTLS)
            {
                accepted =
                    candidate->client_cert_source ==
                        MQTT_CERT_SOURCE_FILE_PATH &&
                    candidate->client_cert_value != NULL &&
                    strcmp(candidate->client_cert_value,
                           s_applied_cfg.client_cert_path) == 0 &&
                    candidate->client_key_source ==
                        MQTT_CERT_SOURCE_FILE_PATH &&
                    candidate->client_key_value != NULL &&
                    strcmp(candidate->client_key_value,
                           s_applied_cfg.client_key_path) == 0;
            }
            else
            {
                accepted =
                    candidate->client_cert_source == MQTT_CERT_SOURCE_NONE &&
                    candidate->client_key_source == MQTT_CERT_SOURCE_NONE;
            }
        }
    }
    mqtt_cfg_unlock();
    return accepted;
}

mqtt_cfg_status_t mqtt_cfg_validate(const mqtt_cfg_t *cfg)
{
    mqtt_cfg_status_t status;

    mqtt_cfg_lock();
    status = mqtt_cfg_validate_fields(cfg);
    if (status != MQTT_CFG_OK)
    {
        /* A rejected configuration also invalidates any previously applied
         * transport.  Otherwise a caller could load a bad document, observe
         * fail-off, and then reconnect stale transport state. */
        s_verified_applied = false;
    }
    mqtt_cfg_unlock();

    if (status != MQTT_CFG_OK)
    {
        mqtt_cfg_fail_off();
    }
    return status;
}

/* --------------------------------------------------------------------- */
/* Load                                                                   */
/* --------------------------------------------------------------------- */

mqtt_cfg_status_t mqtt_cfg_load(mqtt_cfg_t *out)
{
    char *json = NULL;
    size_t json_len = 0u;
    cJSON *root = NULL;
    mqtt_cfg_status_t status = MQTT_CFG_ERR_IO;

    if (out == NULL)
    {
        /* Every load failure forces the output inactive (fail-off
         * contract); an invalid argument is a failure like any other. */
        mqtt_cfg_lock();
        s_verified_applied = false;
        mqtt_cfg_unlock();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_INVALID_ARGUMENT;
    }

    json = mqtt_cfg_read_file(MQTT_CFG_FILE_PATH, MQTT_CFG_MAX_FILE_BYTES,
                              &json_len);
    if (json == NULL)
    {
        /* Distinguish a missing document from unreadable/oversized data. */
        osal_fstat_t st = { 0 };
        status = (osal_stat(MQTT_CFG_FILE_PATH, &st) == OSAL_SUCCESS)
                     ? MQTT_CFG_ERR_BOUNDS
                     : MQTT_CFG_ERR_NOT_FOUND;
        goto fail;
    }

    /* Strict parse: the whole input must be consumed; trailing garbage or a
     * second concatenated object is rejected as malformed. */
    root = cJSON_ParseWithLengthOpts(json, json_len + 1u, NULL, 1);
    if (root == NULL || !cJSON_IsObject(root))
    {
        status = MQTT_CFG_ERR_MALFORMED;
        goto fail;
    }
    if (mqtt_cfg_raw_json_has_nul_escape(json))
    {
        status = MQTT_CFG_ERR_MALFORMED;
        goto fail;
    }

    /* Parse into a temporary object so malformed or policy-invalid input
     * cannot partially overwrite the caller's last known-good configuration.
     * Publish the result only after parsing and validation both succeed. */
    mqtt_cfg_t parsed;
    status = mqtt_cfg_parse_root(root, &parsed);
    if (status != MQTT_CFG_OK)
    {
        goto fail;
    }

    status = mqtt_cfg_validate_fields(&parsed);
    if (status != MQTT_CFG_OK)
    {
        goto fail;
    }

    *out = parsed;
    cJSON_Delete(root);
    free(json);
    osal_log_info("mqtt_cfg: loaded mqtt.json hostname=%s port=%u "
                  "client_id=%s auth_mode=%d tls_mode=mqtts ca=%s",
                  parsed.hostname, (unsigned)parsed.port, parsed.client_id,
                  (int)parsed.auth_mode, parsed.ca_path);
    return MQTT_CFG_OK;

fail:
    mqtt_cfg_lock();
    s_verified_applied = false;
    mqtt_cfg_unlock();
    cJSON_Delete(root);
    free(json);
    osal_log_error("mqtt_cfg: mqtt.json rejected: %d (%s)",
                   (int)status, mqtt_cfg_status_name(status));
    mqtt_cfg_fail_off();
    return status;
}

/* --------------------------------------------------------------------- */
/* Apply                                                                  */
/* --------------------------------------------------------------------- */

static bool mqtt_cfg_apply_cert_source(mqtt_config_value_t key,
                                       const char *path)
{
    return mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH, path, key);
}

mqtt_cfg_status_t mqtt_cfg_apply(const mqtt_cfg_t *cfg)
{
    char address[MQTT_CFG_HOSTNAME_MAX_LEN + 16u];
    mqtt_cfg_status_t status;
    bool flag = false;
    mqtt_cfg_cert_snapshot_t ca_snapshot = { 0 };
    mqtt_cfg_cert_snapshot_t client_cert_snapshot = { 0 };
    mqtt_cfg_cert_snapshot_t client_key_snapshot = { 0 };
    const char *str = NULL;
    mqtt_cert_source_t source = MQTT_CERT_SOURCE_NONE;
    const char *value = NULL;

    /* Register this module as the verified configuration owner (TASK-121
     * gate, finding 4): the generic mqtt_config_save() reconnect path can
     * only restart the transport with values this callback approves
     * (fail-closed otherwise), so no other caller — hq_cmd_mqtt or any
     * direct mqtt_config setter — can reconnect from unverified values.
     * Registration is idempotent and thread-safe. */
    mqtt_app_set_config_validation_callback(mqtt_cfg_config_gate);

    /* A new apply invalidates and stops any old session before its transport
     * values can be replaced.  This is intentionally done even when the new
     * document later fails validation: a failed reconfiguration must not
     * leave an old (possibly insecure) session usable.  The generation and
     * latch are updated under the module lock; the teardown itself runs
     * outside it (mqtt_app_deinit() blocks on the transport thread, whose
     * safety callbacks never take this lock). */
    mqtt_cfg_lock();
    uint32_t gen = atomic_load_explicit(&s_apply_generation,
                                        memory_order_relaxed) + 1u;
    if (gen == 0u)
    {
        gen = 1u;
    }
    atomic_store_explicit(&s_apply_generation, gen, memory_order_relaxed);
    atomic_store_explicit(&s_connected_generation, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_waiting_for_connection, false,
                          memory_order_relaxed);
    s_verified_applied = false;
    mqtt_cfg_unlock();

    if (mqtt_app_is_connected())
    {
        mqtt_cfg_fail_off();
    }
    mqtt_app_deinit();

    /* Every apply attempt re-arms the connect gate: a rejected (even
     * partially applied) configuration must never leave a usable transport
     * state that mqtt_cfg_connect() could start.  The validated apply
     * sequence (field validation, setters, self-check and snapshot publish)
     * runs under the module lock, so a concurrent connect() can never read
     * half-applied mqtt_config values (finding 2). */
    mqtt_cfg_lock();
    s_verified_applied = false;

    status = mqtt_cfg_validate_fields(cfg);
    if (status != MQTT_CFG_OK)
    {
        mqtt_cfg_unlock();
        mqtt_cfg_fail_off();
        return status;
    }

    /* The transport configuration service must be initialized before its
     * setters are used (idempotent; mqtt_app_init() also calls it). */
    mqtt_config_init();

    if (cfg->auth_mode == MQTT_CFG_AUTH_ACCESS_TOKEN)
    {
        const char *access_token = NULL;

        if (device_identity_token(&access_token) != DEVICE_IDENTITY_OK ||
            access_token == NULL || access_token[0] == '\0' ||
            !mqtt_config_set_string(access_token,
                                    MQTT_CONFIG_VALUE_USERNAME) ||
            !mqtt_config_set_string("", MQTT_CONFIG_VALUE_PASSWORD))
        {
            mqtt_cfg_unlock();
            mqtt_cfg_fail_off();
            return MQTT_CFG_ERR_APPLY;
        }
    }
    else if (!mqtt_config_set_string("", MQTT_CONFIG_VALUE_USERNAME) ||
             !mqtt_config_set_string("", MQTT_CONFIG_VALUE_PASSWORD))
    {
        mqtt_cfg_unlock();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_APPLY;
    }

    if (snprintf(address, sizeof(address), MQTT_CFG_ADDRESS_FMT,
                 cfg->hostname, (unsigned)cfg->port) < 0)
    {
        mqtt_cfg_unlock();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_APPLY;
    }

    if (!mqtt_config_set_string(address, MQTT_CONFIG_VALUE_ADDRESS) ||
        !mqtt_config_set_string(cfg->client_id, MQTT_CONFIG_VALUE_CLIENT_ID) ||
        !mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL) ||
        !mqtt_config_set_bool(false, MQTT_CONFIG_VALUE_SKIP_VERIFY))
    {
        mqtt_cfg_unlock();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_APPLY;
    }

    /* The CA is mandatory and must resolve to a readable file under /cert:
     * a missing/unreadable CA fails the application with the CA-path error
     * (never a generic apply error, and never a disabled verification). */
    if (!mqtt_cfg_apply_cert_source(MQTT_CONFIG_VALUE_CERT, cfg->ca_path))
    {
        mqtt_cfg_unlock();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_CA_PATH;
    }

    if (cfg->auth_mode == MQTT_CFG_AUTH_MTLS)
    {
        if (!mqtt_cfg_apply_cert_source(MQTT_CONFIG_VALUE_CLIENT_CERT,
                                        cfg->client_cert_path) ||
            !mqtt_cfg_apply_cert_source(MQTT_CONFIG_VALUE_CLIENT_KEY,
                                        cfg->client_key_path))
        {
            mqtt_cfg_unlock();
            mqtt_cfg_fail_off();
            return MQTT_CFG_ERR_CERT_PATH;
        }
    }
    else
    {
        /* Clear any previously applied mTLS material so a downgraded
         * configuration cannot silently keep sending a client cert. */
        bool ok = mqtt_config_set_cert_source(MQTT_CERT_SOURCE_NONE, NULL,
                                              MQTT_CONFIG_VALUE_CLIENT_CERT);
        ok = ok && mqtt_config_set_cert_source(MQTT_CERT_SOURCE_NONE, NULL,
                                               MQTT_CONFIG_VALUE_CLIENT_KEY);
        if (!ok)
        {
            mqtt_cfg_unlock();
            mqtt_cfg_fail_off();
            return MQTT_CFG_ERR_APPLY;
        }
    }

    /* Self-check through the getters: the accepted configuration must
     * always be visible as verified TLS. */
    str = mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
    if (str == NULL || strcmp(str, address) != 0)
    {
        mqtt_cfg_unlock();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_APPLY;
    }
    if (!mqtt_config_get_bool(&flag, MQTT_CONFIG_VALUE_SSL) || !flag)
    {
        mqtt_cfg_unlock();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_APPLY;
    }
    if (!mqtt_config_get_bool(&flag, MQTT_CONFIG_VALUE_SKIP_VERIFY) || flag)
    {
        /* skip_verify must never be on for an accepted configuration. */
        (void)mqtt_config_set_bool(false, MQTT_CONFIG_VALUE_SKIP_VERIFY);
        mqtt_cfg_unlock();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_APPLY;
    }
    if (!mqtt_config_get_cert_source(&source, &value,
                                     MQTT_CONFIG_VALUE_CERT) ||
        source != MQTT_CERT_SOURCE_FILE_PATH)
    {
        mqtt_cfg_unlock();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_CA_PATH;
    }
    if (value == NULL || strcmp(value, cfg->ca_path) != 0 ||
        !mqtt_cfg_capture_cert(MQTT_CONFIG_VALUE_CERT, &ca_snapshot))
    {
        mqtt_cfg_unlock();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_CA_PATH;
    }

    if (cfg->auth_mode == MQTT_CFG_AUTH_MTLS &&
        (!mqtt_cfg_capture_cert(MQTT_CONFIG_VALUE_CLIENT_CERT,
                                &client_cert_snapshot) ||
         !mqtt_cfg_capture_cert(MQTT_CONFIG_VALUE_CLIENT_KEY,
                                &client_key_snapshot)))
    {
        mqtt_cfg_unlock();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_CERT_PATH;
    }

    /* All setters and self-checks succeeded.  Copy the accepted fields into
     * module-owned storage before opening the connect gate.  The caller's
     * mqtt_cfg_t may be reused or changed after this point, but the transport
     * can only be connected against this validated snapshot. */
    s_applied_cfg = *cfg;
    s_applied_ca = ca_snapshot;
    s_applied_client_cert = client_cert_snapshot;
    s_applied_client_key = client_key_snapshot;
    s_verified_applied = true;
    mqtt_cfg_unlock();

    osal_log_info("mqtt_cfg: verified transport configured: %s client_id=%s",
                  address, s_applied_cfg.client_id);
    return MQTT_CFG_OK;
}

mqtt_cfg_status_t mqtt_cfg_load_and_apply(void)
{
    mqtt_cfg_t cfg;
    mqtt_cfg_status_t status = mqtt_cfg_load(&cfg);

    if (status != MQTT_CFG_OK)
    {
        return status;
    }
    return mqtt_cfg_apply(&cfg);
}

/* --------------------------------------------------------------------- */
/* Connect (verified TLS)                                                 */
/* --------------------------------------------------------------------- */

static void mqtt_cfg_on_connect(void)
{
    /* Only a connect event observed while this call owns the current apply
     * generation can release the barrier.  All fields are atomics: the
     * callback runs on the Mongoose poll thread while the connect poll loop
     * runs on the caller's thread, and publication uses release ordering
     * (finding 3). */
    if (atomic_load_explicit(&s_waiting_for_connection,
                             memory_order_acquire))
    {
        atomic_store_explicit(
            &s_connected_generation,
            atomic_load_explicit(&s_apply_generation,
                                 memory_order_acquire),
            memory_order_release);
    }
}

static void mqtt_cfg_on_disconnect(mqtt_disconnect_reason_t reason)
{
    /* Once a verified session has been established, every later transport
     * loss is a safety event.  In particular, do not allow an automatic
     * reconnect to release the lamp barrier: only a fresh successful
     * mqtt_cfg_connect() does that. */
    (void)reason;
    atomic_store_explicit(&s_waiting_for_connection, false,
                          memory_order_release);
    atomic_store_explicit(&s_connected_generation, 0u,
                          memory_order_release);
    mqtt_cfg_fail_off();
}

static void mqtt_cfg_on_connect_failure(mqtt_connect_failure_reason_t reason)
{
    /* Fail-off contract: a TLS/transport connection failure forces the
     * output inactive immediately, before the poll loop reports it. */
    atomic_store_explicit(&s_waiting_for_connection, false,
                          memory_order_release);
    atomic_store_explicit(&s_connected_generation, 0u,
                          memory_order_release);
    atomic_store_explicit(&s_connect_failed, true, memory_order_release);
    if (reason == MQTT_CONNECT_FAILURE_REASON_CONFIG_REJECTED)
    {
        /* The generic save/apply path was refused because the candidate did
         * not match the verified snapshot (finding 4).  The platform has
         * already left the transport disconnected; also close the product's
         * connect gate so the mutated state cannot be retried or re-enabled
         * without another validated apply. */
        mqtt_cfg_lock();
        s_verified_applied = false;
        mqtt_cfg_unlock();
    }
    mqtt_cfg_fail_off();
}

mqtt_cfg_status_t mqtt_cfg_connect(uint32_t timeout_ms)
{
    const char *address;
    uint32_t waited = 0u;

    /* Only a fully applied verified configuration may start the transport.
     * The applied latch is owned by mqtt_cfg_apply() (set exclusively after
     * all setters and self-checks succeed, cleared before every apply and
     * on every failure).  The verified check and the values the transport
     * consumes are covered by the module lock (finding 2): apply() cannot
     * interleave between this check and the transport start, and the poll
     * loop re-checks under the same lock before accepting success. */
    mqtt_cfg_lock();
    if (!s_verified_applied)
    {
        /* No apply requested a wait: clear any leftover wait state so a
         * stale generation can never be published for this connect (the
         * timeout path below mirrors this). */
        atomic_store_explicit(&s_waiting_for_connection, false,
                              memory_order_release);
        mqtt_cfg_unlock();
        mqtt_app_deinit();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_NOT_APPLIED;
    }
    if (!mqtt_cfg_applied_transport_matches())
    {
        /* A mutation of any Mongoose value is treated exactly like a failed
         * configuration.  Close the gate so the changed state cannot be
         * retried without another validated apply. */
        s_verified_applied = false;
        atomic_store_explicit(&s_waiting_for_connection, false,
                              memory_order_release);
        mqtt_cfg_unlock();
        mqtt_app_deinit();
        mqtt_cfg_fail_off();
        return MQTT_CFG_ERR_NOT_APPLIED;
    }
    address = mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
    mqtt_cfg_unlock();

    /* Always discard a session that may have been started by another
     * consumer.  The current apply generation must receive a new connect
     * event; merely observing an old connected flag is insufficient.  The
     * teardown runs outside the lock: mqtt_app_deinit() blocks on the
     * transport thread, whose safety callbacks never take this lock. */
    mqtt_app_deinit();

    /* Establish the wait state and generation BEFORE the asynchronous
     * connect starts (finding 3): a connect or failure event published by
     * the transport thread after mqtt_app_init() is never lost. */
    mqtt_cfg_lock();
    atomic_store_explicit(&s_connect_failed, false, memory_order_relaxed);
    atomic_store_explicit(&s_connected_generation, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_waiting_for_connection, true,
                          memory_order_release);
    mqtt_cfg_unlock();

    mqtt_app_set_safety_callbacks(mqtt_cfg_on_connect,
                                  mqtt_cfg_on_disconnect,
                                  mqtt_cfg_on_connect_failure);
    mqtt_app_init();

    for (;;)
    {
        uint32_t apply_gen;
        uint32_t connected_gen;
        bool connect_failed;
        bool connected;
        bool matches;

        mqtt_cfg_lock();
        apply_gen = atomic_load_explicit(&s_apply_generation,
                                         memory_order_acquire);
        connected_gen = atomic_load_explicit(&s_connected_generation,
                                             memory_order_acquire);
        connect_failed = atomic_load_explicit(&s_connect_failed,
                                              memory_order_acquire);
        connected = connected_gen == apply_gen && mqtt_app_is_connected();
        matches = mqtt_cfg_applied_transport_matches();

        if (connected && s_verified_applied && matches)
        {
            atomic_store_explicit(&s_waiting_for_connection, false,
                                  memory_order_release);
            /* Re-enable contract (TASK-110): the lamp fail-off barrier is
             * released only here — after a valid configuration was applied
             * AND the verified TLS connection succeeded.  A Wi-Fi
             * connection alone, a rejected configuration or any failed
             * connect keeps the output latched off.
             *
             * The release is performed while still under the module lock so
             * the verified check, the transport-match re-check and the
             * barrier release form one atomic critical section: a concurrent
             * mqtt_config mutation cannot squeeze into that window and leave
             * the transport authorized against state that is no longer in
             * effect.  The release is a fast, non-blocking call, and the
             * transport callbacks never take this lock, so no deadlock can
             * form. */
            lamp_status_t ls = lamp_control_release_fail_off();
            mqtt_cfg_unlock();
            osal_log_info("mqtt_cfg: verified TLS connection established (%s)",
                          address);
            if (ls != LAMP_OK)
            {
                osal_log_error("mqtt_cfg: fail-off release failed: %d",
                               (int)ls);
            }
            return MQTT_CFG_OK;
        }
        if (connected && !(s_verified_applied && matches))
        {
            /* TOCTOU (finding 2): mqtt_config was mutated while the connect
             * was in flight, so the transport may have consumed unverified
             * values despite the successful entry check.  Refuse, close the
             * gate and fail off: the connection either uses the verified
             * snapshot or is refused with fail-off — never a spurious
             * success on mutated values. */
            s_verified_applied = false;
            atomic_store_explicit(&s_waiting_for_connection, false,
                                  memory_order_release);
            atomic_store_explicit(&s_connected_generation, 0u,
                                  memory_order_release);
            mqtt_cfg_unlock();
            mqtt_app_deinit();
            mqtt_cfg_fail_off();
            return MQTT_CFG_ERR_NOT_APPLIED;
        }
        if (connect_failed)
        {
            mqtt_cfg_unlock();
            return MQTT_CFG_ERR_CONNECT;
        }
        if (timeout_ms == 0u || waited >= timeout_ms)
        {
            /* single immediate check, or the bounded wait expired */
            atomic_store_explicit(&s_waiting_for_connection, false,
                                  memory_order_release);
            atomic_store_explicit(&s_connected_generation, 0u,
                                  memory_order_release);
            mqtt_cfg_unlock();
            osal_log_error(
                "mqtt_cfg: verified TLS connection timed out after %u ms",
                (unsigned)timeout_ms);
            mqtt_cfg_fail_off();
            return MQTT_CFG_ERR_CONNECT;
        }

        uint32_t step = MQTT_CFG_CONNECT_POLL_MS;
        if ((timeout_ms - waited) < step)
        {
            step = timeout_ms - waited;
        }
        mqtt_cfg_unlock();
        (void)osal_task_delay_ms(step);
        waited += step;
    }
}
