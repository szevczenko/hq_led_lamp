/**
 * @file mqtt_cfg.h
 * @brief MQTT/TLS broker configuration loader and verified-transport setup
 *        (TASK-110)
 *
 * Normative public API of the product-owned MQTT/TLS configuration owner.
 * The module:
 *
 *   1. loads the bounded, strictly-schema'd broker document
 *      /config/mqtt.json (hostname, port, client ID, TLS mode, certificate
 *      paths and authentication mode),
 *   2. validates it against the verified-TLS-only production policy
 *      (only `mqtts://` is accepted; `skip_verify` is rejected; the CA
 *      path is mandatory and must stay under the logical /cert directory),
 *   3. applies the accepted configuration through the existing
 *      Mongoose/ThingsBoard configuration API (mqtt_config.h) — never
 *      through direct sockets — so the transport always runs verified TLS
 *      with a private CA and DNS-hostname verification, and
 *   4. can drive one verified TLS connection through the Mongoose MQTT
 *      application layer (mqtt_app.h).
 *
 * Fail-off contract
 * -----------------
 * Every configuration, application or TLS-connection failure forces the
 * lamp output inactive through lamp_control_force_inactive() before the
 * error status is returned: TLS configuration failures can never enable
 * the output.  The operation is idempotent and safe to call while the
 * output is already off.
 *
 * The fail-off barrier is lifted only by a successful
 * mqtt_cfg_connect() — i.e. after a valid configuration was applied AND
 * the verified TLS connection succeeded.  A Wi-Fi connection alone or any
 * rejected configuration/connection keeps the output latched off.
 *
 * Logical paths and the LittleFS mount
 * ------------------------------------
 * The document stores logical OSAL paths (`/cert/ca.crt`, ...).  They are
 * validated against the /cert prefix and handed to mqtt_config verbatim;
 * the OSAL backend maps them consistently onto the LittleFS mount point
 * (e.g. `/littlefs/cert/ca.crt` on ESP).  The component never re-bases,
 * re-writes or accepts mount-point-prefixed paths, so a logical path
 * resolves to exactly one file wherever the volume is mounted.
 *
 * Host testability
 * ----------------
 * The module speaks only the portable OSAL file API, mqtt_config/mqtt_app
 * and lamp_control; host tests compile it against the real POSIX OSAL +
 * LittleFS, the real mqtt_config codec and doubles for mqtt_app and
 * lamp_control (see tests/mqtt_cfg).  It never references ESP-IDF VFS,
 * sockets or TLS stack symbols directly.
 *
 * Schema (v1)
 * -----------
 * /config/mqtt.json at schema_version 1:
 *
 *     {
 *       "schema_version": 1,
 *       "hostname": "thingsboard.home.arpa",
 *       "port": 8883,
 *       "tls_mode": "mqtts",
 *       "ca_path": "/cert/ca.crt",
 *       "client_cert_path": "/cert/device.crt",   // optional; required for mTLS
 *       "client_key_path": "/cert/device.key",    // optional; required for mTLS
 *       "client_id": "klc-kitchen-01",
 *       "auth_mode": "access_token",              // "none" | "access_token" | "mtls"
 *       "skip_verify": false                       // optional; true is rejected
 *     }
 *
 * Unknown or duplicate members are rejected (strict schema), so no secret
 * or policy override can be smuggled into the document.
 */

#ifndef MQTT_CFG_H
#define MQTT_CFG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Schema version and bounds                                              */
/* --------------------------------------------------------------------- */

/** @brief Current MQTT/TLS configuration schema version. */
#define MQTT_CFG_SCHEMA_VERSION 1U

/** @brief Maximum on-storage document size in bytes (file bound). */
#define MQTT_CFG_MAX_FILE_BYTES 2048U

/** @brief Maximum hostname length (excluding terminator). */
#define MQTT_CFG_HOSTNAME_MAX_LEN 128U

/** @brief Maximum client ID length (excluding terminator). */
#define MQTT_CFG_CLIENT_ID_MAX_LEN 64U

/** @brief Maximum certificate-path length (excluding terminator). */
#define MQTT_CFG_PATH_MAX_LEN 128U

/** @brief Minimum accepted broker port. */
#define MQTT_CFG_PORT_MIN 1U

/** @brief Maximum accepted broker port. */
#define MQTT_CFG_PORT_MAX 65535U

/** @brief Logical path of the broker/TLS document. */
#define MQTT_CFG_FILE_PATH "/config/mqtt.json"

/** @brief Logical certificate directory; every cert path must start here. */
#define MQTT_CFG_CERT_DIR "/cert"

/* --------------------------------------------------------------------- */
/* Status type                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Result type for the MQTT/TLS configuration service.
 *
 * #MQTT_CFG_OK (0) is the only success code.
 */
