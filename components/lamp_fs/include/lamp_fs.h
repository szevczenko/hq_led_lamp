/**
 * @file lamp_fs.h
 * @brief LittleFS bootstrap for credential and configuration storage (TASK-107)
 *
 * Normative public API for the product filesystem bootstrap.
 *
 * Scope
 * -----
 * This component owns the boot-time filesystem bring-up for the kitchen LED
 * controller:
 *   - mounting the LittleFS partition ("storage") at `/littlefs` through the
 *     portable OSAL filesystem contract (osal_mount.h / osal_dir.h),
 *   - creating the application directory layout idempotently after a
 *     successful mount,
 *   - a fail-safe contract: if the mount or any directory creation fails,
 *     a caller-supplied fail-safe callback is invoked exactly once and the
 *     bootstrap reports the failure.  The product layer wires that callback
 *     to lamp_control_force_inactive(), so the lamp output can never be
 *     energized while the credential/config filesystem is unusable.
 *
 * Logical paths
 * -------------
 * Logical OSAL paths used by the application (mapped by the OSAL backend
 * onto the `/littlefs` mount point):
 *
 *     /cert    CA certificate, device certificate and private key
 *     /config  wifi.json, mqtt.json, device.json, manufacturing.json
 *     /state   ota.json and other runtime state
 *
 * Formatting policy (normative)
 * -----------------------------
 * The boot path NEVER formats the filesystem.  Formatting is destructive to
 * credential storage (device keys, certificates, manufacturing state), so it
 * is permitted only through an explicit, operator-triggered flow:
 *
 *   - permitted: a manufacturing or provisioning command (for example an
 *     explicit factory-reset / re-provision operation) that deliberately
 *     formats or re-initializes the storage partition.  Such an operation
 *     must be an intentional act, never an automatic reaction to a boot
 *     error, and it must force the lamp output off afterwards.
 *   - required: every ordinary boot-time failure (osal_mount() error,
 *     directory creation error, unexpected filesystem state) must take the
 *     safe-failure path: invoke the fail-safe callback, report the error,
 *     keep the existing storage contents untouched, and keep the lamp
 *     output off.  Silent reformatting of credential storage is prohibited.
 *
 * A corrupted or missing filesystem is therefore a degraded boot: the
 * device stays safe (output off) and surfaces the error in the boot log;
 * recovery happens through the explicit manufacturing/provisioning flow.
 *
 * Idempotency
 * -----------
 * Directory creation is idempotent: a directory that already exists
 * (OSAL_ERR_NAME_TAKEN) is treated as success, so every boot converges to
 * the same directory layout without touching existing files.
 *
 * Host testability
 * ----------------
 * The component speaks only the portable OSAL filesystem API, so host tests
 * exercise it against an in-memory OSAL double (see tests/lamp_fs).  It never
 * references ESP-IDF VFS, esp_littlefs or flash-partition symbols directly.
 */

#ifndef LAMP_FS_H
#define LAMP_FS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Status type                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Portable result/error type returned by the bootstrap.
 *
 * #LAMP_FS_OK (0) signals success; every other value is an error code.
 */
typedef enum lamp_fs_status {
    LAMP_FS_OK                   = 0,  /**< Mounted and directory layout ready. */
    LAMP_FS_ERR_MOUNT            = -1, /**< osal_mount() failed; storage untouched. */
    LAMP_FS_ERR_DIRECTORY        = -2, /**< Creating /cert, /config or /state failed. */
    LAMP_FS_ERR_INVALID_ARGUMENT = -3  /**< Invalid argument (NULL path, empty path). */
} lamp_fs_status_t;

/* --------------------------------------------------------------------- */
/* Constants                                                              */
/* --------------------------------------------------------------------- */

/** @brief LittleFS partition label (must match partitions.csv). */
#define LAMP_FS_PARTITION_LABEL "storage"

/** @brief OSAL mount point for the LittleFS volume. */
#define LAMP_FS_MOUNT_POINT "/littlefs"

