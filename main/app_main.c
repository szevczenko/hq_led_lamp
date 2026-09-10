/**
 * @file app_main.c
 * @brief Kitchen LED Controller application entry point.
 *
 *  ## Boot-order contract (TASK-107 / TASK-118 / TASK-109)
 *
 *   1. lamp_control_init() — the output is initialized off and stays off
 *      until a valid desired state arrives.
 *   2. lamp_fs_init() — mounts the LittleFS "storage" partition at
 *      /littlefs through the OSAL and creates /cert, /config, /state
 *      idempotently.  The mount never formats (TASK-118 removed the
 *      backend's format_if_mount_failed), and lamp_fs never calls
 *      osal_mkfs()/osal_rmfs() on the boot path: formatting is reserved
 *      for explicit manufacturing/provisioning operations.
 *   3. On any filesystem failure the fail-safe below forces the output
 *      inactive, the error is logged, storage is preserved untouched and
 *      configuration is not loaded.  A failed directory bootstrap unmounts
 *      the volume before returning, so no hidden mount survives.
 *   4. Only a fully successful bootstrap reaches configuration loading.
 *      Configuration loading itself is ordered: product documents
 *      (device.json / manufacturing.json, TASK-108) and the broker/TLS
 *      document (mqtt.json, TASK-110) first, then the device identity
 *      (TASK-111).
 *   5. Device identity (TASK-111) is loaded after configuration validation
 *      and before any ThingsBoard initialization.  device_identity_load()
 *      only trusts validated manufacturing/configuration records, rejects
 *      missing, empty, oversized or malformed identity input, never logs
 *      the token, and is FATAL safe-off: a missing or invalid identity
 *      forces the output inactive and blocks the ThingsBoard session — the
 *      device never falls back to anonymous or plaintext connectivity.
 *   6. Wi-Fi onboarding (TASK-109) runs next through the product-owned
 *      network adapter.  ThingsBoard must never connect before the network:
 *      the gate is the consumed network_manager_wait_connected() call in
 *      start_network() — a ThingsBoard connect (TASK-110..112) may only be
 *      attempted behind that gate, re-checked with
 *      network_manager_is_connected() ( ThingsBoard integration follows in
 *      its own tasks ).
 *   7. On Wi-Fi loss the adapter forces the lamp output inactive
 *      synchronously and informs the application state machine through
 *      on_disconnected().
 *   8. The single re-enable transition for the lamp fail-off barrier is a
 *      successful verified MQTT/TLS connection (mqtt_cfg_connect(), TASK-110).
 *      on_network_connected() only opens the connect gate — it never
 *      releases the barrier, so a Wi-Fi connection alone, an invalid or
 *      missing identity, or any rejected broker/TLS configuration can
 *      never enable the output.
 *
 *  The idempotent mkdir (EEXIST -> OSAL_ERR_NAME_TAKEN -> LAMP_FS_OK) is
 *  expected after a reflash onto persistent storage and is not a defect.
 */

#include <stdbool.h>

#include "esp_log.h"

#include "app_config.h"
#include "device_identity.h"
#include "hal_types.h"
#include "lamp_control.h"
#include "lamp_fs.h"
#include "mqtt_cfg.h"
#include "network_manager.h"
#include "sdkconfig.h"

static const char *TAG = "klc";

/* --------------------------------------------------------------------- */
/* Network adapter wiring (TASK-109)                                       */
/* --------------------------------------------------------------------- */

/*
 * Gate note for TASK-110..112: the single network gate is the consumed
 * network_manager_wait_connected() call in start_network() below (plus
 * network_manager_is_connected() for later re-checks).  There is NO separate
 * ready-flag here on purpose: a flag written by the callbacks but not read
 * by the ThingsBoard start decision would be dead gate state.  The
 * ThingsBoard tasks must consume the same gate before any connect attempt.
 */

/**
 * @brief Network connection established (Wi-Fi manager worker context).
 *
 * Called by the adapter after the network is up.  Only now may the
 * ThingsBoard client begin its connect attempts; nothing here touches Wi-Fi
 * credentials — the adapter and the manager keep them private.
 *
 * Fail-off re-enable contract (TASK-110): this callback NEVER releases the
 * lamp fail-off barrier.  The barrier latched by the disconnect path or by
 * a rejected broker/TLS configuration stays latched until
 * mqtt_cfg_connect() succeeds — i.e. until a valid configuration has been
 * applied AND the verified MQTT/TLS connection has succeeded.  A Wi-Fi
 * connection alone must not lift the barrier, otherwise a TLS
 * configuration failure could be re-enabled by a later state application.
 */