typedef enum mqtt_cfg_status {
    MQTT_CFG_OK                    = 0,   /**< Success. */
    MQTT_CFG_ERR_INVALID_ARGUMENT  = -1,  /**< NULL/invalid argument. */
    MQTT_CFG_ERR_IO                = -2,  /**< OSAL file operation failed. */
    MQTT_CFG_ERR_NOT_FOUND         = -3,  /**< /config/mqtt.json does not exist. */
    MQTT_CFG_ERR_MALFORMED         = -4,  /**< Bad JSON, wrong-typed, unknown or
                                               duplicate members. */
    MQTT_CFG_ERR_BOUNDS            = -5,  /**< File or field size/port bound violated. */
    MQTT_CFG_ERR_UNKNOWN_SCHEMA    = -6,  /**< schema_version newer than supported. */
    MQTT_CFG_ERR_PLAINTEXT         = -7,  /**< tls_mode is not "mqtts" (plaintext
                                               MQTT rejected). */
    MQTT_CFG_ERR_SKIP_VERIFY       = -8,  /**< skip_verify=true rejected. */
    MQTT_CFG_ERR_CA_PATH           = -9,  /**< CA path empty, outside /cert, or
                                               the CA file is missing/unreadable. */
    MQTT_CFG_ERR_CERT_PATH         = -10, /**< Client cert/key path invalid (mTLS). */
    MQTT_CFG_ERR_HOSTNAME          = -11, /**< Hostname empty, malformed, or an
                                               IP literal (a changing raw IP is
                                               never accepted). */
    MQTT_CFG_ERR_APPLY             = -12, /**< Transport configuration refused or
                                               self-check failed. */
    MQTT_CFG_ERR_CONNECT           = -13, /**< Verified TLS connection failed or
                                               timed out. */
    MQTT_CFG_ERR_NOT_APPLIED       = -14  /**< connect() called before a valid
                                               transport configuration. */
} mqtt_cfg_status_t;

/**
 * @brief Human-readable name of a status code (never NULL).
 */
const char *mqtt_cfg_status_name(mqtt_cfg_status_t status);

/* --------------------------------------------------------------------- */
/* Parsed configuration                                                   */
/* --------------------------------------------------------------------- */

/**
 * @brief Accepted TLS modes.
 *
 * Only #MQTT_CFG_TLS_MODE_MQTTS is valid in production; plaintext MQTT has
 * no representable value (it is rejected at load/validate time).
 */
typedef enum mqtt_cfg_tls_mode {
    MQTT_CFG_TLS_MODE_MQTTS = 1 /**< mqtts:// — verified TLS only. */
} mqtt_cfg_tls_mode_t;

/**
 * @brief Authentication mode recorded in mqtt.json.
 *
 * The credential itself is owned by the identity task (TASK-111); this
 * document only records which mode the transport must be configured for.
 */
typedef enum mqtt_cfg_auth_mode {
    MQTT_CFG_AUTH_NONE         = 0, /**< No client authentication (server TLS only). */
    MQTT_CFG_AUTH_ACCESS_TOKEN = 1, /**< ThingsBoard access token (MQTT username). */
    MQTT_CFG_AUTH_MTLS         = 2  /**< Device certificate + private key. */
} mqtt_cfg_auth_mode_t;

/**
 * @brief Validated broker/TLS configuration (mqtt.json schema v1).
 *
 * All strings are NUL-terminated and bounded; fields are filled only by
 * successful load/validation.
 */
typedef struct mqtt_cfg {
    uint32_t schema_version;             /**< Always #MQTT_CFG_SCHEMA_VERSION when stored. */
    char     hostname[MQTT_CFG_HOSTNAME_MAX_LEN + 1U]; /**< Broker DNS name (never an IP). */
    uint16_t port;                       /**< Broker MQTT/TLS port. */
    char     client_id[MQTT_CFG_CLIENT_ID_MAX_LEN + 1U]; /**< MQTT client ID. */
    mqtt_cfg_tls_mode_t tls_mode;        /**< Always #MQTT_CFG_TLS_MODE_MQTTS when accepted. */
    mqtt_cfg_auth_mode_t auth_mode;      /**< Recorded authentication mode. */
    char     ca_path[MQTT_CFG_PATH_MAX_LEN + 1U];        /**< Logical CA path under /cert. */
    char     client_cert_path[MQTT_CFG_PATH_MAX_LEN + 1U]; /**< Logical mTLS cert (optional). */
    char     client_key_path[MQTT_CFG_PATH_MAX_LEN + 1U];  /**< Logical mTLS key (optional). */
} mqtt_cfg_t;

/* --------------------------------------------------------------------- */
/* Validation                                                             */
/* --------------------------------------------------------------------- */

