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
 *   5. Wi-Fi onboarding (TASK-109) runs next through the product-owned
 *      network adapter.  ThingsBoard must never connect before the network:
 *      s_network_ready is the gate — a ThingsBoard connect may only be
 *      attempted after network_manager_wait_connected() reported a
 *      connection ( ThingsBoard integration follows in its own task ).
 *   6. On Wi-Fi loss the adapter forces the lamp output inactive
 *      synchronously and informs the application state machine through
 *      on_disconnected().
 *
 *  The idempotent mkdir (EEXIST -> OSAL_ERR_NAME_TAKEN -> LAMP_FS_OK) is
 *  expected after a reflash onto persistent storage and is not a defect.
 */

#include <stdbool.h>

#include "esp_log.h"

#include "app_config.h"
#include "hal_types.h"
#include "lamp_control.h"
#include "lamp_fs.h"
#include "network_manager.h"
#include "sdkconfig.h"

static const char *TAG = "klc";

/* --------------------------------------------------------------------- */
/* Network adapter wiring (TASK-109)                                       */
/* --------------------------------------------------------------------- */

/**
 * Application network state machine flag.  It is set only after the network
 * gate (network_manager_wait_connected) has confirmed a connection and is
 * cleared on disconnect.  A ThingsBoard connect may only be attempted while
 * this flag is set — the network contract of release 1.
 */
static bool s_network_ready = false;

/**
 * @brief Network connection established (Wi-Fi manager worker context).
 *
 * Called by the adapter after the network is up.  Only now may the
 * ThingsBoard client begin its connect attempts; nothing here touches Wi-Fi
 * credentials — the adapter and the manager keep them private.
 */
static void on_network_connected(void *context)
{
    (void)context;
    s_network_ready = true;
    ESP_LOGI(TAG, "Network connected; ThingsBoard connect is now allowed");
}

/**
 * @brief Network lost or connect attempts exhausted (Wi-Fi worker context).
 *
 * The adapter has already forced the lamp output inactive synchronously on
 * this path (fail-off within application latency); this callback only
 * updates the application state machine.  ThingsBoard is considered down
 * until the network gate passes again.
 */
static void on_network_disconnected(void *context)
{
    (void)context;
    s_network_ready = false;
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
     * connected.  ThingsBoard initialization that follows in its own task
     * must run only behind this gate / the s_network_ready flag. */
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

static void load_configuration(void)
{
    (void)load_product_configuration();
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

    load_configuration();

    /* Wi-Fi onboarding (TASK-109) runs before any ThingsBoard connect
     * attempt.  On failure (or timeout) the device stays offline in the
     * safe state: the output is off and s_network_ready keeps the (later)
     * ThingsBoard initialization gated. */
    if (!start_network())
    {
        ESP_LOGW(TAG, "Continuing offline; ThingsBoard connect is blocked");
    }
}
