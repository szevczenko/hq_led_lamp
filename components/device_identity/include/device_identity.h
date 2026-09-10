/**
 * @file device_identity.h
 * @brief Access-token device identity (TASK-111)
 *
 * Normative public API of the product-owned identity owner.  The module
 * provides the ThingsBoard access token and the stable MQTT client ID that
 * authenticate the device to the ThingsBoard tenant.
 *
 * Authentication model
 * --------------------
 * Initial authentication is verified server TLS (owned by mqtt_cfg,
 * TASK-110) plus a per-device ThingsBoard access token and a stable client
 * ID.  This component owns the credential half of that model:
 *
 *   - the access token (the ThingsBoard device credential, used as the MQTT
 *     username),
 *   - the stable client ID (the MQTT client identifier, kept identical to
 *     the one validated in /config/mqtt.json by the manufacturing flow).
 *
 * Missing or invalid identity is FATAL for ThingsBoard initialization:
 * this module never falls back to an anonymous session, and callers must
 * not start the ThingsBoard client without a successful
 * device_identity_load().  The load path itself forces the lamp output
 * inactive on every failure (fail-off, mirroring mqtt_cfg), so an invalid
 * identity can never leave the output enabled.
 *
 * Identity record (/config/identity.json, schema v1)
 * --------------------------------------------------
 * The provisioning secret is deliberately NOT stored in the app_config
 * managed documents (manufacturing.json explicitly never carries secrets).
 * This component owns its own bounded, strictly-schema'd record:
 *
 *     {
 *       "schema_version": 1,
 *       "client_id": "klc-kitchen-01",
 *       "access_token": "YOUR_THINGSBOARD_ACCESS_TOKEN"
 *     }
 *
 * Unknown or duplicate members are rejected (strict schema), so no policy
 * override or extra secret can be smuggled into the record.  All string
 * input is bounded and charset-restricted:
 *
 *   - client_id:     1..DEVICE_IDENTITY_CLIENT_ID_MAX_LEN printable ASCII,
 *   - access_token:  1..DEVICE_IDENTITY_TOKEN_MAX_LEN printable ASCII
 *                    without whitespace (ThingsBoard tokens are base64-ish
 *                    bearer values; spaces/newlines/control characters are
 *                    rejected).
 *
 * The token is a bearer credential: it is never logged, never echoed in a
 * status, and every temporary copy made during loading (the raw record
 * buffer and the JSON parser's value copy) is zeroized before it is freed.
 * After a successful load exactly one copy remains, in module-owned
 * storage, and device_identity_clear() zeroizes it.
 *
 * Manufacturing/configuration validation gate
 * -------------------------------------------
 * The load path only trusts validated manufacturing/configuration records:
 * it validates the manufacturing document through app_config and refuses
 * identity unless the device is provisioned with an enrolled credential
 * mode (manufacturing_state == PROVISIONED and credential_mode != NONE).
 * An unprovisioned, quarantined or credential-less device reports
 * DEVICE_IDENTITY_ERR_UNPROVISIONED and never receives a token.
 *
 * Host testability
 * ----------------
 * The module speaks only the portable OSAL file API (osal_file.h), cJSON,
 * the app_config service and lamp_control; host tests compile it against
 * the real POSIX OSAL + LittleFS, the real app_config, a lamp-control
 * double and the Unity framework (see tests/device_identity).  It never
 * references ESP-IDF VFS, sockets or TLS symbols directly.
 */

#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Schema version and bounds                                              */
/* --------------------------------------------------------------------- */

/** @brief Current identity record schema version. */
#define DEVICE_IDENTITY_SCHEMA_VERSION 1U

/** @brief Maximum on-storage record size in bytes (file bound). */
#define DEVICE_IDENTITY_MAX_FILE_BYTES 2048U

/** @brief Maximum stable client ID length (excluding terminator). */
#define DEVICE_IDENTITY_CLIENT_ID_MAX_LEN 64U

/**
 * @brief Maximum ThingsBoard access-token length (excluding terminator).
 *
 * Matches the platform ThingsBoard client's credential-field bound
 * (TB_CLIENT_CONFIG_STR_SIZE == 128) so any accepted token fits the client
 * configuration verbatim.
 */
#define DEVICE_IDENTITY_TOKEN_MAX_LEN 128U

/** @brief Logical path of the identity record. */
#define DEVICE_IDENTITY_FILE_PATH "/config/identity.json"

/* --------------------------------------------------------------------- */
/* Status type                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Result type for the device-identity service.
 *
 * #DEVICE_IDENTITY_OK (0) is the only success code.
 */