static void on_network_connected(void *context)
{
    (void)context;
    ESP_LOGI(TAG, "Network connected; verified MQTT/TLS connect is now "
                  "allowed (fail-off release is owned by mqtt_cfg_connect)");
}

/**
 * @brief Network lost or connect attempts exhausted (Wi-Fi worker context).
 *
 * The adapter has already forced the lamp output inactive synchronously on
 * this path (fail-off within application latency); this callback only logs
 * the transition.  ThingsBoard is considered down until the network gate
 * (network_manager_wait_connected / network_manager_is_connected) passes
 * again.
 */
static void on_network_disconnected(void *context)
{
    (void)context;
    ESP_LOGW(TAG, "Network lost; lamp forced off by adapter, "
                  "ThingsBoard stays disconnected until reconnect");
}

/**
 * @return true when the network is started AND connected; false leaves the
 *         application in the safe offline state (output off, no ThingsBoard).
 */
static bool start_network(void)
{
    static const network_callbacks_t callbacks = {
        .on_connected    = on_network_connected,
        .on_disconnected = on_network_disconnected,
        .context         = NULL,
    };

    const int status = network_manager_start(&callbacks);
    if (status != NETWORK_OK)
    {
        ESP_LOGE(TAG, "network_manager_start failed: %d (offline, output off)",
                 (int)status);
        return false;
    }

    /* The ThingsBoard ordering gate: block (bounded) until the network is
     * connected.  ThingsBoard initialization that follows in TASK-110..112
     * must run only behind this gate — it is the single network gate of the
     * application (no separate ready-flag is kept, see the gate note above). */
    if (!network_manager_wait_connected(NETWORK_CONNECT_TIMEOUT_MS))
    {
        ESP_LOGW(TAG, "No Wi-Fi connection within %u ms; "
                      "ThingsBoard connect stays blocked",
                 (unsigned)NETWORK_CONNECT_TIMEOUT_MS);
        return false;
    }

    return true;
}

/* --------------------------------------------------------------------- */
/* Lamp output                                                            */
/* --------------------------------------------------------------------- */

static bool init_lamp_output(void)
{
    /* CONFIG_KLC_* values come from the IDF Kconfig (sdkconfig.h).  A bool
     * option that is "not set" is simply not #defined, so the polarity is
     * selected in preprocessor context (undefined -> 0 in #if). */
    lamp_control_config_t config = {
        .pin          = (hal_pin_t)CONFIG_KLC_LED_PWM_GPIO,
        .frequency_hz = (uint32_t)CONFIG_KLC_LED_PWM_FREQUENCY_HZ,
#if CONFIG_KLC_LED_PWM_ACTIVE_LOW
        .polarity     = HAL_POLARITY_ACTIVE_LOW,
#else
        .polarity     = HAL_POLARITY_ACTIVE_HIGH,
#endif
    };

    lamp_status_t status = lamp_control_init(&config);
    if (status != LAMP_OK)
    {
        ESP_LOGE(TAG, "lamp_control_init failed: %d (output off)", (int)status);
        return false;
    }

    /* The lamp is initialized with its output off; it stays off until a
     * valid desired state arrives. */
    return true;
}

/* --------------------------------------------------------------------- */
/* Filesystem bootstrap                                                   */
/* --------------------------------------------------------------------- */

/**
 * @brief Fail-safe invoked on any filesystem bootstrap failure.
 *
 * Forces the lamp output to the logical INACTIVE level so the electrical
 * output is off while the credential/config filesystem is unusable.
 */
static void lamp_fs_fail_safe(void)
{
    lamp_status_t status = lamp_control_force_inactive();
    if (status != LAMP_OK)
    {
        ESP_LOGE(TAG, "Filesystem fail-safe could not force output off: %d",
                 (int)status);
    }
}

/**
 * @return true when the filesystem is mounted and the directory layout is
 *         ready; false when boot must degrade (output off, no config load).
 */
static bool bootstrap_filesystem(void)
{
    lamp_fs_config_t fs_config = {
        .fail_safe_cb = lamp_fs_fail_safe,
    };

    lamp_fs_status_t status = lamp_fs_init(&fs_config);
    if (status != LAMP_FS_OK)
    {
        ESP_LOGE(TAG, "Filesystem bootstrap failed: %d "
                      "(storage preserved, output off, configuration not loaded)",
                 (int)status);
        return false;
    }

    ESP_LOGI(TAG, "Filesystem ready: %s mounted at %s (cert/, config/, state/)",
             LAMP_FS_PARTITION_LABEL, LAMP_FS_MOUNT_POINT);
    return true;
}

