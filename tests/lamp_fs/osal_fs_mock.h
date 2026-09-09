/**
 * @file osal_fs_mock.h
 * @brief Test-only OSAL filesystem double for lamp-fs host tests (TASK-107)
 *
 * Implements the subset of the OSAL filesystem API used by components/lamp_fs
 * (osal_mount, osal_mkdir, osal_unmount) as an in-memory test double so the
 * bootstrap logic can be exercised on the host with deterministic failure
 * injection and call recording.  Compiled only into the lamp-fs host test
 * binary; never part of any production build.
 */

#ifndef OSAL_FS_MOCK_H
#define OSAL_FS_MOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "osal_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of distinct directories the mock tracks. */
#define OSAL_FS_MOCK_MAX_DIRS 16

/**
 * @brief Reset the double to its pristine state (call in every setUp).
 */
void osal_fs_mock_reset(void);

/**
 * @brief Inject the status the next osal_mount() call returns.
 *
 * OSAL_SUCCESS by default.
 */
void osal_fs_mock_set_mount_status(int32_t status);

/**
 * @brief Inject the status the next osal_mkdir() call returns.
 *
 * OSAL_SUCCESS by default; set to OSAL_ERR_NAME_TAKEN to model an existing
 * directory or OSAL_ERROR to model a creation failure.  The injection is
 * one-shot: it applies only to the next osal_mkdir() call for a directory
 * that does not already exist, after which the double returns to normal
 * behaviour (existing directories keep returning OSAL_ERR_NAME_TAKEN).
 */
void osal_fs_mock_set_mkdir_status(int32_t status);

/**
 * @brief Inject the status the next osal_unmount() call returns.
 */
void osal_fs_mock_set_unmount_status(int32_t status);

/**
 * @brief Pre-register an existing directory by logical path (idempotency).
 */
void osal_fs_mock_add_existing_dir(const char *path);

/**
 * @brief Number of osal_mount() calls since reset.
 */
int osal_fs_mock_mount_calls(void);

/**
 * @brief Number of osal_mkdir() calls since reset.
 */
int osal_fs_mock_mkdir_calls(void);

/**
 * @brief Number of osal_unmount() calls since reset.
 */
int osal_fs_mock_unmount_calls(void);

/**
 * @brief Number of times the double reports the volume as mounted.
 *
 * Exposes the mock's internal mounted flag (set by a successful
 * osal_mount(), cleared by a successful osal_unmount()), so tests can
 * verify directory creation only happens after a mount.
 */
bool osal_fs_mock_is_mounted(void);

/**
 * @brief Fetch the logical path passed to mkdir() call number @p index.
 *
 * Copies into @p out (size @p out_size).  Returns false if the index is out
 * of range.
 */
bool osal_fs_mock_mkdir_path(int index, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_FS_MOCK_H */
