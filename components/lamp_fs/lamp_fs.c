/**
 * @file lamp_fs.c
 * @brief LittleFS bootstrap implementation (TASK-107)
 *
 * See lamp_fs.h for the normative contract.  Implementation notes:
 *
 *   - the boot path mounts with osal_mount() only.  It never calls
 *     osal_mkfs()/osal_rmfs(): formatting is reserved for the explicit
 *     manufacturing/provisioning flows and is never an automatic reaction
 *     to a boot error, because a silent reformat would destroy device
 *     credentials and manufacturing state,
 *   - directory creation goes through osal_mkdir() on logical OSAL paths;
 *     #OSAL_ERR_NAME_TAKEN ("already exists") is mapped to success so the
 *     bootstrap is idempotent across reboots,
 *   - every failure invokes the fail-safe callback exactly once and leaves
 *     the storage contents untouched.
 */

#include "lamp_fs.h"

#include <string.h>

#include "osal_dir.h"
#include "osal_error.h"
#include "osal_mount.h"

/* --------------------------------------------------------------------- */
/* Internal state                                                         */
/* --------------------------------------------------------------------- */

static bool s_mounted;

/** @brief Directories that must exist after a successful bootstrap. */
static const char *const s_required_dirs[] = {
    LAMP_FS_DIR_CERT,
    LAMP_FS_DIR_CONFIG,
    LAMP_FS_DIR_STATE,
};

#define LAMP_FS_REQUIRED_DIR_COUNT \
    (sizeof(s_required_dirs) / sizeof(s_required_dirs[0]))

/* --------------------------------------------------------------------- */
/* Helpers                                                                */
/* --------------------------------------------------------------------- */

static void run_fail_safe(const lamp_fs_config_t *config)
{
    if (config != NULL && config->fail_safe_cb != NULL)
    {
        config->fail_safe_cb();
    }
}

/* --------------------------------------------------------------------- */
/* Public API                                                             */
/* --------------------------------------------------------------------- */

bool lamp_fs_is_mounted(void)
{
    return s_mounted;
}

lamp_fs_status_t lamp_fs_ensure_dir(const char *logical_path)
{
    if (logical_path == NULL || logical_path[0] == '\0')
    {
        return LAMP_FS_ERR_INVALID_ARGUMENT;
    }

    if (!s_mounted)
    {
        return LAMP_FS_ERR_DIRECTORY;
    }

    int32_t rc = osal_mkdir(logical_path);
    if (rc == OSAL_SUCCESS || rc == OSAL_ERR_NAME_TAKEN)
    {
        /* OSAL_ERR_NAME_TAKEN: the directory already exists — idempotent
         * success, existing contents are untouched. */
        return LAMP_FS_OK;
    }

    return LAMP_FS_ERR_DIRECTORY;
}

lamp_fs_status_t lamp_fs_init(const lamp_fs_config_t *config)
{
    s_mounted = false;

    int32_t rc = osal_mount(LAMP_FS_PARTITION_LABEL, LAMP_FS_MOUNT_POINT);
    if (rc != OSAL_SUCCESS)
    {
        /* Safe failure: do not format, do not erase.  Existing storage —
         * including device credentials — must survive this error.  Force
         * the lamp output off and report the failure. */
        run_fail_safe(config);
        return LAMP_FS_ERR_MOUNT;
    }

    s_mounted = true;

    for (size_t i = 0U; i < LAMP_FS_REQUIRED_DIR_COUNT; ++i)
    {
        lamp_fs_status_t status = lamp_fs_ensure_dir(s_required_dirs[i]);
        if (status != LAMP_FS_OK)
        {
            /* Safe failure: keep whatever directories were created and all
             * existing file contents, force the output off, report it. */
            s_mounted = false;
            run_fail_safe(config);
            return status;
        }
    }

    return LAMP_FS_OK;
}

lamp_fs_status_t lamp_fs_deinit(void)
{
    if (!s_mounted)
    {
        return LAMP_FS_OK;
    }

    int32_t rc = osal_unmount(LAMP_FS_MOUNT_POINT);
    if (rc != OSAL_SUCCESS)
    {
        return LAMP_FS_ERR_MOUNT;
    }

    s_mounted = false;
    return LAMP_FS_OK;
}
