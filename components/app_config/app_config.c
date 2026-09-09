/**
 * @file app_config.c
 * @brief Versioned product configuration service implementation (TASK-108)
 *
 * See app_config.h for the normative contract.  Implementation notes:
 *
 *   - all storage access goes through the portable OSAL file API
 *     (osal_file.h) plus cJSON; no ESP-IDF filesystem symbols are
 *     referenced, so the code is host-testable against an in-memory OSAL
 *     filesystem double,
 *   - commit order: validate -> write temporary file -> close (the OSAL has
 *     no separate flush primitive; osal_close() is the durability point) ->
 *     read back and re-validate -> update the last-known-good copy (only
 *     from a currently valid live file, itself read back and validated) ->
 *     atomic osal_rename().  Where the OSAL backend cannot atomically
 *     rename, the commit is refused with APP_CONFIG_ERR_UNSUPPORTED: a
 *     non-atomic copy could truncate a valid live document, so it is never
 *     used as a replacement path,
 *   - load order: live file first; on any failure the ".good" copy is
 *     validated and, only when it validates, restored through the safe
 *     commit path.  Unvalidated data is never promoted,
 *   - schemas are strict: unknown or duplicate members are rejected, every
 *     string field is bounded and charset-checked, so no secret can be
 *     smuggled into a managed document,
 *   - lamp state is never persisted: the API surface contains no write path
 *     for runtime state (ThingsBoard remains the desired-state authority).
 */

#include "app_config.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osal_error.h"
#include "osal_file.h"

#include "cJSON.h"

/* --------------------------------------------------------------------- */
/* Constants                                                              */
/* --------------------------------------------------------------------- */

/** @brief Temporary file suffix. */
#define APP_CONFIG_TMP_SUFFIX ".tmp"

/** @brief Last-known-good file suffix. */
#define APP_CONFIG_GOOD_SUFFIX ".good"

/** @brief Last-known-good staging file suffix. */
#define APP_CONFIG_GOOD_TMP_SUFFIX ".good.tmp"

/** @brief Replacement text for redacted secret values. */
#define APP_CONFIG_REDACTED "[redacted]"

/** @brief Output produced for unparsable diagnostic documents. */
#define APP_CONFIG_UNPARSABLE "[unparsable document]"

/** @brief Working buffer for migrated / committed documents. */
#define APP_CONFIG_WORK_BUF_BYTES (APP_CONFIG_MAX_FILE_BYTES + 1U)

/* --------------------------------------------------------------------- */
/* Status names                                                           */
/* --------------------------------------------------------------------- */

const char *app_config_status_name(app_config_status_t status)
{
    switch (status)
    {
        case APP_CONFIG_OK:                   return "OK";
        case APP_CONFIG_OK_RECOVERED:         return "OK_RECOVERED";
        case APP_CONFIG_ERR_INVALID_ARGUMENT: return "ERR_INVALID_ARGUMENT";
        case APP_CONFIG_ERR_IO:               return "ERR_IO";
        case APP_CONFIG_ERR_NOT_FOUND:        return "ERR_NOT_FOUND";
        case APP_CONFIG_ERR_MALFORMED:        return "ERR_MALFORMED";
        case APP_CONFIG_ERR_BOUNDS:           return "ERR_BOUNDS";
        case APP_CONFIG_ERR_UNKNOWN_SCHEMA:   return "ERR_UNKNOWN_SCHEMA";
        case APP_CONFIG_ERR_MIGRATION:        return "ERR_MIGRATION";
        case APP_CONFIG_ERR_UNSUPPORTED:      return "ERR_UNSUPPORTED";
        default:                              return "UNKNOWN";
    }
}

/* --------------------------------------------------------------------- */
/* Document paths                                                         */
/* --------------------------------------------------------------------- */

const char *app_config_doc_path(app_config_doc_kind_t kind)
{
    switch (kind)
    {
        case APP_CONFIG_DOC_DEVICE:        return APP_CONFIG_DEVICE_PATH;
        case APP_CONFIG_DOC_MANUFACTURING: return APP_CONFIG_MANUFACTURING_PATH;
        default:                           return NULL;
    }
}

/** @brief Build "<live><suffix>"; false on invalid kind / overflow. */
static bool build_variant_path(app_config_doc_kind_t kind,
                               const char *suffix,
                               char *out,
                               size_t out_cap)
{
    const char *live = app_config_doc_path(kind);
    if ((live == NULL) || (suffix == NULL) || (out == NULL))
    {
        return false;
    }

    const int written = snprintf(out, out_cap, "%s%s", live, suffix);
    return (written > 0) && ((size_t)written < out_cap);
}

/* --------------------------------------------------------------------- */
/* Raw file helpers                                                       */
/* --------------------------------------------------------------------- */

/** @brief Does this OSAL status mean "the file simply is not there"? */
static bool status_is_missing(int32_t rc)
{
    return (rc == OSAL_ERR_NAME_NOT_FOUND) ||
           (rc == OSAL_FS_ERR_PATH_INVALID) ||
           (rc == OSAL_FS_ERR_NAME_TOO_LONG) ||
           (rc == OSAL_FS_ERR_PATH_TOO_LONG);
}

/**
 * @brief Does this OSAL status mean "could not open for reading" in a way
 *        that is indistinguishable from "no such document" on backends
 *        without a distinct missing-file code (the LittleFS backend maps
 *        LFS_ERR_NOENT to OSAL_ERROR)?
 */
static bool status_is_missing_or_unreadable(int32_t rc)
{
    return status_is_missing(rc) || (rc == OSAL_ERROR);
}

/**
 * @brief Read a whole file, enforcing the document size bound.
 *
 * @param[out] buf     Buffer receiving the NUL-terminated contents.
 * @param[in]  cap     Buffer capacity in bytes (must exceed the bound).
 * @param[out] out_len Length of the contents (excluding the terminator).
 */
