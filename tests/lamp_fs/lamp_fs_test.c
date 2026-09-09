/**
 * @file lamp_fs_test.c
 * @brief Host unit tests for the LittleFS bootstrap (TASK-107)
 *
 * Runs the production components/lamp_fs code against the in-memory OSAL
 * filesystem double (osal_fs_mock.c) under Unity.  Coverage per the
 * TASK-107 definition of done:
 *
 *   1. first mount — a successful mount creates /cert, /config and /state
 *      exactly once each, reports mounted, and does not touch the fail-safe,
 *   2. existing directories — pre-existing directories (OSAL_ERR_NAME_TAKEN)
 *      are idempotent success; contents are never recreated or erased,
 *   3. mount failure — a failed osal_mount() invokes the fail-safe exactly
 *      once, creates no directories, reports the storage untouched,
 *   4. directory failure — a failed osal_mkdir() invokes the fail-safe and
 *      reports the failure while earlier created directories survive; the
 *      volume is unmounted through osal_unmount() before the error is
 *      returned, so no hidden mount is left behind (if the unmount itself
 *      fails, LAMP_FS_ERR_UNMOUNT is reported and the mount state stays
 *      consistent with the backend),
 *   5. no format — osal_mkfs()/osal_rmfs() are never invoked on the boot
 *      path in any scenario (credential storage is never reformatted),
 *   6. argument and lifecycle edge cases (NULL path, unmount when not
 *      mounted, idempotent re-init, retry after a failed bootstrap).
 */

#include <string.h>

#include "lamp_fs.h"
#include "osal_error.h"
#include "osal_fs_mock.h"

#include "unity.h"

/* The OSAL double does not drag in the real OSAL headers (they pull the
 * whole file API), so the path buffer size used by the mock API mirrors
 * OSAL_MAX_PATH_LEN from osal_file.h. */
#define OSAL_MAX_PATH_LEN_MOCK 128

/* --------------------------------------------------------------------- */
/* Test fixtures                                                          */
/* --------------------------------------------------------------------- */

static int s_fail_safe_calls;

static void record_fail_safe(void)
{
    ++s_fail_safe_calls;
}

void setUp(void)
{
    osal_fs_mock_reset();
    s_fail_safe_calls = 0;
}

void tearDown(void)
{
}

/* --------------------------------------------------------------------- */
/* 1. First mount                                                         */
/* --------------------------------------------------------------------- */

