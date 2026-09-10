/**
 * @file osal_file_mock.h
 * @brief Test-only OSAL file API double for app-config host tests (TASK-108)
 *
 * In-memory POSIX-style file store used to exercise components/app_config
 * on the host.  It implements the subset of the OSAL file API the component
 * uses (osal_open_create, osal_close, osal_read, osal_write, osal_stat,
 * osal_remove, osal_rename, osal_cp) with deterministic fault injection so
 * corruption and interrupted-write scenarios are reproducible.  Compiled
 * only into the app-config host test binary; never part of any production
 * build.
 *
 * The double models the LittleFS semantics the OSAL backend exposes:
 * files are flat path-keyed byte blobs, rename() is atomic, and a failing
 * write leaves a truncated file behind (interrupted write).
 */

#ifndef OSAL_FILE_MOCK_H
#define OSAL_FILE_MOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "osal_error.h"
#include "osal_file.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of files the double stores. */
#define OSAL_FILE_MOCK_MAX_FILES 16

/** @brief Maximum size of one stored file. */
#define OSAL_FILE_MOCK_MAX_FILE_SIZE 8192

/**
 * @brief Reset the double to its pristine state (call in every setUp).
 */
void osal_file_mock_reset(void);

/**
 * @brief Clear all fault injections without wiping the stored files.
 */
void osal_file_mock_reset_failures(void);

/**
 * @brief Pre-place a file with the given contents.
 *
 * @return true when the file fits into the double's storage.
 */
bool osal_file_mock_add_file(const char *path, const char *contents);

/**
 * @brief Copy the current contents of @p path into @p out.
 *
 * @return true when the file exists and fits into @p out_cap.
 */
bool osal_file_mock_get_file(const char *path, char *out, size_t out_cap);

/**
 * @brief Does the double currently hold @p path?
 */
bool osal_file_mock_has_file(const char *path);

/**
 * @brief Number of files currently stored.
 */
int osal_file_mock_file_count(void);

/**
 * @brief Make osal_write() fail after @p total_bytes_written bytes have
 *        successfully been written (cumulative across all handles).
 *
 * A negative value disables the injection.  The failure models an
 * interrupted write (power loss): the bytes written before the failure
 * stay in the file, the file is left truncated.
 */
void osal_file_mock_fail_write_after(int32_t total_bytes_written);

/**
 * @brief Make the next osal_rename() call return @p status (one-shot).
 *
 * The injection is consumed by the first rename after it is set, so a
 * commit that performs several renames (last-known-good promotion and live
 * replacement) only has the FIRST rename refused; the component reports
 * the failure and never retries over the injected fault.
 */
void osal_file_mock_set_rename_status(int32_t status);

/**
 * @brief Make the @p call_number-th osal_rename() call (1-based, counted
 *        since the last reset) return @p status (one-shot).
 *
 * A commit performs up to three renames in a fixed order:
 *   1. preserve the previous "<live>.good" at "<live>.good.old",
 *   2. promote the staged copy to "<live>.good",
 *   3. atomically replace the live file.
 * The injection is consumed by the call whose number matches, so tests can
 * target a specific promotion step (e.g. call 3 = the live replacement)
 * while earlier steps proceed normally.
 */
void osal_file_mock_fail_rename_at(int call_number, int32_t status);

/**
 * @brief Make every osal_open_create() for @p path fail with @p status
 *        until cleared (status == OSAL_SUCCESS disables the injection).
 *
 * This models "the file exists (osal_stat() succeeds) but cannot be opened
 * right now" — a transient storage failure underneath a present document.
 */
void osal_file_mock_set_open_status(const char *path, int32_t status);

/**
 * @brief Make the next osal_stat() call return @p status (one-shot;
 *        OSAL_SUCCESS disables).  Models a transient stat failure.
 */
void osal_file_mock_set_stat_status(int32_t status);

/**
 * @brief Create or overwrite @p path with exactly @p len bytes of @p data.
 *
 * Unlike osal_file_mock_add_file() this does NOT stop at a NUL byte, so a
 * test can store a file with embedded-NUL / binary trailing content.
 *
 * @return true on success.
 */
bool osal_file_mock_add_file_raw(const char *path, const void *data,
                                 size_t len);

/**
 * @brief Make the next osal_cp() call fail after the copy started
 *        (one-shot): the destination is left truncated/partial.
 */
void osal_file_mock_fail_next_cp(bool fail);

/**
 * @brief Make the next osal_remove() call return @p status (one-shot).
 */
void osal_file_mock_set_remove_status(int32_t status);

/**
 * @brief Number of osal_rename() calls since reset.
 */
int osal_file_mock_rename_calls(void);

/**
 * @brief Number of osal_cp() calls since reset.
 */
int osal_file_mock_cp_calls(void);

/**
 * @brief Number of osal_remove() calls since reset.
 */
int osal_file_mock_remove_calls(void);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_FILE_MOCK_H */