static app_config_status_t read_whole_file(const char *path,
                                           char *buf,
                                           size_t cap,
                                           size_t *out_len)
{
    if ((path == NULL) || (buf == NULL) || (cap == 0U) || (out_len == NULL))
    {
        return APP_CONFIG_ERR_INVALID_ARGUMENT;
    }

    *out_len = 0U;
    buf[0] = '\0';

    osal_fstat_t st;
    int32_t rc = osal_stat(path, &st);
    if (rc != OSAL_SUCCESS)
    {
        /* A missing document is a normal condition (first provisioning);
         * on backends without a distinct missing-file code the stat/open
         * failure is indistinguishable from "not there" and is reported as
         * NOT_FOUND so the last-known-good recovery path still runs. */
        return status_is_missing_or_unreadable(rc) ? APP_CONFIG_ERR_NOT_FOUND
                                                   : APP_CONFIG_ERR_IO;
    }

    if (OSAL_FILESTAT_SIZE(st) > APP_CONFIG_MAX_FILE_BYTES)
    {
        return APP_CONFIG_ERR_BOUNDS;
    }

    osal_file_id_t fd =
        osal_open_create(path, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    if (fd < 0)
    {
        return status_is_missing_or_unreadable(fd) ? APP_CONFIG_ERR_NOT_FOUND
                                                   : APP_CONFIG_ERR_IO;
    }

    size_t total = 0U;
    app_config_status_t status = APP_CONFIG_OK;

    /* Read until EOF.  Exactly APP_CONFIG_MAX_FILE_BYTES are accepted; the
     * buffer holds one extra probe byte so a file that grew beyond the
     * bound (or between stat() and read()) is detected by observing a real
     * byte beyond the maximum instead of colliding with the terminator. */
    while (total <= APP_CONFIG_MAX_FILE_BYTES)
    {
        /* Probe budget: 1 byte beyond the bound when the bound was filled. */
        size_t want = (total < APP_CONFIG_MAX_FILE_BYTES)
                          ? (APP_CONFIG_MAX_FILE_BYTES - total)
                          : 1U;
        const size_t room = cap - 1U;
        if (want > room)
        {
            want = room;
        }

        const int32_t nread = osal_read(fd, buf + total, want);
        if (nread < 0)
        {
            status = APP_CONFIG_ERR_IO;
            break;
        }
        if (nread == 0)
        {
            break; /* EOF */
        }
        total += (size_t)nread;
    }
    const int32_t close_rc = osal_close(fd);

    if (status != APP_CONFIG_OK)
    {
        return status;
    }
    if (close_rc != OSAL_SUCCESS)
    {
        return APP_CONFIG_ERR_IO;
    }

    if (total > APP_CONFIG_MAX_FILE_BYTES)
    {
        /* The probe read succeeded: the file holds real data beyond the
         * advertised maximum, so it is oversized (never accept it and never
         * terminate the buffer past its allocated capacity). */
        return APP_CONFIG_ERR_BOUNDS;
    }

    buf[total] = '\0';
    *out_len = total;
    return APP_CONFIG_OK;
}

/**
 * @brief Write @p len bytes to @p path, replacing any existing content.
 *
 * osal_close() is the flush/durability point: the OSAL file API exposes no
 * separate flush primitive, and close pushes the written data out of the
 * filesystem's write-back cache.
 */
static bool write_whole_file(const char *path, const char *data, size_t len)
{
    if ((path == NULL) || ((data == NULL) && (len > 0U)))
    {
        return false;
    }

    osal_file_id_t fd = osal_open_create(
        path,
        (osal_file_flag_t)(OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE),
        OSAL_WRITE_ONLY);
    if (fd < 0)
    {
        return false;
    }

    bool ok = true;
    size_t offset = 0U;

    while (ok && (offset < len))
    {
        const int32_t nwritten = osal_write(fd, data + offset, len - offset);
        if (nwritten <= 0)
        {
            ok = false; /* Interrupted write: partial data stays behind. */
        }
        else
        {
            offset += (size_t)nwritten;
        }
    }

    if (osal_close(fd) != OSAL_SUCCESS)
    {
        ok = false;
    }

    return ok;
}

/** @brief Best-effort remove; "already gone" counts as success. */
static void remove_quiet(const char *path)
{
    (void)osal_remove(path);
}

/* --------------------------------------------------------------------- */
/* JSON field helpers                                                     */
/* --------------------------------------------------------------------- */

/** @brief Printable ASCII only (0x20..0x7E) — log-safe, path-safe. */
static bool string_is_printable_ascii(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p)
    {
        if ((*p < 0x20U) || (*p > 0x7EU))
        {
            return false;
        }
    }
    return true;
}

/** @brief Serial numbers: A-Z a-z 0-9 and '-'. */
static bool string_is_serial_charset(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p)
    {
        const unsigned char c = *p;
        const bool ok = ((c >= 'A') && (c <= 'Z')) ||
                        ((c >= 'a') && (c <= 'z')) ||
                        ((c >= '0') && (c <= '9')) || (c == '-');
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Extract a bounded printable-ASCII string member.
 *
 * @return #APP_CONFIG_OK, #APP_CONFIG_ERR_MALFORMED (missing / wrong type /
 *         bad charset) or #APP_CONFIG_ERR_BOUNDS (value too long).
 */
static app_config_status_t get_bounded_string(const cJSON *object,
                                              const char *key,
                                              char *out,
                                              size_t cap,
                                              bool serial)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    if ((item == NULL) || !cJSON_IsString(item) || (item->valuestring == NULL))
    {
        return APP_CONFIG_ERR_MALFORMED;
    }

    const size_t len = strlen(item->valuestring);
    if ((len == 0U) || (len >= cap))
    {
        /* Identity fields are defined as 1..cap-1 characters: an empty
         * value is a bounds violation just like an over-long one. */
        return APP_CONFIG_ERR_BOUNDS;
    }
    if (!string_is_printable_ascii(item->valuestring))
    {
        return APP_CONFIG_ERR_MALFORMED;
    }
    if (serial && !string_is_serial_charset(item->valuestring))
    {
        return APP_CONFIG_ERR_MALFORMED;
    }

    memcpy(out, item->valuestring, len + 1U);
    return APP_CONFIG_OK;
}

/**
 * @brief Extract an integral, non-negative, bounded number member.
 */
static app_config_status_t get_bounded_number(const cJSON *object,
                                              const char *key,
                                              uint32_t limit,
                                              uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    if ((item == NULL) || !cJSON_IsNumber(item))
    {
        return APP_CONFIG_ERR_MALFORMED;
    }

    const double v = item->valuedouble;

    /* Reject non-finite values (NaN / +/-Infinity, e.g. from "1e309")
     * before any floating-to-integer conversion, which would be undefined
     * behavior for out-of-range operands. */
    if ((v != v) || (v < 0.0) || (v > DBL_MAX))
    {
        return APP_CONFIG_ERR_MALFORMED;
    }

    if ((v > (double)limit) || ((double)(uint32_t)v != v))
    {
        return APP_CONFIG_ERR_BOUNDS;
    }

    *out = (uint32_t)v;
    return APP_CONFIG_OK;
}