static void test_first_mount_creates_directories(void)
{
    lamp_fs_config_t config = { .fail_safe_cb = record_fail_safe };

    lamp_fs_status_t status = lamp_fs_init(&config);

    TEST_ASSERT_EQUAL_INT(LAMP_FS_OK, status);
    TEST_ASSERT_TRUE(lamp_fs_is_mounted());
    TEST_ASSERT_EQUAL_INT(1, osal_fs_mock_mount_calls());
    TEST_ASSERT_EQUAL_INT(3, osal_fs_mock_mkdir_calls());

    char path[OSAL_MAX_PATH_LEN_MOCK];
    TEST_ASSERT_TRUE(osal_fs_mock_mkdir_path(0, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(LAMP_FS_DIR_CERT, path);
    TEST_ASSERT_TRUE(osal_fs_mock_mkdir_path(1, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(LAMP_FS_DIR_CONFIG, path);
    TEST_ASSERT_TRUE(osal_fs_mock_mkdir_path(2, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(LAMP_FS_DIR_STATE, path);

    /* Success never runs the fail-safe. */
    TEST_ASSERT_EQUAL_INT(0, s_fail_safe_calls);
}

static void test_first_mount_without_callback(void)
{
    /* NULL config / NULL callback are tolerated on the success path. */
    TEST_ASSERT_EQUAL_INT(LAMP_FS_OK, lamp_fs_init(NULL));
    TEST_ASSERT_TRUE(lamp_fs_is_mounted());
    TEST_ASSERT_EQUAL_INT(3, osal_fs_mock_mkdir_calls());
}

/* --------------------------------------------------------------------- */
/* 2. Existing directories (idempotency)                                  */
/* --------------------------------------------------------------------- */

static void test_existing_directories_are_idempotent(void)
{
    osal_fs_mock_add_existing_dir(LAMP_FS_DIR_CERT);
    osal_fs_mock_add_existing_dir(LAMP_FS_DIR_CONFIG);
    osal_fs_mock_add_existing_dir(LAMP_FS_DIR_STATE);

    lamp_fs_config_t config = { .fail_safe_cb = record_fail_safe };
    lamp_fs_status_t status = lamp_fs_init(&config);

    TEST_ASSERT_EQUAL_INT(LAMP_FS_OK, status);
    TEST_ASSERT_TRUE(lamp_fs_is_mounted());
    /* Every directory still verified through mkdir (existing = NAME_TAKEN). */
    TEST_ASSERT_EQUAL_INT(3, osal_fs_mock_mkdir_calls());
    TEST_ASSERT_EQUAL_INT(0, s_fail_safe_calls);
}

static void test_partially_existing_directories(void)
{
    /* Only /config exists from a previous boot. */
    osal_fs_mock_add_existing_dir(LAMP_FS_DIR_CONFIG);

    TEST_ASSERT_EQUAL_INT(LAMP_FS_OK, lamp_fs_init(NULL));
    TEST_ASSERT_EQUAL_INT(3, osal_fs_mock_mkdir_calls());
    /* The bootstrap still ran the failing-safe nothing: no fail-safe call. */
    TEST_ASSERT_EQUAL_INT(0, s_fail_safe_calls);
}

/* --------------------------------------------------------------------- */
/* 3. Mount failure                                                       */
/* --------------------------------------------------------------------- */

static void test_mount_failure_forces_fail_safe(void)
{
    osal_fs_mock_set_mount_status(OSAL_ERROR);

    lamp_fs_config_t config = { .fail_safe_cb = record_fail_safe };
    lamp_fs_status_t status = lamp_fs_init(&config);

    TEST_ASSERT_EQUAL_INT(LAMP_FS_ERR_MOUNT, status);
    TEST_ASSERT_FALSE(lamp_fs_is_mounted());
    /* Fail-safe invoked exactly once, before returning. */
    TEST_ASSERT_EQUAL_INT(1, s_fail_safe_calls);
    /* No directory touched: existing storage preserved. */
    TEST_ASSERT_EQUAL_INT(0, osal_fs_mock_mkdir_calls());
    /* And no format either: a mount failure must never reformat storage. */
    TEST_ASSERT_EQUAL_INT(0, osal_fs_mock_mkfs_calls());
    TEST_ASSERT_EQUAL_INT(0, osal_fs_mock_rmfs_calls());
}

static void test_mount_failure_without_callback(void)
{
    osal_fs_mock_set_mount_status(OSAL_ERROR);

    /* Must not crash without a callback; still reports the failure. */
    TEST_ASSERT_EQUAL_INT(LAMP_FS_ERR_MOUNT, lamp_fs_init(NULL));
    TEST_ASSERT_EQUAL_INT(0, s_fail_safe_calls);
}

/* --------------------------------------------------------------------- */
/* 4. Directory creation failure                                          */
/* --------------------------------------------------------------------- */

static void test_directory_failure_forces_fail_safe(void)
{
    /* /cert exists from a previous boot (idempotent), /config then fails,
     * /state is never attempted. */
    osal_fs_mock_add_existing_dir(LAMP_FS_DIR_CERT);
    osal_fs_mock_set_mkdir_status(OSAL_ERROR);

    lamp_fs_config_t config = { .fail_safe_cb = record_fail_safe };
    lamp_fs_status_t status = lamp_fs_init(&config);

    TEST_ASSERT_EQUAL_INT(LAMP_FS_ERR_DIRECTORY, status);
    TEST_ASSERT_EQUAL_INT(1, s_fail_safe_calls);
    /* Failure stops the sequence: /cert verified, /config attempted. */
    TEST_ASSERT_EQUAL_INT(2, osal_fs_mock_mkdir_calls());
    /* No hidden mount: the volume was unmounted before returning the
     * error, so the component state and the backend state agree. */
    TEST_ASSERT_EQUAL_INT(1, osal_fs_mock_unmount_calls());
    TEST_ASSERT_FALSE(lamp_fs_is_mounted());
    TEST_ASSERT_FALSE(osal_fs_mock_is_mounted());
}

static void test_directory_failure_unmount_failure_is_reported(void)
{
    /* Directory creation fails AND the recovery unmount fails: the error
     * must be reported, and lamp_fs_is_mounted() must keep reflecting the
     * still-mounted backend (no hidden mount with a lying is_mounted()). */
    osal_fs_mock_add_existing_dir(LAMP_FS_DIR_CERT);
    osal_fs_mock_set_mkdir_status(OSAL_ERROR);
    osal_fs_mock_set_unmount_status(OSAL_ERROR);

    lamp_fs_config_t config = { .fail_safe_cb = record_fail_safe };
    lamp_fs_status_t status = lamp_fs_init(&config);

    TEST_ASSERT_EQUAL_INT(LAMP_FS_ERR_UNMOUNT, status);
    TEST_ASSERT_EQUAL_INT(1, s_fail_safe_calls);
    TEST_ASSERT_EQUAL_INT(1, osal_fs_mock_unmount_calls());
    /* Backend really is still mounted, so is_mounted() stays true. */
    TEST_ASSERT_TRUE(osal_fs_mock_is_mounted());
    TEST_ASSERT_TRUE(lamp_fs_is_mounted());

    /* The subsequent explicit unmount succeeds and restores consistency. */
    osal_fs_mock_set_unmount_status(OSAL_SUCCESS);
    TEST_ASSERT_EQUAL_INT(LAMP_FS_OK, lamp_fs_deinit());
    TEST_ASSERT_FALSE(lamp_fs_is_mounted());
    TEST_ASSERT_FALSE(osal_fs_mock_is_mounted());
}

static void test_retry_after_failed_directory_bootstrap(void)
{
    /* A failed bootstrap must be retryable cleanly: the second init finds
     * no stale mount, mounts again and converges to a working layout. */
    osal_fs_mock_set_mkdir_status(OSAL_ERROR);

    lamp_fs_config_t config = { .fail_safe_cb = record_fail_safe };
    TEST_ASSERT_EQUAL_INT(LAMP_FS_ERR_DIRECTORY, lamp_fs_init(&config));
    TEST_ASSERT_FALSE(lamp_fs_is_mounted());
    TEST_ASSERT_FALSE(osal_fs_mock_is_mounted());

    /* Injected mkdir failure was one-shot; the retry succeeds normally. */
    TEST_ASSERT_EQUAL_INT(LAMP_FS_OK, lamp_fs_init(&config));
    TEST_ASSERT_TRUE(lamp_fs_is_mounted());
    TEST_ASSERT_TRUE(osal_fs_mock_is_mounted());
    TEST_ASSERT_EQUAL_INT(2, osal_fs_mock_mount_calls());
    TEST_ASSERT_EQUAL_INT(4, osal_fs_mock_mkdir_calls());
    /* Exactly one fail-safe run for the one failed attempt. */
    TEST_ASSERT_EQUAL_INT(1, s_fail_safe_calls);
}

static void test_no_format_on_boot_path(void)
{
    /* Guard against silent credential destruction: in no scenario —
     * success, mount failure, directory failure — may the bootstrap call
     * osal_mkfs() or osal_rmfs(). */
    TEST_ASSERT_EQUAL_INT(LAMP_FS_OK, lamp_fs_init(NULL));
    TEST_ASSERT_EQUAL_INT(0, osal_fs_mock_mkfs_calls());
    TEST_ASSERT_EQUAL_INT(0, osal_fs_mock_rmfs_calls());

    osal_fs_mock_reset();
    osal_fs_mock_set_mount_status(OSAL_ERROR);
    (void)lamp_fs_init(NULL);
    TEST_ASSERT_EQUAL_INT(0, osal_fs_mock_mkfs_calls());
    TEST_ASSERT_EQUAL_INT(0, osal_fs_mock_rmfs_calls());

    osal_fs_mock_reset();
    osal_fs_mock_set_mkdir_status(OSAL_ERROR);
    (void)lamp_fs_init(NULL);
    TEST_ASSERT_EQUAL_INT(0, osal_fs_mock_mkfs_calls());
    TEST_ASSERT_EQUAL_INT(0, osal_fs_mock_rmfs_calls());
}

static void test_ensure_dir_failure_reported(void)
{
    TEST_ASSERT_EQUAL_INT(LAMP_FS_OK, lamp_fs_init(NULL));
    osal_fs_mock_set_mkdir_status(OSAL_ERROR);

    /* Direct ensure_dir() reports, but does not run the init fail-safe
     * (no fail-safe callback is registered for direct calls). */
    TEST_ASSERT_EQUAL_INT(LAMP_FS_ERR_DIRECTORY,
                          lamp_fs_ensure_dir(LAMP_FS_DIR_STATE));
    TEST_ASSERT_EQUAL_INT(0, s_fail_safe_calls);
}

/* --------------------------------------------------------------------- */
/* 5. Argument and lifecycle edge cases                                   */
/* --------------------------------------------------------------------- */

static void test_ensure_dir_invalid_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(LAMP_FS_ERR_INVALID_ARGUMENT, lamp_fs_ensure_dir(NULL));
    TEST_ASSERT_EQUAL_INT(LAMP_FS_ERR_INVALID_ARGUMENT, lamp_fs_ensure_dir(""));

    /* Before a mount, ensure_dir reports a directory error. */
    TEST_ASSERT_EQUAL_INT(LAMP_FS_ERR_DIRECTORY,
                          lamp_fs_ensure_dir(LAMP_FS_DIR_CERT));
}

static void test_unmount_lifecycle(void)
{
    TEST_ASSERT_EQUAL_INT(LAMP_FS_OK, lamp_fs_deinit()); /* not mounted: ok */

    TEST_ASSERT_EQUAL_INT(LAMP_FS_OK, lamp_fs_init(NULL));
    TEST_ASSERT_TRUE(lamp_fs_is_mounted());
    TEST_ASSERT_EQUAL_INT(LAMP_FS_OK, lamp_fs_deinit());
    TEST_ASSERT_FALSE(lamp_fs_is_mounted());

    /* Re-init after unmount re-creates the layout idempotently. */
    TEST_ASSERT_EQUAL_INT(LAMP_FS_OK, lamp_fs_init(NULL));
    TEST_ASSERT_TRUE(lamp_fs_is_mounted());
    TEST_ASSERT_EQUAL_INT(6, osal_fs_mock_mkdir_calls());
}

static void test_mount_and_dirs_are_ordered(void)
{
    /* Regression: directory creation must only ever happen after a
     * successful mount (the OSAL rejects mkdir while unmounted). */
    osal_fs_mock_set_mount_status(OSAL_ERROR);
    lamp_fs_config_t config = { .fail_safe_cb = record_fail_safe };
    (void)lamp_fs_init(&config);

    char path[OSAL_MAX_PATH_LEN_MOCK];
    TEST_ASSERT_EQUAL_INT(0, osal_fs_mock_mkdir_calls());
    TEST_ASSERT_FALSE(osal_fs_mock_mkdir_path(0, path, sizeof(path)));
}

/* --------------------------------------------------------------------- */
/* Unity entry point                                                      */
/* --------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_first_mount_creates_directories);
    RUN_TEST(test_first_mount_without_callback);
    RUN_TEST(test_existing_directories_are_idempotent);
    RUN_TEST(test_partially_existing_directories);
    RUN_TEST(test_mount_failure_forces_fail_safe);
    RUN_TEST(test_mount_failure_without_callback);
    RUN_TEST(test_directory_failure_forces_fail_safe);
    RUN_TEST(test_directory_failure_unmount_failure_is_reported);
    RUN_TEST(test_retry_after_failed_directory_bootstrap);
    RUN_TEST(test_no_format_on_boot_path);
    RUN_TEST(test_ensure_dir_failure_reported);
    RUN_TEST(test_ensure_dir_invalid_arguments);
    RUN_TEST(test_unmount_lifecycle);
    RUN_TEST(test_mount_and_dirs_are_ordered);

    return UNITY_END();
}
