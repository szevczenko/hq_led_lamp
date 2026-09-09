/**
 * @file app_main.c
 * @brief Kitchen LED Controller application entry point.
 *
 *  ## Boot-order contract (TASK-107 / TASK-118)
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
 *
 *  The idempotent mkdir (EEXIST -> OSAL_ERR_NAME_TAKEN -> LAMP_FS_OK) is
 *  expected after a reflash onto persistent storage and is not a defect.
 */

#include <stdbool.h>

#include "esp_log.h"

#include "hal_types.h"
#include "lamp_control.h"
#include "lamp_fs.h"
#include "sdkconfig.h"

static const char *TAG = "klc";

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
/* Configuration loading (placeholder — TASK-108)                          */
/* --------------------------------------------------------------------- */

static void load_configuration(void)
{
    /* Configuration service (versioned device.json / mqtt.json parsing)
     * arrives with TASK-108; it must only ever run after a successful
     * filesystem bootstrap. */
    ESP_LOGI(TAG, "Configuration loading not implemented yet (TASK-108)");
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
}