/** @brief Logical directory holding certificates and device keys. */
#define LAMP_FS_DIR_CERT "/cert"

/** @brief Logical directory holding product configuration. */
#define LAMP_FS_DIR_CONFIG "/config"

/** @brief Logical directory holding runtime state (OTA, diagnostics). */
#define LAMP_FS_DIR_STATE "/state"

/* --------------------------------------------------------------------- */
/* Configuration                                                          */
/* --------------------------------------------------------------------- */

/**
 * @brief Fail-safe callback invoked when the bootstrap fails.
 *
 * Called exactly once per failed lamp_fs_init() attempt, before the failure
 * status is returned.  The product layer wires this to the lamp fail-off
 * (lamp_control_force_inactive()) so the output is off while storage is
 * unusable.  May be NULL in tests or when no fail-safe action is available.
 */
typedef void (*lamp_fs_fail_safe_cb_t)(void);

/**
 * @brief Bootstrap configuration.
 *
 * All fields are optional; a NULL config pointer means "no fail-safe
 * callback" (used by host tests).
 */
typedef struct lamp_fs_config {
    lamp_fs_fail_safe_cb_t fail_safe_cb; /**< Invoked on any bootstrap failure. */
} lamp_fs_config_t;

/* --------------------------------------------------------------------- */
/* Operations                                                             */
/* --------------------------------------------------------------------- */

/**
 * @brief Mount the filesystem and create the directory layout.
 *
 * Sequence:
 *  1. osal_mount(LAMP_FS_PARTITION_LABEL, LAMP_FS_MOUNT_POINT).  On failure
 *     the fail-safe callback (if any) runs, the storage is left untouched
 *     (no format, no erase) and #LAMP_FS_ERR_MOUNT is returned.
 *  2. Create LAMP_FS_DIR_CERT, LAMP_FS_DIR_CONFIG and LAMP_FS_DIR_STATE
 *     idempotently.  An existing directory (#OSAL_ERR_NAME_TAKEN) is
 *     success.  Any other directory error runs the fail-safe callback and
 *     returns #LAMP_FS_ERR_DIRECTORY; already-created directories and all
 *     existing file contents are preserved.
 *
 * On success the filesystem is mounted and the three directories exist.
 * This must be called before loading any configuration or credentials.
 *
 * @param[in] config Optional configuration (may be NULL).
 *
 * @return
 *  - #LAMP_FS_OK on success,
 *  - #LAMP_FS_ERR_MOUNT if the mount failed (storage untouched, output off),
 *  - #LAMP_FS_ERR_DIRECTORY if a directory could not be created (output off).
 */
lamp_fs_status_t lamp_fs_init(const lamp_fs_config_t *config);

/**
 * @brief Create one logical directory idempotently.
 *
 * Exposed for tests and for future runtime directory provisioning.  An
 * existing directory is success; any other OSAL failure is reported.
 *
 * @param[in] logical_path Logical OSAL path, e.g. "/cert".
 *
 * @return
 *  - #LAMP_FS_OK when the directory exists or was created,
 *  - #LAMP_FS_ERR_INVALID_ARGUMENT if @p logical_path is NULL or empty,
 *  - #LAMP_FS_ERR_DIRECTORY if the OSAL refused creation for another reason.
 */
lamp_fs_status_t lamp_fs_ensure_dir(const char *logical_path);

/**
 * @brief Report whether the bootstrap currently has the volume mounted.
 *
 * @return true after a successful lamp_fs_init(); false before init, after
 *         a failed init, or after lamp_fs_deinit().
 */
bool lamp_fs_is_mounted(void);

/**
 * @brief Unmount the filesystem.
 *
 * Forwards to osal_unmount(); safe to call when not mounted (returns
 * #LAMP_FS_OK).  Used by explicit manufacturing/factory-reset flows after
 * they have formatted storage.
 *
 * @return #LAMP_FS_OK on success or when already unmounted, otherwise
 *         #LAMP_FS_ERR_MOUNT mapped from the OSAL error.
 */
lamp_fs_status_t lamp_fs_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* LAMP_FS_H */