/**
 * @brief Extract the integral `schema_version` member.
 *
 * Version envelopes have dedicated error categories:
 *   - missing / non-finite / negative / non-integral
 *     -> #APP_CONFIG_ERR_MALFORMED,
 *   - any integral value above the current schema version
 *     -> #APP_CONFIG_ERR_UNKNOWN_SCHEMA (a newer schema is unsupported,
 *     not a field-size violation).  This covers every finite integral
 *     value the JSON codec can represent, including ones that exceed the
 *     uint32_t capacity — no arbitrary upper limit changes the error
 *     category,
 *   - integral values within the supported envelope are returned.
 *
 * Non-finite values (NaN, +/-Infinity from inputs such as "1e309" or
 * "-1e309") are rejected before any floating-to-integer conversion, which
 * would otherwise be undefined behavior.
 */
static app_config_status_t get_schema_version(const cJSON *object,
                                              uint32_t *out)
{
    const cJSON *item =
        cJSON_GetObjectItemCaseSensitive(object, "schema_version");

    if ((item == NULL) || !cJSON_IsNumber(item))
    {
        return APP_CONFIG_ERR_MALFORMED;
    }

    const double v = item->valuedouble;

    /* NaN is never ordered, so (v != v) is true for NaN only.  Anything
     * beyond DBL_MAX overflows to +/-Infinity when parsed.  Both are
     * non-integral representations, i.e. malformed documents — checked
     * BEFORE any float-to-integer conversion (UB otherwise). */
    if ((v != v) || (v < 0.0) || (v > DBL_MAX))
    {
        return APP_CONFIG_ERR_MALFORMED;
    }

    /* v is finite and >= 0.0 here.  Values above 2^63 exceed the exact
     * integer range that a double round-trip can classify, and cannot be
     * current schema versions in any case: bound them before any
     * float-to-integer conversion so the conversion is always defined. */
    if (v >= 9.2233720368547758e18) /* 2^63 */
    {
        return APP_CONFIG_ERR_UNKNOWN_SCHEMA;
    }

    /* Defined round-trip: v < 2^63 here, so the uint64_t conversion is in
     * range; a non-integral value converts back to a different double. */
    if ((double)(uint64_t)v != v)
    {
        /* Fractional values are malformed documents, not newer schemas. */
        return APP_CONFIG_ERR_MALFORMED;
    }

    if (v > (double)UINT32_MAX)
    {
        /* Integral but beyond the representable version range: still a
         * newer schema, not a field-size violation. */
        return APP_CONFIG_ERR_UNKNOWN_SCHEMA;
    }

    const uint32_t version = (uint32_t)v;
    if (version > APP_CONFIG_SCHEMA_VERSION)
    {
        return APP_CONFIG_ERR_UNKNOWN_SCHEMA;
    }

    *out = version;
    return APP_CONFIG_OK;
}

/**
 * @brief Reject unknown members, unnamed members and duplicate keys.
 *
 * The caller additionally requires every allowed member to be present, so
 * together these rules force the object to contain exactly the schema's
 * key set.
 */
static app_config_status_t check_member_keys(const cJSON *object,
                                             const char *const *allowed,
                                             size_t allowed_count)
{
    const cJSON *child = NULL;
    size_t members = 0U;

    cJSON_ArrayForEach(child, object)
    {
        if ((child->string == NULL) || (child->string[0] == '\0'))
        {
            return APP_CONFIG_ERR_MALFORMED;
        }

        bool known = false;
        for (size_t i = 0U; i < allowed_count; ++i)
        {
            if (strcmp(child->string, allowed[i]) == 0)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            return APP_CONFIG_ERR_MALFORMED;
        }
        ++members;
    }

    if (members > allowed_count)
    {
        return APP_CONFIG_ERR_MALFORMED; /* At least one duplicate key. */
    }

    return APP_CONFIG_OK;
}

/* --------------------------------------------------------------------- */
/* Schema: device.json                                                    */
/* --------------------------------------------------------------------- */

static const char *const DEVICE_KEYS[] = {
    "schema_version", "product", "hardware_revision", "serial",
    "thingsboard_name",
};

#define DEVICE_KEY_COUNT (sizeof(DEVICE_KEYS) / sizeof(DEVICE_KEYS[0]))

app_config_status_t app_config_validate_device_json(
    const char *json, app_config_device_doc_t *doc)
{
    if (json == NULL)
    {
        return APP_CONFIG_ERR_INVALID_ARGUMENT;
    }

    const size_t len = strlen(json);
    if (len > APP_CONFIG_MAX_FILE_BYTES)
    {
        return APP_CONFIG_ERR_BOUNDS;
    }

    cJSON *root = cJSON_ParseWithLength(json, len + 1U);
    if ((root == NULL) || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return APP_CONFIG_ERR_MALFORMED;
    }

    app_config_status_t status = APP_CONFIG_OK;

    uint32_t version = 0U;
    status = get_schema_version(root, &version);
    if (status == APP_CONFIG_OK)
    {
        /* Version envelope first: a legacy document (even one whose member
         * set differs from the current schema) must surface ERR_MIGRATION,
         * and a newer one ERR_UNKNOWN_SCHEMA — never a generic malformed
         * error. */
        if (version < APP_CONFIG_SCHEMA_VERSION)
        {
            /* Legacy envelope: only the current schema may be materialized
             * here.  Migration happens through the explicit hook
             * (app_config_migrate_stored() and the registered handler). */
            status = APP_CONFIG_ERR_MIGRATION;
        }
    }

    if (status == APP_CONFIG_OK)
    {
        status = check_member_keys(root, DEVICE_KEYS, DEVICE_KEY_COUNT);
    }

    if (status == APP_CONFIG_OK)
    {
        app_config_device_doc_t parsed;
        (void)memset(&parsed, 0, sizeof(parsed));
        parsed.schema_version = version;

        status = get_bounded_string(root, "product", parsed.product,
                                    sizeof(parsed.product), false);
        if (status == APP_CONFIG_OK)
        {
            status = get_bounded_string(root, "hardware_revision",
                                        parsed.hardware_revision,
                                        sizeof(parsed.hardware_revision),
                                        false);
        }
        if (status == APP_CONFIG_OK)
        {
            status = get_bounded_string(root, "serial", parsed.serial,
                                        sizeof(parsed.serial), true);
        }
        if (status == APP_CONFIG_OK)
        {
            status = get_bounded_string(root, "thingsboard_name",
                                        parsed.thingsboard_name,
                                        sizeof(parsed.thingsboard_name),
                                        false);
        }

        if ((status == APP_CONFIG_OK) && (doc != NULL))
        {
            *doc = parsed;
        }
    }

    cJSON_Delete(root);
    return status;
}

/* --------------------------------------------------------------------- */
/* Schema: manufacturing.json                                             */
/* --------------------------------------------------------------------- */