/* --------------------------------------------------------------------- */
/* Configuration loading (TASK-108)                                        */
/* --------------------------------------------------------------------- */

/**
 * @brief Load the product documents through the configuration service.
 *
 * Runs only after a successful filesystem bootstrap.  Both documents are
 * loaded with last-known-good recovery; on any failure the error is logged
 * (never any document content) and boot continues in the safe degraded
 * mode — the output is off until ThingsBoard supplies a valid desired
 * state.  No lamp state is ever persisted: ThingsBoard is the
 * desired-state authority in release 1.
 *
 * @return true when both documents are usable (loaded or recovered).
 */
static bool load_product_configuration(void)
{
    app_config_device_doc_t device;
    app_config_status_t status = app_config_load_device(&device);
    if ((status != APP_CONFIG_OK) && (status != APP_CONFIG_OK_RECOVERED))
    {
        ESP_LOGE(TAG, "device.json unavailable: %d (%s)%s",
                 (int)status, app_config_status_name(status),
                 (status == APP_CONFIG_OK_RECOVERED) ? "" :
                 " (manufacturing flow must provision it)");
        return false;
    }

    ESP_LOGI(TAG, "device.json loaded%s: product='%s' hw='%s' tb='%s'",
             (status == APP_CONFIG_OK_RECOVERED) ? " (recovered)" : "",
             device.product, device.hardware_revision,
             device.thingsboard_name);

    app_config_manufacturing_doc_t manufacturing;
    status = app_config_load_manufacturing(&manufacturing);
    if ((status != APP_CONFIG_OK) && (status != APP_CONFIG_OK_RECOVERED))
    {
        ESP_LOGE(TAG, "manufacturing.json unavailable: %d (%s)",
                 (int)status, app_config_status_name(status));
        return false;
    }

    ESP_LOGI(TAG, "manufacturing.json loaded%s: state=%d mode=%d",
             (status == APP_CONFIG_OK_RECOVERED) ? " (recovered)" : "",
             (int)manufacturing.manufacturing_state,
             (int)manufacturing.credential_mode);

    return true;
}

/**
 * @brief Load and validate the broker/TLS configuration (TASK-110).
 *
 * Runs after the filesystem bootstrap and behind
 * network_manager_wait_connected() (see start_network()).  An accepted
 * /config/mqtt.json always configures verified TLS (mqtts:// with the
 * private CA and DNS-hostname verification) through the Mongoose
 * mqtt_config API; mqtt_cfg rejects plaintext mode, skip_verify, empty CA
 * paths and certificate paths outside /cert, and forces the lamp output
 * inactive on any configuration failure, so a rejected or missing broker
 * configuration can never enable the output.  The verified transport
 * connect (mqtt_cfg_connect()) is consumed by the ThingsBoard tasks
 * (TASK-111/112) behind the same network gate.
 */
static void load_broker_tls_configuration(void)
{
    mqtt_cfg_status_t status = mqtt_cfg_load_and_apply();
    if (status != MQTT_CFG_OK)
    {
        ESP_LOGE(TAG, "Broker/TLS configuration rejected: %d (%s)"
                      " (output forced off, ThingsBoard blocked)",
                 (int)status, mqtt_cfg_status_name(status));
    }
}

/* --------------------------------------------------------------------- */
/* Device identity loading (TASK-111)                                      */
/* --------------------------------------------------------------------- */

/**
 * @brief Load the ThingsBoard access-token identity (TASK-111).
 *
 * Runs after the filesystem bootstrap and the product/broker configuration
 * validation, and before any ThingsBoard initialization.  The identity
 * module only trusts validated manufacturing/configuration records and
 * rejects missing, empty, oversized or malformed identity input; on any
 * rejection the lamp output is forced inactive (fatal safe-off) and the
 * ThingsBoard session stays blocked — the device never falls back to an
 * anonymous or plaintext session.
 *
 * Redaction contract: the error log carries the status and its name only.
 * The token is a bearer credential and is NEVER logged, printed or echoed;
 * the module itself performs no logging.
 */
