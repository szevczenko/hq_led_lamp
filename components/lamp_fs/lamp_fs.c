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

#include "hq_storage.h"

/* --------------------------------------------------------------------- */
/* Internal state                                                         */
/* --------------------------------------------------------------------- */

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

static void run_fail_safe(hq_storage_status_t status, void *user_data)
{
    const lamp_fs_config_t *config = user_data;

    (void)status;
    if (config != NULL && config->fail_safe_cb != NULL)
    {
        config->fail_safe_cb();
    }
}

static lamp_fs_status_t map_status(hq_storage_status_t status)
{
    switch (status)
    {
    case HQ_STORAGE_OK:
        return LAMP_FS_OK;
    case HQ_STORAGE_ERR_INVALID_ARGUMENT:
        return LAMP_FS_ERR_INVALID_ARGUMENT;
    case HQ_STORAGE_ERR_MOUNT:
        return LAMP_FS_ERR_MOUNT;
    case HQ_STORAGE_ERR_UNMOUNT:
        return LAMP_FS_ERR_UNMOUNT;
    case HQ_STORAGE_ERR_DIRECTORY:
    case HQ_STORAGE_ERR_NOT_MOUNTED:
    default:
        return LAMP_FS_ERR_DIRECTORY;
    }
}

/* --------------------------------------------------------------------- */
/* Public API                                                             */
/* --------------------------------------------------------------------- */

bool lamp_fs_is_mounted(void)
{
    return hq_storage_is_mounted();
}

lamp_fs_status_t lamp_fs_ensure_dir(const char *logical_path)
{
    if (logical_path == NULL || logical_path[0] == '\0')
    {
        return LAMP_FS_ERR_INVALID_ARGUMENT;
    }

    return map_status(hq_storage_ensure_dir(logical_path));
}

lamp_fs_status_t lamp_fs_init(const lamp_fs_config_t *config)
{
    const hq_storage_config_t storage_config = {
        .partition_label = LAMP_FS_PARTITION_LABEL,
        .mount_point = LAMP_FS_MOUNT_POINT,
        .directories = s_required_dirs,
        .directory_count = LAMP_FS_REQUIRED_DIR_COUNT,
        .on_failure = run_fail_safe,
        .user_data = (void *)config,
    };

    return map_status(hq_storage_init(&storage_config));
}

lamp_fs_status_t lamp_fs_deinit(void)
{
    return map_status(hq_storage_deinit());
}