static const char *const MANUFACTURING_KEYS[] = {
    "schema_version", "manufacturing_state", "credential_mode",
};

#define MANUFACTURING_KEY_COUNT \
    (sizeof(MANUFACTURING_KEYS) / sizeof(MANUFACTURING_KEYS[0]))

app_config_status_t app_config_validate_manufacturing_json(
    const char *json, app_config_manufacturing_doc_t *doc)
{
    if (json == NULL)
    {
        return APP_CONFIG_ERR_INVALID_ARGUMENT;
    }

    const size_t len = strlen(json);
    if (len > APP_CONFIG_MAX_FILE_BYTES)
    {
        return APP_CONFIG_ERR_BOUNDS;
    }

    cJSON *root = cJSON_ParseWithLength(json, len + 1U);
    if ((root == NULL) || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return APP_CONFIG_ERR_MALFORMED;
    }

    app_config_status_t status = APP_CONFIG_OK;

    uint32_t version = 0U;
    status = get_schema_version(root, &version);
    if (status == APP_CONFIG_OK)
    {
        /* Version envelope first (see the device validator). */
        if (version < APP_CONFIG_SCHEMA_VERSION)
        {
            status = APP_CONFIG_ERR_MIGRATION;
        }
    }

    if (status == APP_CONFIG_OK)
    {
        status = check_member_keys(root, MANUFACTURING_KEYS,
                                   MANUFACTURING_KEY_COUNT);
    }

    if (status == APP_CONFIG_OK)
    {
        uint32_t state = 0U;
        uint32_t mode = 0U;

        status = get_bounded_number(root, "manufacturing_state",
                                    (uint32_t)APP_CONFIG_MFG_STATE_QUARANTINED,
                                    &state);
        if (status == APP_CONFIG_OK)
        {
            status = get_bounded_number(root, "credential_mode",
                                        (uint32_t)APP_CONFIG_CRED_MODE_MTLS,
                                        &mode);
        }

        if ((status == APP_CONFIG_OK) && (doc != NULL))
        {
            doc->schema_version      = version;
            doc->manufacturing_state = (app_config_mfg_state_t)state;
            doc->credential_mode     = (app_config_credential_mode_t)mode;
        }
    }

    cJSON_Delete(root);
    return status;
}

/* --------------------------------------------------------------------- */
/* Serialization                                                          */
/* --------------------------------------------------------------------- */

/**
 * @brief Bounded validity check for a caller-provided character array.
 *
 * The typed commit path receives plain character arrays whose content is
 * caller-controlled; they are not guaranteed to be NUL-terminated.  Every
 * check below works on at most @p cap bytes, so no unbounded string API is
 * reachable with such input.
 *
 * A valid field occupies exactly @p out_len bytes (1..@p max_len printable
 * ASCII characters, or the serial charset when @p serial is set) followed
 * by a NUL terminator within the first @p cap bytes of the array.
 *
 * @param[in]  s        Character array (never dereferenced past @p cap).
 * @param[in]  cap      Full capacity of the array in bytes.
 * @param[in]  max_len  Maximum field length in characters.
 * @param[in]  serial   Require the serial-number charset.
 * @param[out] out_len  Length of the valid field in characters.
 *
 * @return #APP_CONFIG_OK when the field is valid, #APP_CONFIG_ERR_BOUNDS
 *         when it is empty, too long or has no terminator within @p cap,
 *         and #APP_CONFIG_ERR_MALFORMED when a character violates the
 *         charset.
 */
static app_config_status_t typed_string_check(const char *s,
                                              size_t cap,
                                              size_t max_len,
                                              bool serial,
                                              size_t *out_len)
{
    if ((s == NULL) || (cap == 0U))
    {
        return APP_CONFIG_ERR_BOUNDS;
    }

    /* Bounded scan: the terminator must exist within the array capacity.
     * An unterminated array is a bounds violation, never a read past the
     * caller's storage. */
    const char *nul = (const char *)memchr(s, '\0', cap);
    if (nul == NULL)
    {
        return APP_CONFIG_ERR_BOUNDS;
    }

    const size_t len = (size_t)(nul - s);
    if ((len == 0U) || (len > max_len))
    {
        /* Identity fields are defined as 1..max_len characters: an empty
         * value is a bounds violation just like an over-long one. */
        return APP_CONFIG_ERR_BOUNDS;
    }

    for (size_t i = 0U; i < len; ++i)
    {
        const unsigned char c = (unsigned char)s[i];
        const bool printable = (c >= 0x20U) && (c <= 0x7EU);
        const bool allowed = serial
                                 ? ((c >= 'A') && (c <= 'Z')) ||
                                       ((c >= 'a') && (c <= 'z')) ||
                                       ((c >= '0') && (c <= '9')) || (c == '-')
                                 : printable;
        if (!allowed)
        {
            return APP_CONFIG_ERR_MALFORMED;
        }
    }

    *out_len = len;
    return APP_CONFIG_OK;
}

/**
 * @brief Validate every string field of a typed device document using
 *        bounded (never strlen-driven) scans.
 *
 * This is the gate between the caller-controlled typed struct and the
 * serializer: no string below is handed to cJSON (or any other unbounded
 * consumer) unless it passed this check, so an unterminated or
 * charset-violating array can never reach serialization.
 */
static app_config_status_t device_doc_strings_valid(
    const app_config_device_doc_t *doc)
{
    size_t len = 0U;
    app_config_status_t status = typed_string_check(
        doc->product, sizeof(doc->product), APP_CONFIG_PRODUCT_MAX_LEN,
        false, &len);
    if (status == APP_CONFIG_OK)
    {
        status = typed_string_check(doc->hardware_revision,
                                    sizeof(doc->hardware_revision),
                                    APP_CONFIG_HARDWARE_REVISION_MAX_LEN,
                                    false, &len);
    }
    if (status == APP_CONFIG_OK)
    {
        status = typed_string_check(doc->serial, sizeof(doc->serial),
                                    APP_CONFIG_SERIAL_MAX_LEN, true, &len);
    }
    if (status == APP_CONFIG_OK)
    {
        status = typed_string_check(doc->thingsboard_name,
                                    sizeof(doc->thingsboard_name),
                                    APP_CONFIG_THINGSBOARD_NAME_MAX_LEN,
                                    false, &len);
    }
    return status;
}

/**
 * @brief Serialize a device document to compact JSON at the current schema.
 *
 * @return true on success (output fits and stays within the file bound).
 */