static void load_device_identity(void)
{
    device_identity_status_t status = device_identity_load();
    if (status != DEVICE_IDENTITY_OK)
    {
        ESP_LOGE(TAG, "Device identity rejected: %d (%s)"
                      " (output forced off, ThingsBoard blocked)",
                 (int)status, device_identity_status_name(status));
        return;
    }

    ESP_LOGI(TAG, "Device identity loaded (access-token auth, client id "
                  "ready for ThingsBoard initialization)");
}

static void load_configuration(void)
{
    (void)load_product_configuration();
    load_broker_tls_configuration();
    load_device_identity();
}

/* --------------------------------------------------------------------- */
/* ThingsBoard initialization ordering (TASK-111)                          */
/* --------------------------------------------------------------------- */

/**
 * @brief Start the ThingsBoard session behind the verified-TLS and
 *        identity gates.
 *
 * Called only after the network gate (start_network()) passed.  The boot
 * order contract (TASK-111) is enforced here:
 *
 *   1. IDENTITY gate — the ThingsBoard session may only be initialized
 *      with a validated identity (device_identity_is_loaded()).  A missing
 *      or invalid identity was already fatal safe-off in
 *      load_device_identity(); this gate re-checks so an anonymous session
 *      can never be started as a fallback.
 *   2. VERIFIED-TLS gate (TASK-110) — mqtt_cfg_connect() drives one
 *      verified MQTT/TLS connection over the applied transport and is the
 *      single re-enable transition for the lamp fail-off barrier.  The
 *      identity is loaded and validated BEFORE this connect is consumed,
 *      so a device can never reach the ThingsBoard transport without a
 *      valid identity.
 *
 * After both gates pass, the ThingsBoard session (TASK-112) consumes the
 * identity exactly here:
 *
 *     tb_client_config_t tb_cfg;                 // TASK-112
 *     tb_cfg.access_token = device_identity_token_provider()();   // the token
 *     (void)device_identity_client_id(&tb_cfg.client_id);        // stable id
 *
 * Nothing from the identity is logged on this path: the token is a bearer
 * credential and never appears in the log.
 */
static void start_thingsboard(void)
{
    if (!device_identity_is_loaded())
    {
        ESP_LOGE(TAG, "Device identity unavailable; ThingsBoard session "
                      "blocked, output stays off");
        return;
    }

    mqtt_cfg_status_t status = mqtt_cfg_connect(NETWORK_CONNECT_TIMEOUT_MS);
    if (status != MQTT_CFG_OK)
    {
        ESP_LOGE(TAG, "Verified TLS connect failed: %d (%s)"
                      " (output forced off, ThingsBoard session blocked)",
                 (int)status, mqtt_cfg_status_name(status));
        return;
    }

    /* Valid identity + verified TLS: the ThingsBoard session is now allowed
     * to initialize (TASK-112) with the access token from the token
     * provider and the stable client ID. */
    ESP_LOGI(TAG, "Verified TLS connected with validated identity; "
                  "ThingsBoard session initialization allowed");
}

/* --------------------------------------------------------------------- */
/* Entry point                                                            */
/* --------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "Kitchen LED Controller starting");

    if (!init_lamp_output())
    {
        /* The output is off; boot continues so the filesystem bootstrap
         * (and, later, OTA state) is still available for a safe degraded
         * run.  A PWM init failure must never block storage bring-up. */
        ESP_LOGW(TAG, "Lamp output unavailable; continuing with output off");
    }

    if (!bootstrap_filesystem())
    {
        /* lamp_fs_fail_safe() already forced the output inactive.  Storage
         * is untouched; recovery happens through the explicit
         * manufacturing/provisioning flow, never by auto-formatting. */
        return;
    }

    /* Configuration loading is ordered (TASK-108 / TASK-110 / TASK-111):
     * product documents and the broker/TLS document are validated first,
     * then the device identity runs its manufacturing/configuration gates.
     * A rejected identity is fatal safe-off for the ThingsBoard session. */
    load_configuration();

    /* Wi-Fi onboarding (TASK-109) runs before any ThingsBoard connect
     * attempt.  On failure (or timeout) the device stays offline in the
     * safe state: the output is off and the consumed wait_connected() gate
     * keeps the ThingsBoard initialization blocked. */
    if (!start_network())
    {
        ESP_LOGW(TAG, "Continuing offline; ThingsBoard connect is blocked");
        return;
    }

    /* Behind the network gate: identity first, then the verified TLS
     * connect, then the ThingsBoard session (TASK-111 ordering). */
    start_thingsboard();
}
