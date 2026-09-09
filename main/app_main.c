/**
 * @file app_main.c
 * @brief Kitchen LED Controller application entry point.
 *
 *  ## Investigation notes (read before resuming TASK-118 / TASK-107)
 *
 *   An on-hardware log was captured after flashing the current TASK-107 code:
 *   `mkdir "/cert"` etc. reported `Failed to mkdir ... Error (-17)` (EEXIST), yet
 *   boot still reported `Filesystem ready`. This was investigated and verified
 *   directly against the vendored `esp_littlefs` source and the OSAL backends:
 *
 *  - **The EEXIST log is not evidence of the TASK-118 bug and is not a defect.**
 *   `idf.py flash` (without `erase-flash`) never rewrites the `storage`
 *   partition, so a LittleFS filesystem created by an earlier boot of this same
 *   firmware persists across reflashes and already contains `/cert`,
 *   `/config`, `/state`. `osal_mkdir()` on ESP already maps `EEXIST` ->
 *   `OSAL_ERR_NAME_TAKEN` (`platform/hq_platform/src/osal/esp/osal_dir_impl.c`),
 *   and `lamp_fs_ensure_dir()` already treats that as `LAMP_FS_OK`. Do not
 *   "fix" this idempotent path again; it is correct as-is.
 *  - **The TASK-118 concern is still real and unresolved.** As of this
 *   verification, `platform/hq_platform/src/osal/esp/osal_mount_impl.c`
 *   `osal_mount()` still sets `format_if_mount_failed = true`. TASK-118 must
 *   still be implemented; it was not accidentally already fixed.
 *  - **A concrete, verified contradiction blocks TASK-107 as currently worded.**
 *   `components/lamp_fs/lamp_fs.c` `lamp_fs_init()` currently handles a
 *   directory-creation failure by only clearing the internal `s_mounted` flag
 *   (`s_mounted = false;`) — it never calls `osal_unmount()`. The existing test
 *   `tests/lamp_fs/lamp_fs_test.c::test_directory_failure_forces_fail_safe`
 *   explicitly asserts the **opposite** of the current task requirement:
 *   `TEST_ASSERT_TRUE(osal_fs_mock_is_mounted())` after a directory failure.
 *   Any agent that implements the "unmount before returning the error"
 *   requirement literally will break this existing, currently-passing test.
 *   The next implementer must update that assertion (and add an
 *   unmount-failure-handling test) in the same change, not treat the existing
 *   test as a spec to preserve.
 *  - Everything else already implemented for TASK-107 (`partitions.csv` layout,
 *   `/cert`/`/config`/`/state` idempotent creation, no `osal_mkfs()`/
 *   `osal_rmfs()` on the boot path) looks complete against the current
 *   requirements and should not be redone from scratch.
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