static bool serialize_device(const app_config_device_doc_t *doc,
                             char *out,
                             size_t cap)
{
    const app_config_status_t vs = device_doc_strings_valid(doc);
    if (vs != APP_CONFIG_OK)
    {
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return false;
    }

    bool ok =
        (cJSON_AddNumberToObject(root, "schema_version",
                                 (double)APP_CONFIG_SCHEMA_VERSION) != NULL) &&
        (cJSON_AddStringToObject(root, "product", doc->product) != NULL) &&
        (cJSON_AddStringToObject(root, "hardware_revision",
                                 doc->hardware_revision) != NULL) &&
        (cJSON_AddStringToObject(root, "serial", doc->serial) != NULL) &&
        (cJSON_AddStringToObject(root, "thingsboard_name",
                                 doc->thingsboard_name) != NULL);

    char *printed = ok ? cJSON_PrintBuffered(root, (int)cap, 0) : NULL;
    ok = (printed != NULL);

    if (ok)
    {
        const size_t len = strlen(printed);
        ok = (len < cap) && (len <= APP_CONFIG_MAX_FILE_BYTES);
        if (ok)
        {
            memcpy(out, printed, len + 1U);
        }
    }

    free(printed);
    cJSON_Delete(root);
    return ok;
}

/** @brief Serialize a manufacturing document (see serialize_device). */
static bool serialize_manufacturing(const app_config_manufacturing_doc_t *doc,
                                    char *out,
                                    size_t cap)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return false;
    }

    bool ok =
        (cJSON_AddNumberToObject(root, "schema_version",
                                 (double)APP_CONFIG_SCHEMA_VERSION) != NULL) &&
        (cJSON_AddNumberToObject(root, "manufacturing_state",
                                 (double)doc->manufacturing_state) != NULL) &&
        (cJSON_AddNumberToObject(root, "credential_mode",
                                 (double)doc->credential_mode) != NULL);

    char *printed = ok ? cJSON_PrintBuffered(root, (int)cap, 0) : NULL;
    ok = (printed != NULL);

    if (ok)
    {
        const size_t len = strlen(printed);
        ok = (len < cap) && (len <= APP_CONFIG_MAX_FILE_BYTES);
        if (ok)
        {
            memcpy(out, printed, len + 1U);
        }
    }

    free(printed);
    cJSON_Delete(root);
    return ok;
}

/* --------------------------------------------------------------------- */
/* Kind dispatch                                                          */
/* --------------------------------------------------------------------- */

/**
 * @brief Validate JSON text for a document kind into a typed document.
 *
 * @param doc Void pointer to the kind's document struct (may be NULL).
 */
static app_config_status_t validate_kind(app_config_doc_kind_t kind,
                                         const char *json,
                                         void *doc)
{
    switch (kind)
    {
        case APP_CONFIG_DOC_DEVICE:
            return app_config_validate_device_json(
                json, (app_config_device_doc_t *)doc);
        case APP_CONFIG_DOC_MANUFACTURING:
            return app_config_validate_manufacturing_json(
                json, (app_config_manufacturing_doc_t *)doc);
        default:
            return APP_CONFIG_ERR_INVALID_ARGUMENT;
    }
}

/** @brief Serialize a typed document for a kind (see serialize_device). */
static bool serialize_kind(app_config_doc_kind_t kind,
                           const void *doc,
                           char *out,
                           size_t cap)
{
    switch (kind)
    {
        case APP_CONFIG_DOC_DEVICE:
            return serialize_device((const app_config_device_doc_t *)doc, out,
                                    cap);
        case APP_CONFIG_DOC_MANUFACTURING:
            return serialize_manufacturing(
                (const app_config_manufacturing_doc_t *)doc, out, cap);
        default:
            return false;
    }
}

/** @brief Check that a commit request carries the current schema version. */
static app_config_status_t check_commit_version(app_config_doc_kind_t kind,
                                                uint32_t schema_version)
{
    if (schema_version > APP_CONFIG_SCHEMA_VERSION)
    {
        return APP_CONFIG_ERR_UNKNOWN_SCHEMA;
    }
    if (schema_version < APP_CONFIG_SCHEMA_VERSION)
    {
        return APP_CONFIG_ERR_MIGRATION;
    }
    (void)kind;
    return APP_CONFIG_OK;
}

/* --------------------------------------------------------------------- */
/* Migration hook                                                         */
/* --------------------------------------------------------------------- */

static app_config_migrate_fn_t s_migration_handler;

void app_config_register_migration_handler(app_config_migrate_fn_t handler)
{
    s_migration_handler = handler;
}

/**
 * @brief Extract the integral schema_version member of a JSON object.
 */
static app_config_status_t extract_schema_version(const char *json,
                                                  uint32_t *out)
{
    if ((json == NULL) || (out == NULL))
    {
        return APP_CONFIG_ERR_INVALID_ARGUMENT;
    }

    cJSON *root = cJSON_Parse(json);
    if ((root == NULL) || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return APP_CONFIG_ERR_MALFORMED;
    }

    uint32_t version = 0U;
    const app_config_status_t status = get_schema_version(root, &version);
    cJSON_Delete(root);

    if (status == APP_CONFIG_OK)
    {
        *out = version;
    }
    return status;
}

/**
 * @brief Run the registered migration hook and validate its output.
 *
 * @param out Buffer receiving the migrated (current-schema) JSON text.
 */
static app_config_status_t migrate_json(app_config_doc_kind_t kind,
                                        const char *json,
                                        uint32_t from_version,
                                        char *out,
                                        size_t cap)
{
    if (s_migration_handler == NULL)
    {
        return APP_CONFIG_ERR_MIGRATION;
    }

    const app_config_status_t rc =
        s_migration_handler(json, from_version, out, cap);
    if (rc != APP_CONFIG_OK)
    {
        return APP_CONFIG_ERR_MIGRATION;
    }

    /* The handler's output must validate against the current schema of the
     * same document kind before it is trusted. */
    const app_config_status_t status = validate_kind(kind, out, NULL);
    if (status != APP_CONFIG_OK)
    {
        return APP_CONFIG_ERR_MIGRATION;
    }
    return APP_CONFIG_OK;
}

/* --------------------------------------------------------------------- */
/* Commit machinery (validated, atomic replacement)                        */
/* --------------------------------------------------------------------- */

/** @brief Path buffer size (matches OSAL_MAX_PATH_LEN). */
#define APP_CONFIG_PATH_CAP OSAL_MAX_PATH_LEN