/**
 * @brief Validate a parsed broker/TLS configuration against the
 *        verified-TLS-only policy.
 *
 * Rules enforced:
 *   - hostname non-empty, bounded, DNS-shaped and never an IP literal,
 *   - port within [#MQTT_CFG_PORT_MIN, #MQTT_CFG_PORT_MAX],
 *   - TLS mode is exactly #MQTT_CFG_TLS_MODE_MQTTS (plaintext rejected),
 *   - CA path non-empty, bounded and under #MQTT_CFG_CERT_DIR,
 *   - client cert/key paths, when present, are bounded and under
 *     #MQTT_CFG_CERT_DIR; both are required for #MQTT_CFG_AUTH_MTLS,
 *   - client ID non-empty, bounded and printable ASCII.
 *
 * Note: a path that resolves to a missing file on the volume is only
 * detected by mqtt_cfg_apply() (file existence is a storage property, not
 * a document property).
 *
 * @param[in] cfg Parsed configuration (may be NULL to validate nothing).
 *
 * @return #MQTT_CFG_OK or the first policy error.
 */
mqtt_cfg_status_t mqtt_cfg_validate(const mqtt_cfg_t *cfg);

/* --------------------------------------------------------------------- */
/* Load / apply / connect                                                 */
/* --------------------------------------------------------------------- */

/**
 * @brief Load and validate /config/mqtt.json.
 *
 * Reads #MQTT_CFG_FILE_PATH, bounds-checks the file, parses it with the
 * strict v1 schema (unknown/duplicate members rejected), validates the
 * verified-TLS policy and copies the result into @p out.  On any failure
 * the lamp output is forced inactive and @p out is left untouched.
 *
 * @param[out] out Parsed and validated configuration.
 *
 * @return #MQTT_CFG_OK or an error code; @p out is only written on
 *         success.
 */
mqtt_cfg_status_t mqtt_cfg_load(mqtt_cfg_t *out);

/**
 * @brief Apply an accepted configuration to the Mongoose/ThingsBoard
 *        transport through the mqtt_config API.
 *
 * The accepted configuration always results in verified TLS:
 *   - address is set to `mqtts://<hostname>:<port>`,
 *   - SSL is enabled and skip-verify is explicitly disabled,
 *   - the CA is configured from the logical /cert path (the OSAL backend
 *     resolves it onto the LittleFS mount); a missing/unreadable CA file
 *     fails the application,
 *   - mTLS client cert/key are configured only for #MQTT_CFG_AUTH_MTLS and
 *     cleared otherwise,
 *   - the applied values are self-checked through the mqtt_config getters.
 *
 * A rejected (even partially applied) configuration leaves the connect
 * gate closed: mqtt_cfg_connect() refuses to start the transport
 * (#MQTT_CFG_ERR_NOT_APPLIED) until a fully successful apply completes
 * every setter and self-check.
 *
 * On any failure the lamp output is forced inactive and the transport
 * configuration may be partially updated (a rejected configuration never
 * reports success).
 *
 * @param[in] cfg Validated configuration (recommended: the output of
 *                mqtt_cfg_load()).
 *
 * @return #MQTT_CFG_OK on success, otherwise an error code.
 */
mqtt_cfg_status_t mqtt_cfg_apply(const mqtt_cfg_t *cfg);

/**
 * @brief Convenience: mqtt_cfg_load() then mqtt_cfg_apply().
 *
 * Forces the output inactive on any failure.
 *
 * @return #MQTT_CFG_OK when the broker/TLS configuration was loaded and
 *         the verified transport was configured.
 */
mqtt_cfg_status_t mqtt_cfg_load_and_apply(void);

/**
 * @brief Drive one verified TLS connection over the configured transport.
 *
 * Only a fully applied configuration (a previous successful
 * mqtt_cfg_apply(), tracked internally) can start the transport; the
 * applied address is re-checked for the mqtts:// scheme, SSL on,
 * skip-verify off and a present file-path CA source before the
 * application layer starts.  Starts the Mongoose MQTT application layer
 * (mqtt_app_init() — idempotent) and waits up to @p timeout_ms for the
 * connection to become established or to fail.  A TLS-level failure
 * (unknown CA, hostname mismatch, handshake error) or a timeout forces
 * the lamp output inactive and returns #MQTT_CFG_ERR_CONNECT.
 *
 * On success the verified transport stays connected for the consumers
 * that follow (TASK-111/112) and the lamp fail-off barrier is released —
 * this is the single re-enable transition (valid configuration applied
 * AND verified TLS connected); a Wi-Fi connection alone never releases
 * it.  The caller remains the lifecycle owner of mqtt_app_deinit().
 *
 * @param[in] timeout_ms Maximum wait in milliseconds; 0 performs a single
 *                       immediate check.
 *
 * @return #MQTT_CFG_OK when connected, #MQTT_CFG_ERR_NOT_APPLIED when no
 *         verified configuration was applied first, #MQTT_CFG_ERR_CONNECT
 *         on failure/timeout.
 */
mqtt_cfg_status_t mqtt_cfg_connect(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CFG_H */