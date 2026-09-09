/**
 * @file app_config.h
 * @brief Versioned product configuration service (TASK-108)
 *
 * Normative public API for validated, recoverable product configuration.
 *
 * Scope
 * -----
 * This component owns the two product-owned documents
 *
 *     /config/device.json         identity (TASK-108 schema v1)
 *     /config/manufacturing.json  manufacturing state + credential mode
 *
 * It provides bounded schemas, atomic replacement, last-known-good
 * recovery, explicit migration hooks and secret redaction for diagnostics.
 *
 * Non-goals (release 1)
 * ---------------------
 * - Wi-Fi configuration stays behind the Wi-Fi storage adapter and is NOT
 *   handled here.
 * - Lamp state is NEVER persisted.  ThingsBoard is the desired-state
 *   authority; there is deliberately no API in this component that could
 *   write lamp state to storage.
 * - Certificates and keys live in /cert and are owned by the provisioning
 *   flow, not by this component.  Manufacturing documents must never
 *   contain secrets (unknown keys are rejected so a secret cannot be
 *   smuggled into a managed document).
 *
 * Storage contract (normative)
 * ----------------------------
 * Every commit (app_config_commit_*) performs, in order:
 *
 *   1. validate the document against the bounded schema,
 *   2. serialize it and write it to `<live>.tmp` (temporary data),
 *   3. flush/close the temporary file (osal_close is the durability point),
 *   4. read the temporary file back and re-validate it; any mismatch or
 *      parse failure aborts and removes the temporary file — the live file
 *      is never touched by an aborted commit,
 *   5. if a live file exists and validates, stage the last-known-good copy
 *      safely: copy it to `<live>.good.tmp`, read the staging copy back and
 *      validate it, then atomically replace `<live>.good` with it.  Any
 *      staging, validation or replacement failure leaves the previous
 *      `<live>.good` untouched (last-known-good),
 *   6. atomically replace the live file with the temporary file
 *      (osal_rename).  When the OSAL backend does not support rename
 *      (OSAL_ERR_OPERATION_NOT_SUPPORTED / OSAL_ERR_NOT_IMPLEMENTED), the
 *      commit is refused with APP_CONFIG_ERR_UNSUPPORTED and the live file
 *      (and its last-known-good copy) remain unchanged: a copy-then-remove
 *      fallback could truncate a valid document if interrupted, so a
 *      non-atomic replacement path is never used.
 *
 * On load (app_config_load_*), a missing or invalid live document is
 * recovered from `<live>.good`; recovery uses only a validated backup, and
 * the recovered content is re-committed through the safe path before
 * APP_CONFIG_OK_RECOVERED is reported.  If both copies are unusable the
 * error is reported and no fabricated data is ever returned.
 *
 * Schema versioning
 * -----------------
 * Every product-owned document carries an integral `schema_version`.
 * - missing -> APP_CONFIG_ERR_MALFORMED,
 * - greater than the current version -> APP_CONFIG_ERR_UNKNOWN_SCHEMA,
 * - smaller than the current version -> passed to the migration handler
 *   registered with app_config_register_migration_handler(); without a
 *   handler APP_CONFIG_ERR_MIGRATION is reported.  app_config_migrate_
 *   stored() is the explicit on-storage migration entry point.
 *
 * Security
 * --------
 * - Field values are bounded and charset-restricted; unknown object keys
 *   are rejected (strict schemas), so no secret can be stored in a managed
 *   document.
 * - app_config_redact() masks the values of secret-bearing keys so raw
 *   documents captured for diagnostics never leak credentials.
 *
 * Host testability
 * ----------------
 * The component speaks only the portable OSAL file API (osal_file.h) and
 * cJSON; host tests exercise it against an in-memory OSAL filesystem double
 * (see tests/app_config).  It never references ESP-IDF VFS or flash
 * symbols directly.
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Schema version and bounds                                              */
/* --------------------------------------------------------------------- */

/** @brief Current product configuration schema version. */
#define APP_CONFIG_SCHEMA_VERSION 1U

/** @brief Oldest schema version accepted through the migration hook. */
#define APP_CONFIG_OLDEST_SCHEMA_VERSION 0U