/**
 * @brief Store an already-validated JSON document through the safe path.
 *
 * Steps (normative, see the header):
 *   1. write "<live>.tmp", flush/close,
 *   2. read the temporary file back and re-validate it,
 *   3. if the live file exists and validates, refresh the last-known-good
 *      copy safely: copy "<live>" to "<live>.good.tmp", read that staging
 *      copy back and validate it, and only then atomically replace
 *      "<live>.good" with it (osal_rename).  Any staging/validation/
 *      replacement failure leaves the previous "<live>.good" untouched, so
 *      a transient write failure can never destroy the only validated
 *      recovery source,
 *   4. atomically replace the live file with the temporary file
 *      (osal_rename).  When the backend cannot rename, the commit is
 *      refused with APP_CONFIG_ERR_UNSUPPORTED: a copy-then-remove fallback
 *      could truncate the valid live document mid-copy, so a non-atomic
 *      replacement path is never used.
 *
 * Any failure leaves the previous live document and its last-known-good
 * copy untouched (best-effort temporary file cleanup aside).
 */
static app_config_status_t commit_json(app_config_doc_kind_t kind,
                                       const char *json)
{
    char tmp_path[OSAL_MAX_PATH_LEN];
    char good_path[OSAL_MAX_PATH_LEN];
    char good_tmp_path[OSAL_MAX_PATH_LEN];

    if (!build_variant_path(kind, APP_CONFIG_TMP_SUFFIX, tmp_path,
                            sizeof(tmp_path)) ||
        !build_variant_path(kind, APP_CONFIG_GOOD_SUFFIX, good_path,
                            sizeof(good_path)) ||
        !build_variant_path(kind, APP_CONFIG_GOOD_TMP_SUFFIX, good_tmp_path,
                            sizeof(good_tmp_path)))
    {
        return APP_CONFIG_ERR_INVALID_ARGUMENT;
    }

    const char *live_path = app_config_doc_path(kind);

    char *buf = malloc(APP_CONFIG_WORK_BUF_BYTES);
    if (buf == NULL)
    {
        return APP_CONFIG_ERR_IO;
    }

    app_config_status_t status = APP_CONFIG_OK;
    size_t len = 0U;

    /* Step 1: temporary write + flush/close. */
    if (!write_whole_file(tmp_path, json, strlen(json)))
    {
        remove_quiet(tmp_path);
        free(buf);
        return APP_CONFIG_ERR_IO;
    }

    /* Step 2: read back and validate what actually reached the storage. */
    status = read_whole_file(tmp_path, buf, APP_CONFIG_WORK_BUF_BYTES, &len);
    if (status == APP_CONFIG_OK)
    {
        status = validate_kind(kind, buf, NULL);
    }
    if (status != APP_CONFIG_OK)
    {
        remove_quiet(tmp_path);
        free(buf);
        return status;
    }

    /* Step 3: last-known-good update, only from a currently valid live
     * file, staged through "<live>.good.tmp" and validated there before the
     * atomic replace.  A corrupt live file is never archived; a failing
     * staging copy never destroys the previous "<live>.good". */
    osal_fstat_t st;
    const int32_t stat_rc = osal_stat(live_path, &st);
    if (stat_rc == OSAL_SUCCESS)
    {
        app_config_status_t live_status =
            read_whole_file(live_path, buf, APP_CONFIG_WORK_BUF_BYTES, &len);
        if ((live_status == APP_CONFIG_OK) &&
            (validate_kind(kind, buf, NULL) != APP_CONFIG_OK))
        {
            live_status = APP_CONFIG_ERR_MALFORMED;
        }

        if (live_status == APP_CONFIG_OK)
        {
            /* Stage the copy, then read it back and validate it BEFORE the
             * previous last-known-good copy is replaced. */
            const int32_t cp_rc = osal_cp(live_path, good_tmp_path);
            if (cp_rc != OSAL_SUCCESS)
            {
                remove_quiet(good_tmp_path);
                remove_quiet(tmp_path);
                free(buf);
                return APP_CONFIG_ERR_IO;
            }

            app_config_status_t good_status = read_whole_file(
                good_tmp_path, buf, APP_CONFIG_WORK_BUF_BYTES, &len);
            if ((good_status == APP_CONFIG_OK) &&
                (validate_kind(kind, buf, NULL) != APP_CONFIG_OK))
            {
                good_status = APP_CONFIG_ERR_MALFORMED;
            }
            if (good_status != APP_CONFIG_OK)
            {
                /* Keep the previous "<live>.good": it remains the only
                 * validated recovery source. */
                remove_quiet(good_tmp_path);
                remove_quiet(tmp_path);
                free(buf);
                return APP_CONFIG_ERR_IO;
            }

            /* The staged copy validated; atomically promote it.  Without
             * atomic rename the promotion is refused: a copy-then-remove
             * fallback could truncate the only validated recovery source
             * mid-copy, so the previous "<live>.good" is preserved as-is. */
            if (osal_rename(good_tmp_path, good_path) != OSAL_SUCCESS)
            {
                remove_quiet(good_tmp_path);
                remove_quiet(tmp_path);
                free(buf);
                return APP_CONFIG_ERR_UNSUPPORTED;
            }
        }
        /* A corrupt live file falls through: the commit repairs it, and the
         * previous ".good" (if any) remains the recovery source. */
    }
    else if (!status_is_missing(stat_rc))
    {
        remove_quiet(tmp_path);
        free(buf);
        return APP_CONFIG_ERR_IO;
    }

    /* Step 4: atomic replacement.  Without rename support the live file is
     * never overwritten non-atomically: a copy-then-remove fallback could
     * leave the live document truncated/invalid if the copy is interrupted,
     * which the storage contract forbids ("invalid data cannot replace
     * valid live configuration"). */
    const int32_t rename_rc = osal_rename(tmp_path, live_path);
    if (rename_rc != OSAL_SUCCESS)
    {
        remove_quiet(tmp_path);
        free(buf);
        return ((rename_rc == OSAL_ERR_OPERATION_NOT_SUPPORTED) ||
                (rename_rc == OSAL_ERR_NOT_IMPLEMENTED))
                   ? APP_CONFIG_ERR_UNSUPPORTED
                   : APP_CONFIG_ERR_IO;
    }

    free(buf);
    return APP_CONFIG_OK;
}

