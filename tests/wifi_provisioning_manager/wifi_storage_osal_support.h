/**
 * @file wifi_storage_osal_support.h
 * @brief Test-only OSAL doubles for the wifi_ap.json overwrite host test
 *        (TASK-136)
 *
 * The overwrite host test compiles the REAL platform Wi-Fi manager
 * (wifi_managment.c) and its persistence module (wifi_config.c) on the
 * host.  Those two production files speak the portable OSAL file API
 * (osal_file.h) and the OSAL log API (osal_log.h).  This support file is
 * the test-only back-end for exactly that surface:
 *
 *   - osal_file_* is backed by a REAL temporary directory that stands in
 *     for the product storage mount (lamp_fs mounts the LittleFS
 *     "storage" partition at /littlefs on the target; the ESP OSAL maps
 *     the relative WIFI_CONFIG_FILE_PATH "wifi_ap.json" onto
 *     /littlefs/wifi_ap.json).  The host double applies the same logical
 *     path mapping ("wifi_ap.json" -> <storage-root>/wifi_ap.json) and the
 *     same CREATE|TRUNCATE open semantics, so every overwrite behaviour of
 *     the product path is exercised against a real file,
 *   - osal_log_printf captures every line into a bounded ring so the test
 *     can prove that neither the old nor the new secret ever reaches a log
 *     line (the credentials-never-logged rule).
 *
 * The remaining OSAL primitives the Wi-Fi manager uses (mutex, binary
 * semaphore, task) are provided by the REAL platform POSIX back-ends
 * (osal_mutex_impl.c, osal_bin_sem_impl.c, osal_task_impl.c, compiled
 * from platform/hq_platform/src/osal/posix): the worker task, its
 * semaphores and its delays behave exactly like on target.
 *
 * This file is a test double only: it is compiled solely into the
 * wifi_storage_overwrite host test binary and is never part of any
 * production build.
 */

#ifndef WIFI_STORAGE_OSAL_SUPPORT_H
#define WIFI_STORAGE_OSAL_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Storage mount (lamp_fs stand-in)                                       */
/* --------------------------------------------------------------------- */

/**
 * @brief Mount a fresh temp-dir-backed logical storage volume.
 *
 * Creates a unique temporary directory (the "storage" root) and marks the
 * OSAL file layer mounted, mirroring the product boot where lamp_fs_init()
 * mounts the /littlefs volume before any Wi-Fi state is loaded.  Any
 * previous volume is unmounted first.  Files written through the OSAL file
 * API are real files under this directory.
 *
 * @return true on success.
 */
bool wifi_storage_test_mount(void);

/**
 * @brief Unmount (remove) the storage volume and reset the file layer.
 *
 * Deletes every file created under the storage root and the directory
 * itself, then clears the mounted flag.  Idempotent.
 */
void wifi_storage_test_unmount(void);

/**
 * @brief Return the host-side absolute path a logical OSAL path maps to.
 *
 * Uses the same mapping rules as the ESP OSAL back-end: a relative path
 * ("wifi_ap.json") resolves under the storage root, an absolute path
 * ("/config/foo") loses its leading slash, and a path already prefixed
 * with the logical mount point ("/littlefs/foo") is treated as logical
 * (resolved under the storage root).
 *
 * @param[in] logical_path Logical OSAL path (e.g. "wifi_ap.json").
 * @return Host absolute path in a static buffer, or "" on invalid input.
 *         The buffer is overwritten by the next call in the same thread.
 */
const char *wifi_storage_test_mapped_path(const char *logical_path);

/**
 * @brief Read a logical file into a caller buffer (raw bytes + NUL).
 *
 * @param[in]  logical_path Logical OSAL path.
 * @param[out] buf          Destination buffer.
 * @param[in]  cap          Buffer capacity in bytes.
 * @return Number of bytes read (>= 0), or -1 on any error.
 */
long wifi_storage_test_read_file(const char *logical_path, char *buf,
                                 size_t cap);

/* --------------------------------------------------------------------- */
/* Captured log                                                           */
/* --------------------------------------------------------------------- */

/** @brief Clear the captured log. */
void wifi_storage_test_log_reset(void);

/**
 * @brief Read-only access to the captured log (NUL-terminated).
 *
 * The capture is a bounded ring, so tests must flush between checks.
 */
const char *wifi_storage_test_log_get(void);

/**
 * @brief Return true when the captured log contains @p needle.
 *
 * @param[in] needle Substring to search for (may be NULL/empty -> false).
 */
bool wifi_storage_test_log_contains(const char *needle);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_STORAGE_OSAL_SUPPORT_H */