/** @brief Maximum on-storage document size in bytes (file bound). */
#define APP_CONFIG_MAX_FILE_BYTES 2048U

/** @brief Maximum length of the product name (excluding terminator). */
#define APP_CONFIG_PRODUCT_MAX_LEN 32U

/** @brief Maximum length of the hardware revision (excluding terminator). */
#define APP_CONFIG_HARDWARE_REVISION_MAX_LEN 16U

/** @brief Maximum length of the serial number (excluding terminator). */
#define APP_CONFIG_SERIAL_MAX_LEN 32U

/** @brief Maximum length of the ThingsBoard device name (excl. terminator). */
#define APP_CONFIG_THINGSBOARD_NAME_MAX_LEN 64U

/** @brief Logical path of the device document. */
#define APP_CONFIG_DEVICE_PATH "/config/device.json"

/** @brief Logical path of the manufacturing document. */
#define APP_CONFIG_MANUFACTURING_PATH "/config/manufacturing.json"

/* --------------------------------------------------------------------- */
/* Status type                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Result type for the configuration service.
 *
 * #APP_CONFIG_OK (0) and #APP_CONFIG_OK_RECOVERED (1) are success codes;
 * every other value is an error.
 */
typedef enum app_config_status {
    APP_CONFIG_OK                    = 0,  /**< Success. */
    APP_CONFIG_OK_RECOVERED          = 1,  /**< Success; live file was restored
                                                from the last-known-good copy. */
    APP_CONFIG_ERR_INVALID_ARGUMENT  = -1, /**< NULL/invalid argument. */
    APP_CONFIG_ERR_IO                = -2, /**< OSAL file operation failed; the
                                                live file was not modified. */
    APP_CONFIG_ERR_NOT_FOUND         = -3, /**< Document does not exist. */
    APP_CONFIG_ERR_MALFORMED         = -4, /**< Truncated/malformed JSON, missing
                                                or wrong-typed fields, unknown
                                                or duplicate keys. */
    APP_CONFIG_ERR_BOUNDS            = -5, /**< File or field size bound violated. */
    APP_CONFIG_ERR_UNKNOWN_SCHEMA    = -6, /**< schema_version is newer than the
                                                implementation supports. */
    APP_CONFIG_ERR_MIGRATION         = -7, /**< Migration required but no handler
                                                registered, or the handler
                                                failed. */
    APP_CONFIG_ERR_UNSUPPORTED       = -8  /**< The OSAL backend does not
                                                support atomic rename; the
                                                commit was refused and
                                                nothing was written. */
} app_config_status_t;

/**
 * @brief Human-readable name of a status code (never NULL).
 */
const char *app_config_status_name(app_config_status_t status);

/* --------------------------------------------------------------------- */
/* Document kinds                                                         */
/* --------------------------------------------------------------------- */

/** @brief Identifies one of the product-owned documents. */
typedef enum app_config_doc_kind {
    APP_CONFIG_DOC_DEVICE         = 0, /**< /config/device.json. */
    APP_CONFIG_DOC_MANUFACTURING  = 1  /**< /config/manufacturing.json. */
} app_config_doc_kind_t;

/**
 * @brief Live storage path for a document kind.
 *
 * @param[in] kind Document kind.
 *
 * @return Logical OSAL path of the live document, or NULL for an invalid
 *         kind.  The temporary and last-known-good copies are this path
 *         with ".tmp" / ".good" / ".good.tmp" appended.
 */
const char *app_config_doc_path(app_config_doc_kind_t kind);

/* --------------------------------------------------------------------- */
/* Bounded schemas                                                        */
/* --------------------------------------------------------------------- */

/**
 * @brief Manufacturing state recorded in /config/manufacturing.json.
 */
typedef enum app_config_mfg_state {
    APP_CONFIG_MFG_STATE_UNPROVISIONED = 0, /**< Out of the factory line. */
    APP_CONFIG_MFG_STATE_PROVISIONED   = 1, /**< Credentials enrolled. */
    APP_CONFIG_MFG_STATE_QUARANTINED   = 2  /**< Held, needs re-inspection. */
} app_config_mfg_state_t;