typedef enum device_identity_status {
    DEVICE_IDENTITY_OK                       =  0, /**< Success. */
    DEVICE_IDENTITY_ERR_INVALID_ARGUMENT     = -1, /**< NULL/invalid argument. */
    DEVICE_IDENTITY_ERR_IO                   = -2, /**< OSAL file operation failed. */
    DEVICE_IDENTITY_ERR_NOT_FOUND            = -3, /**< Identity record does not exist. */
    DEVICE_IDENTITY_ERR_MALFORMED            = -4, /**< Bad JSON, missing/wrong-typed/
                                                        unknown/duplicate members, or a
                                                        charset-invalid value. */
    DEVICE_IDENTITY_ERR_BOUNDS               = -5, /**< File or field size bound violated. */
    DEVICE_IDENTITY_ERR_UNKNOWN_SCHEMA       = -6, /**< schema_version newer than supported. */
    DEVICE_IDENTITY_ERR_UNPROVISIONED        = -7, /**< Manufacturing record missing or not
                                                        provisioned / no credential mode. */
    DEVICE_IDENTITY_ERR_EMPTY_CLIENT_ID      = -8, /**< client_id member present but empty. */
    DEVICE_IDENTITY_ERR_EMPTY_TOKEN          = -9, /**< access_token member present but empty. */
    DEVICE_IDENTITY_ERR_NOT_LOADED           = -10 /**< Getter/provider queried before a
                                                        successful load. */
} device_identity_status_t;

/**
 * @brief Human-readable name of a status code (never NULL).
 *
 * Names carry no record content (in particular never the token), so they
 * are safe to log on failure paths.
 */
const char *device_identity_status_name(device_identity_status_t status);

/* --------------------------------------------------------------------- */
/* Token-provider interface                                               */
/* --------------------------------------------------------------------- */

/**
 * @brief Token-provider callback.
 *
 * Zero-argument accessor for the validated identity; returns the
 * module-owned ThingsBoard access token, or NULL when no identity has been
 * loaded (or after device_identity_clear()).
 */
typedef const char *(*device_identity_token_provider_t)(void);

/* --------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* --------------------------------------------------------------------- */

/**
 * @brief Load and validate the device identity.
 *
 * Ordering contract: runs after the filesystem bootstrap and the product
 * configuration validation, and before any ThingsBoard initialization.
 * The load path:
 *
 *   1. validates the manufacturing record through app_config and requires
 *      manufacturing_state == PROVISIONED and credential_mode != NONE
 *      ("load only validated manufacturing/configuration records"),
 *   2. reads the bounded identity record /config/identity.json,
 *   3. parses it with the strict v1 schema (whole input consumed, unknown
 *      and duplicate members rejected) and validates every field against
 *      the documented bounds and charsets,
 *   4. publishes the validated client ID and token into module-owned
 *      storage, zeroizing every temporary secret-bearing copy (the raw
 *      record buffer and the parser's value copy) before it is freed,
 *   5. on ANY failure forces the lamp output inactive
 *      (lamp_control_force_inactive(), fail-off) and leaves the module
 *      unloaded: no anonymous/plaintext fallback is ever possible.
 *
 * A successful load does NOT release the lamp fail-off barrier — the
 * single re-enable transition stays owned by the verified MQTT/TLS
 * connect (mqtt_cfg_connect(), TASK-110).
 *
 * Calling load again re-validates and replaces the module state.
 *
 * @return #DEVICE_IDENTITY_OK on success, otherwise the rejection reason.
 */
device_identity_status_t device_identity_load(void);

/**
 * @brief Has a valid identity been loaded and not yet cleared?
 */
bool device_identity_is_loaded(void);

/**
 * @brief Invalidate the module state and zeroize the stored token.
 *
 * Safe to call at any time; getters return #DEVICE_IDENTITY_ERR_NOT_LOADED
 * afterwards.  Intended for lifecycle end / factory-reset paths.
 */
void device_identity_clear(void);

/* --------------------------------------------------------------------- */
/* Stable client-ID and token access                                     */
/* --------------------------------------------------------------------- */

/**
 * @brief Get the validated stable client ID.
 *
 * The returned pointer references module-owned storage that stays valid
 * until the next device_identity_load()/device_identity_clear() call.
 *
 * @param[out] out_client_id Receives the module-owned client ID (non-NULL
 *                           on success).
 *
 * @return #DEVICE_IDENTITY_OK and a non-NULL @p *out_client_id, or
 *         #DEVICE_IDENTITY_ERR_INVALID_ARGUMENT /
 *         #DEVICE_IDENTITY_ERR_NOT_LOADED.
 */
device_identity_status_t device_identity_client_id(const char **out_client_id);

/**
 * @brief Get the validated ThingsBoard access token.
 *
 * The returned pointer references module-owned storage that stays valid
 * until the next device_identity_load()/device_identity_clear() call.
 * The token is a bearer credential: callers must not log it, and should
 * copy it into the consumer (e.g. the ThingsBoard client configuration)
 * and drop the reference as soon as the consumer owns the value.
 *
 * @param[out] out_token Receives the module-owned token (non-NULL on
 *                       success).
 *
 * @return #DEVICE_IDENTITY_OK and a non-NULL @p *out_token, or
 *         #DEVICE_IDENTITY_ERR_INVALID_ARGUMENT /
 *         #DEVICE_IDENTITY_ERR_NOT_LOADED.
 */
device_identity_status_t device_identity_token(const char **out_token);

/**
 * @brief Get the token-provider callback.
 *
 * The returned callback returns the module-owned token when an identity is
 * loaded and NULL otherwise — a consumer that invokes the provider always
 * receives the validated value or a clear "not available" instead of a
 * fallback.
 */
device_identity_token_provider_t device_identity_token_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_IDENTITY_H */