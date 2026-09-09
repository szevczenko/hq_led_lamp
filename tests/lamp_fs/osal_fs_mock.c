/**
 * @file osal_fs_mock.c
 * @brief Test-only OSAL filesystem double implementation (TASK-107)
 *
 * Records calls and models an in-memory directory set:
 *   - osal_mount() fails only with the injected status; on success it marks
 *     the double mounted (mirrors the OSAL contract that directory
 *     operations require a mounted volume),
 *   - osal_mkdir() fails when the volume is not mounted, returns
 *     OSAL_ERR_NAME_TAKEN for pre-registered directories, and otherwise
 *     creates the directory or returns the injected failure status,
 *   - osal_unmount() clears the mounted flag unless a failure is injected,
 *   - osal_mkfs() and osal_rmfs() are recorded but never invoked by the
 *     production bootstrap; their counters exist so the tests can prove no
 *     format operation ever happens on the boot path.
 *
 * The mock deliberately provides only the narrow OSAL surface the bootstrap
 * uses, without pulling in the real OSAL headers (they would drag the whole
 * file API and platform backends into the host build).
 */

#include "osal_fs_mock.h"

#include <stdint.h>
#include <string.h>

#include "osal_error.h"

/* Mirror of OSAL_MAX_PATH_LEN (osal_file.h); the real header drags in the
 * full file API, which the host test build intentionally avoids. */
#define OSAL_FS_MOCK_PATH_LEN 128
#define OSAL_FS_MOCK_MAX_DIRS 16

struct osal_fs_mock_state
{
    int32_t mount_status;
    int32_t mkdir_status;
    int32_t unmount_status;

    bool mounted;
    int mount_calls;
    int mkdir_calls;
    int unmount_calls;
    int mkfs_calls;
    int rmfs_calls;

    bool dir_exists[OSAL_FS_MOCK_MAX_DIRS];
    char dir_paths[OSAL_FS_MOCK_MAX_DIRS][OSAL_FS_MOCK_PATH_LEN];

    int mkdir_count;
    char mkdir_paths[OSAL_FS_MOCK_MAX_DIRS][OSAL_FS_MOCK_PATH_LEN];
};

static struct osal_fs_mock_state s;

void osal_fs_mock_reset(void)
{
    memset(&s, 0, sizeof(s));
    s.mount_status = OSAL_SUCCESS;
    s.mkdir_status = OSAL_SUCCESS;
    s.unmount_status = OSAL_SUCCESS;
}

void osal_fs_mock_set_mount_status(int32_t status)
{
    s.mount_status = status;
}

void osal_fs_mock_set_mkdir_status(int32_t status)
{
    s.mkdir_status = status;
}

void osal_fs_mock_set_unmount_status(int32_t status)
{
    s.unmount_status = status;
}

void osal_fs_mock_add_existing_dir(const char *path)
{
    for (int i = 0; i < OSAL_FS_MOCK_MAX_DIRS; ++i)
    {
        if (!s.dir_exists[i])
        {
            s.dir_exists[i] = true;
            strncpy(s.dir_paths[i], path, OSAL_FS_MOCK_PATH_LEN - 1);
            s.dir_paths[i][OSAL_FS_MOCK_PATH_LEN - 1] = '\0';
            return;
        }
    }
}

int osal_fs_mock_mount_calls(void)
{
    return s.mount_calls;
}

int osal_fs_mock_mkdir_calls(void)
{
    return s.mkdir_calls;
}

int osal_fs_mock_unmount_calls(void)
{
    return s.unmount_calls;
}

int osal_fs_mock_mkfs_calls(void)
{
    return s.mkfs_calls;
}

int osal_fs_mock_rmfs_calls(void)
{
    return s.rmfs_calls;
}

bool osal_fs_mock_is_mounted(void)
{
    return s.mounted;
}

bool osal_fs_mock_mkdir_path(int index, char *out, size_t out_size)
{
    if (index < 0 || index >= s.mkdir_count || out == NULL)
    {
        return false;
    }
    strncpy(out, s.mkdir_paths[index], out_size - 1);
    out[out_size - 1] = '\0';
    return true;
}

/* --------------------------------------------------------------------- */
/* OSAL contract doubles                                                  */
/* --------------------------------------------------------------------- */

int32_t osal_mount(const char *devname, const char *mount_point)
{
    (void)devname;
    (void)mount_point;
    ++s.mount_calls;

    if (s.mount_status != OSAL_SUCCESS)
    {
        return s.mount_status;
    }

    s.mounted = true;
    return OSAL_SUCCESS;
}

int32_t osal_unmount(const char *mount_point)
{
    (void)mount_point;
    ++s.unmount_calls;

    if (s.unmount_status != OSAL_SUCCESS)
    {
        return s.unmount_status;
    }

    s.mounted = false;
    return OSAL_SUCCESS;
}

int32_t osal_mkdir(const char *path)
{
    ++s.mkdir_calls;

    if (s.mkdir_count < OSAL_FS_MOCK_MAX_DIRS)
    {
        strncpy(s.mkdir_paths[s.mkdir_count], path, OSAL_FS_MOCK_PATH_LEN - 1);
        s.mkdir_paths[s.mkdir_count][OSAL_FS_MOCK_PATH_LEN - 1] = '\0';
    }
    ++s.mkdir_count;

    if (!s.mounted)
    {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }

    for (int i = 0; i < OSAL_FS_MOCK_MAX_DIRS; ++i)
    {
        if (s.dir_exists[i] && strcmp(s.dir_paths[i], path) == 0)
        {
            return OSAL_ERR_NAME_TAKEN;
        }
    }

    /* One-shot injected failure for the next (non-existing) directory. */
    int32_t status = s.mkdir_status;
    s.mkdir_status = OSAL_SUCCESS;
    return status;
}

int32_t osal_mkfs(char *address, const char *devname, const char *volname,
                  size_t block_size, size_t num_blocks)
{
    (void)address;
    (void)devname;
    (void)volname;
    (void)block_size;
    (void)num_blocks;

    ++s.mkfs_calls;
    return OSAL_SUCCESS;
}

int32_t osal_rmfs(const char *devname)
{
    (void)devname;

    ++s.rmfs_calls;
    return OSAL_SUCCESS;
}