/**
 * @brief Credential mode recorded in /config/manufacturing.json.
 *
 * The provisioning secret itself is NEVER stored in the document.
 */
typedef enum app_config_credential_mode {
    APP_CONFIG_CRED_MODE_NONE = 0, /**< No credential provisioned. */
    APP_CONFIG_CRED_MODE_PSK  = 1, /**< Pre-shared key held by the platform. */
    APP_CONFIG_CRED_MODE_MTLS = 2  /**< Device certificate + private key. */
} app_config_credential_mode_t;

/**
 * @brief Validated device identity document (device.json schema v1).
 *
 * All strings are NUL-terminated and bounded; the storage form is
 * `{"schema_version":1,"product":...,"hardware_revision":...,"serial":...,
 * "thingsboard_name":...}` with no additional members.
 */
typedef struct app_config_device_doc {
    uint32_t schema_version;                                /**< Always #APP_CONFIG_SCHEMA_VERSION when stored. */
    char     product[APP_CONFIG_PRODUCT_MAX_LEN + 1U];              /**< Product name. */
    char     hardware_revision[APP_CONFIG_HARDWARE_REVISION_MAX_LEN + 1U]; /**< Hardware revision. */
    char     serial[APP_CONFIG_SERIAL_MAX_LEN + 1U];        /**< Serial number. */
    char     thingsboard_name[APP_CONFIG_THINGSBOARD_NAME_MAX_LEN + 1U]; /**< ThingsBoard device name. */
} app_config_device_doc_t;

/**
 * @brief Validated manufacturing document (manufacturing.json schema v1).
 *
 * Storage form is `{"schema_version":1,"manufacturing_state":...,
 * "credential_mode":...}` with no additional members and no secrets.
 */
typedef struct app_config_manufacturing_doc {
    uint32_t schema_version;                    /**< #APP_CONFIG_SCHEMA_VERSION when stored. */
    app_config_mfg_state_t manufacturing_state; /**< Manufacturing state. */
    app_config_credential_mode_t credential_mode; /**< Credential mode. */
} app_config_manufacturing_doc_t;

/* --------------------------------------------------------------------- */
/* Validation                                                             */
/* --------------------------------------------------------------------- */

/**
 * @brief Validate a serialized device document against the bounded schema.
 *
 * Accepts only a JSON object with exactly the five known members,
 * well-formed and within bounds (see the header top for the bound rules).
 * On success the parsed document is copied into @p doc.
 *
 * @param[in]  json       NUL-terminated JSON text.
 * @param[out] doc        Parsed document (may be NULL to validate only).
 *
 * @return #APP_CONFIG_OK, #APP_CONFIG_ERR_INVALID_ARGUMENT,
 *         #APP_CONFIG_ERR_BOUNDS, #APP_CONFIG_ERR_MALFORMED,
 *         #APP_CONFIG_ERR_UNKNOWN_SCHEMA or #APP_CONFIG_ERR_MIGRATION
 *         (a legacy version requires a registered handler; this call never
 *         migrates, it only accepts the current version).
 */
app_config_status_t app_config_validate_device_json(const char *json,
                                                    app_config_device_doc_t *doc);

/**
 * @brief Validate a serialized manufacturing document (see above).
 */
app_config_status_t app_config_validate_manufacturing_json(
    const char *json, app_config_manufacturing_doc_t *doc);

/* --------------------------------------------------------------------- */
/* Migration                                                              */
/* --------------------------------------------------------------------- */

/**
 * @brief Migration handler invoked for documents older than the current
 *        schema version.
 *
 * Implementations must serialize the migrated document into @p out_json
 * (NUL-terminated, at most @p out_cap bytes including the terminator) at
 * the CURRENT schema version.  The result is re-validated by the service
 * before it is used; a handler may not emit schema versions above the
 * current one and must not introduce secret-bearing fields.
 *
 * @param[in]  json_text   Raw stored document text.
 * @param[in]  from_version schema_version found in @p json_text.
 * @param[out] out_json     Buffer for the migrated document.
 * @param[in]  out_cap      Capacity of @p out_json in bytes.
 *
 * @return #APP_CONFIG_OK on success, any other status on failure.
 */