/** @brief Validate, serialize and store a typed document (safe path). */
static app_config_status_t commit_kind(app_config_doc_kind_t kind,
                                       const void *doc)
{
    char *json = malloc(APP_CONFIG_WORK_BUF_BYTES);
    if (json == NULL)
    {
        return APP_CONFIG_ERR_IO;
    }

    app_config_status_t status = APP_CONFIG_OK;

    if (kind == APP_CONFIG_DOC_DEVICE)
    {
        /* Bounded gate: caller-controlled string fields are validated with
         * bounded scans (no strlen on possibly unterminated arrays) before
         * serialization, and invalid data cannot replace a valid live
         * document. */
        status = device_doc_strings_valid(
            (const app_config_device_doc_t *)doc);
    }

    if (status == APP_CONFIG_OK)
    {
        status =
            serialize_kind(kind, doc, json, APP_CONFIG_WORK_BUF_BYTES)
                ? APP_CONFIG_OK
                : APP_CONFIG_ERR_IO;
    }

    if (status == APP_CONFIG_OK)
    {
        status = validate_kind(kind, json, NULL);
    }
    if (status == APP_CONFIG_OK)
    {
        status = commit_json(kind, json);
    }

    free(json);
    return status;
}

/* --------------------------------------------------------------------- */
/* Load machinery (with last-known-good recovery)                         */
/* --------------------------------------------------------------------- */

/** @brief Size in bytes of a kind's typed document struct (0 = invalid). */
static size_t doc_size_for_kind(app_config_doc_kind_t kind)
{
    switch (kind)
    {
        case APP_CONFIG_DOC_DEVICE:
            return sizeof(app_config_device_doc_t);
        case APP_CONFIG_DOC_MANUFACTURING:
            return sizeof(app_config_manufacturing_doc_t);
        default:
            return 0U;
    }
}

/**
 * @brief Load and validate one specific path (live or last-known-good).
 *
 * Legacy schema versions migrate through the registered handler before
 * validation; the migrated text must itself validate against the current
 * schema before it is materialized into @p doc.
 *
 * On success, @p out_json receives the exact text that validated and was
 * materialized: the original file text for current-schema documents, or the
 * migrated current-schema text for legacy documents.  Callers must use this
 * text for any follow-up commit (e.g. recovery) so the committed bytes are
 * always the validated bytes.
 *
 * @param[out] doc      Typed document receiving the validated content.
 *                      Written only when validation succeeds.
 * @param[out] out_json Buffer for the validated text (may be NULL when the
 *                      caller does not need it).
 * @param[in]  out_cap  Capacity of @p out_json in bytes.
 */
static app_config_status_t load_from_path(app_config_doc_kind_t kind,
                                          const char *path,
                                          char *buf,
                                          size_t buf_cap,
                                          char *mig,
                                          size_t mig_cap,
                                          void *doc,
                                          char *out_json,
                                          size_t out_cap)
{
    size_t len = 0U;
    app_config_status_t status =
        read_whole_file(path, buf, buf_cap, &len);
    if (status != APP_CONFIG_OK)
    {
        return status;
    }

    const char *validated = buf;

    status = validate_kind(kind, buf, doc);
    if (status == APP_CONFIG_ERR_MIGRATION)
    {
        uint32_t from_version = 0U;
        app_config_status_t v = extract_schema_version(buf, &from_version);
        if (v != APP_CONFIG_OK)
        {
            return v;
        }

        status = migrate_json(kind, buf, from_version, mig, mig_cap);
        if (status == APP_CONFIG_OK)
        {
            status = validate_kind(kind, mig, doc);
            if (status == APP_CONFIG_OK)
            {
                /* The migrated (current-schema) text is the validated
                 * text; the original legacy text must never be committed
                 * or materialized. */
                validated = mig;
            }
        }
    }

    if ((status == APP_CONFIG_OK) && (out_json != NULL))
    {
        const size_t text_len = strlen(validated);
        if (text_len < out_cap)
        {
            memcpy(out_json, validated, text_len + 1U);
        }
        else
        {
            status = APP_CONFIG_ERR_IO;
        }
    }

    return status;
}

/**
 * @brief Shared load implementation with recovery.
 *
 * Live file first; on any failure the last-known-good copy is validated
 * and, only when it passes, restored through the safe commit path.  The
 * restored bytes are exactly the bytes that validated (including migrated
 * text for legacy-schema backups), never the raw backup contents.
 */
static app_config_status_t load_kind(app_config_doc_kind_t kind, void *doc)
{
    if (doc == NULL)
    {
        return APP_CONFIG_ERR_INVALID_ARGUMENT;
    }
    if (app_config_doc_path(kind) == NULL)
    {
        return APP_CONFIG_ERR_INVALID_ARGUMENT;
    }

    char *buf = malloc(APP_CONFIG_WORK_BUF_BYTES);
    char *mig = malloc(APP_CONFIG_WORK_BUF_BYTES);
    char *validated = malloc(APP_CONFIG_WORK_BUF_BYTES);
    if ((buf == NULL) || (mig == NULL) || (validated == NULL))
    {
        free(buf);
        free(mig);
        free(validated);
        return APP_CONFIG_ERR_IO;
    }

    /* Recovery materializes the backup into a temporary document: the
     * caller's output is only touched after the restored live file has been
     * atomically committed, so a failed recovery leaves @p doc untouched
     * (the API contract: doc is written only on success / recovery). */
    void *recovered = malloc(doc_size_for_kind(kind));
    if (recovered == NULL)
    {
        free(buf);
        free(mig);
        free(validated);
        return APP_CONFIG_ERR_IO;
    }

    const char *live_path = app_config_doc_path(kind);
    char good_path[OSAL_MAX_PATH_LEN];
    const bool have_good = build_variant_path(kind, APP_CONFIG_GOOD_SUFFIX,
                                              good_path, sizeof(good_path));

    app_config_status_t status = load_from_path(
        kind, live_path, buf, APP_CONFIG_WORK_BUF_BYTES, mig,
        APP_CONFIG_WORK_BUF_BYTES, doc, NULL, 0U);

    if ((status != APP_CONFIG_OK) && have_good)
    {
        /* Recovery source: the last-known-good copy, used only after it
         * validates.  @p validated receives the exact text that validated
         * (the migrated text when the backup has a legacy schema); the
         * temporary recovery document receives the materialized content. */
        const app_config_status_t good_status = load_from_path(
            kind, good_path, buf, APP_CONFIG_WORK_BUF_BYTES, mig,
            APP_CONFIG_WORK_BUF_BYTES, recovered, validated,
            APP_CONFIG_WORK_BUF_BYTES);

        if (good_status == APP_CONFIG_OK)
        {
            /* Restore the live file from the validated backup.  The bytes
             * are exactly the content that just validated. */
            const app_config_status_t commit_status =
                commit_json(kind, validated);
            if (commit_status == APP_CONFIG_OK)
            {
                /* Only now that the backup is atomically restored is the
                 * validated document published to the caller. */
                memcpy(doc, recovered, doc_size_for_kind(kind));
                status = APP_CONFIG_OK_RECOVERED;
            }
            else
            {
                status = commit_status;
            }
        }
        /* Both copies unusable: report the live-file failure unchanged; no
         * fabricated data is ever returned. */
    }

    free(recovered);
    free(buf);
    free(mig);
    free(validated);
    return status;
}