typedef app_config_status_t (*app_config_migrate_fn_t)(const char *json_text,
                                                       uint32_t from_version,
                                                       char *out_json,
                                                       size_t out_cap);

/**
 * @brief Register the migration hook (explicit migration entry point).
 *
 * @param[in] handler Handler invoked for schema_version values in
 *                    [#APP_CONFIG_OLDEST_SCHEMA_VERSION,
 *                    #APP_CONFIG_SCHEMA_VERSION); NULL unregisters.
 */
void app_config_register_migration_handler(app_config_migrate_fn_t handler);

/**
 * @brief Explicitly migrate a stored document to the current schema.
 *
 * Reads the live document, and — when it carries an older schema version —
 * runs the registered migration handler, validates the result and replaces
 * the live document through the full safe commit path (temporary file,
 * read-back validation, last-known-good update, atomic replace).
 *
 * @param[in] kind Document kind to migrate.
 *
 * @return
 *  - #APP_CONFIG_OK when the stored document already has the current
 *    schema (after validation) or was migrated successfully,
 *  - #APP_CONFIG_ERR_NOT_FOUND when no live document exists,
 *  - #APP_CONFIG_ERR_MIGRATION / #APP_CONFIG_ERR_UNKNOWN_SCHEMA when the
 *    document cannot be migrated,
 *  - validation or I/O errors otherwise.
 */
app_config_status_t app_config_migrate_stored(app_config_doc_kind_t kind);

/* --------------------------------------------------------------------- */
/* Load with recovery                                                     */
/* --------------------------------------------------------------------- */

/**
 * @brief Load and validate the device document.
 *
 * Reads /config/device.json and validates it (legacy versions migrate
 * through the registered hook).  If the live document is missing or
 * invalid, the last-known-good copy is validated and, when it passes, is
 * re-committed to the live path; #APP_CONFIG_OK_RECOVERED is then returned.
 * A document that failed validation is never reported as loaded.
 *
 * @param[out] doc Parsed document.
 *
 * @return #APP_CONFIG_OK, #APP_CONFIG_OK_RECOVERED, or an error code.
 *         @p doc is only written on success/recovery.
 */
app_config_status_t app_config_load_device(app_config_device_doc_t *doc);

/**
 * @brief Load and validate the manufacturing document (see above).
 */
app_config_status_t app_config_load_manufacturing(
    app_config_manufacturing_doc_t *doc);

/* --------------------------------------------------------------------- */
/* Commit (validated, atomic)                                             */
/* --------------------------------------------------------------------- */

/**
 * @brief Validate and atomically store the device document.
 *
 * Full safe path: schema validation, temporary write, flush/close,
 * read-back re-validation, last-known-good update, atomic replacement.
 * If any step fails the previous live document (and its last-known-good
 * copy) remain unchanged.
 *
 * @param[in] doc Document to store; schema_version must equal
 *                #APP_CONFIG_SCHEMA_VERSION.
 *
 * @return #APP_CONFIG_OK on success, otherwise the reason the commit was
 *         aborted (the live file was not modified).
 */
app_config_status_t app_config_commit_device(
    const app_config_device_doc_t *doc);

/**
 * @brief Validate and atomically store the manufacturing document.
 */
app_config_status_t app_config_commit_manufacturing(
    const app_config_manufacturing_doc_t *doc);

/* --------------------------------------------------------------------- */
/* Diagnostics (secret redaction)                                         */
/* --------------------------------------------------------------------- */

/**
 * @brief Redact secret-bearing values from a JSON document.
 *
 * Parses @p json and replaces the string value of every member whose key
 * is secret-bearing ("token", "secret", "password", "psk", "key",
 * "provisioning", case-insensitive) with "[redacted]", then prints the
 * document into @p out.  Unparsable input yields "[unparsable document]".
 * Never logs the input itself.
 *
 * @param[in]  json    Raw JSON text (NUL-terminated).
 * @param[out] out     Output buffer for the redacted document.
 * @param[in]  out_cap Capacity of @p out (including terminator).
 *
 * @return Number of bytes written (excluding the terminator); 0 when the
 *         arguments are invalid or the output does not fit.
 */
size_t app_config_redact(const char *json, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