/* --------------------------------------------------------------------- */
/* Public API: load / commit / migrate                                    */
/* --------------------------------------------------------------------- */

app_config_status_t app_config_load_device(app_config_device_doc_t *doc)
{
    return load_kind(APP_CONFIG_DOC_DEVICE, doc);
}

app_config_status_t app_config_load_manufacturing(
    app_config_manufacturing_doc_t *doc)
{
    return load_kind(APP_CONFIG_DOC_MANUFACTURING, doc);
}

app_config_status_t app_config_commit_device(
    const app_config_device_doc_t *doc)
{
    if (doc == NULL)
    {
        return APP_CONFIG_ERR_INVALID_ARGUMENT;
    }

    const app_config_status_t v =
        check_commit_version(APP_CONFIG_DOC_DEVICE, doc->schema_version);
    if (v != APP_CONFIG_OK)
    {
        return v;
    }

    return commit_kind(APP_CONFIG_DOC_DEVICE, doc);
}

app_config_status_t app_config_commit_manufacturing(
    const app_config_manufacturing_doc_t *doc)
{
    if (doc == NULL)
    {
        return APP_CONFIG_ERR_INVALID_ARGUMENT;
    }

    const app_config_status_t v = check_commit_version(
        APP_CONFIG_DOC_MANUFACTURING, doc->schema_version);
    if (v != APP_CONFIG_OK)
    {
        return v;
    }

    return commit_kind(APP_CONFIG_DOC_MANUFACTURING, doc);
}

app_config_status_t app_config_migrate_stored(app_config_doc_kind_t kind)
{
    if (app_config_doc_path(kind) == NULL)
    {
        return APP_CONFIG_ERR_INVALID_ARGUMENT;
    }

    char *buf = malloc(APP_CONFIG_WORK_BUF_BYTES);
    if (buf == NULL)
    {
        return APP_CONFIG_ERR_IO;
    }

    size_t len = 0U;
    app_config_status_t status =
        read_whole_file(app_config_doc_path(kind), buf,
                        APP_CONFIG_WORK_BUF_BYTES, &len);
    if (status == APP_CONFIG_OK)
    {
        status = validate_kind(kind, buf, NULL);
        if (status == APP_CONFIG_ERR_MIGRATION)
        {
            uint32_t from_version = 0U;
            app_config_status_t v = extract_schema_version(buf, &from_version);
            if (v != APP_CONFIG_OK)
            {
                status = v;
            }
            else
            {
                char *mig = malloc(APP_CONFIG_WORK_BUF_BYTES);
                if (mig == NULL)
                {
                    status = APP_CONFIG_ERR_IO;
                }
                else
                {
                    status = migrate_json(kind, buf, from_version, mig,
                                          APP_CONFIG_WORK_BUF_BYTES);
                    if (status == APP_CONFIG_OK)
                    {
                        status = commit_json(kind, mig);
                    }
                    free(mig);
                }
            }
        }
        /* Current-version documents pass through unchanged; newer or
         * broken documents surface their own error codes. */
    }

    free(buf);
    return status;
}

/* --------------------------------------------------------------------- */
/* Diagnostics: secret redaction                                          */
/* --------------------------------------------------------------------- */

/** @brief Secret-bearing member names (case-insensitive match). */
static const char *const SECRET_KEYS[] = {
    "token", "secret", "password", "psk", "key", "provisioning",
};

#define SECRET_KEY_COUNT (sizeof(SECRET_KEYS) / sizeof(SECRET_KEYS[0]))

/** @brief ASCII lower-case compare (locale-independent). */
static bool key_equals_nocase(const char *a, const char *b)
{
    while ((*a != '\0') && (*b != '\0'))
    {
        char ca = *a;
        char cb = *b;
        if ((ca >= 'A') && (ca <= 'Z'))
        {
            ca = (char)(ca - 'A' + 'a');
        }
        if ((cb >= 'A') && (cb <= 'Z'))
        {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb)
        {
            return false;
        }
        ++a;
        ++b;
    }
    return (*a == '\0') && (*b == '\0');
}

static bool key_is_secret(const char *key)
{
    for (size_t i = 0U; i < (sizeof(SECRET_KEYS) / sizeof(SECRET_KEYS[0]));
         ++i)
    {
        if (key_equals_nocase(key, SECRET_KEYS[i]))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Recursively redact secret-bearing members.
 *
 * A member whose key is secret-bearing has its ENTIRE value replaced with
 * a single redacted string, whatever the value's type (string, number,
 * boolean, object or array), so no descendant of a secret key — and no
 * non-string secret value — can survive into diagnostics.  Recursion only
 * continues into container members whose own keys are not secret-bearing.
 *
 * @param parent Object or array whose children are processed.
 */
static void redact_children(cJSON *parent)
{
    cJSON *child = parent->child;

    while (child != NULL)
    {
        /* Capture the sibling first: replacement frees @p child. */
        cJSON *next = child->next;

        if ((child->string != NULL) && key_is_secret(child->string))
        {
            /* Replace the whole value regardless of its type. */
            cJSON *redacted = cJSON_CreateString(APP_CONFIG_REDACTED);
            if (redacted != NULL)
            {
                if (cJSON_IsObject(parent))
                {
                    (void)cJSON_ReplaceItemInObjectCaseSensitive(
                        parent, child->string, redacted);
                }
                else
                {
                    cJSON_Delete(redacted);
                    /* Array elements have no key of their own: their
                     * parent's key governs, which was already handled at
                     * the level above, so nothing to do here. */
                }
            }
        }
        else if (cJSON_IsObject(child) || cJSON_IsArray(child))
        {
            redact_children(child);
        }

        child = next;
    }
}

size_t app_config_redact(const char *json, char *out, size_t out_cap)
{
    if ((json == NULL) || (out == NULL) || (out_cap == 0U))
    {
        return 0U;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL)
    {
        const size_t n = strlen(APP_CONFIG_UNPARSABLE);
        if (n >= out_cap)
        {
            return 0U;
        }
        memcpy(out, APP_CONFIG_UNPARSABLE, n + 1U);
        return n;
    }

    redact_children(root);

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (printed == NULL)
    {
        return 0U;
    }

    const size_t n = strlen(printed);
    if (n >= out_cap)
    {
        free(printed);
        return 0U;
    }

    memcpy(out, printed, n + 1U);
    free(printed);
    return n;
}

