/**
 * @file osal_file_mock.c
 * @brief Test-only OSAL file API double implementation (TASK-108)
 *
 * See osal_file_mock.h for the contract.  State model:
 *
 *   - files are path-keyed byte blobs; osal_open_create() with
 *     OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE replaces content,
 *     read-only opens fail with OSAL_ERR_NAME_NOT_FOUND for missing files,
 *   - osal_write() appends at the implicit write offset (files opened for
 *     writing start empty when truncated, otherwise at EOF), so the double
 *     behaves like a POSIX fd for the component's whole-file writes,
 *   - fault injection: writes can fail after N cumulative bytes (leaving a
 *     truncated file — the interrupted-write model), rename/remove return
 *     one-shot statuses, and cp can be made to fail mid-copy,
 *   - osal_cp() is implemented through the double's own open/read/write, so
 *     write fault injection affects copies the same way as direct writes.
 */

#include "osal_file_mock.h"

#include <string.h>

#include "osal_error.h"

/* --------------------------------------------------------------------- */
/* Internal state                                                         */
/* --------------------------------------------------------------------- */

typedef struct
{
    bool    in_use;
    char    path[OSAL_MAX_PATH_LEN];
    char    data[OSAL_FILE_MOCK_MAX_FILE_SIZE];
    size_t  size;
} mock_file_t;

static mock_file_t s_files[OSAL_FILE_MOCK_MAX_FILES];

static bool    s_open_in_use[OSAL_FILE_MOCK_MAX_FILES];
static int     s_open_slot[OSAL_FILE_MOCK_MAX_FILES]; /* file index or -1 */
static bool    s_open_writable[OSAL_FILE_MOCK_MAX_FILES];
static size_t  s_open_offset[OSAL_FILE_MOCK_MAX_FILES];
static int     s_open_count;

static int32_t s_write_fail_after;   /* -1 = disabled */
static int32_t s_write_total;        /* cumulative successful bytes   */
static int32_t s_rename_status;      /* one-shot, OSAL_SUCCESS = off  */
static int32_t s_remove_status;      /* one-shot, OSAL_SUCCESS = off  */
static int32_t s_stat_status;        /* one-shot, OSAL_SUCCESS = off  */
static bool    s_fail_next_cp;
static int     s_rename_fail_at;    /* 0 = disabled (1-based call no.) */
static int32_t s_rename_fail_at_status;

/* Persistent open-failure injection: osal_open_create() for the given path
 * fails with this status until cleared (OSAL_SUCCESS = disabled).  Models
 * "the file exists but cannot be opened right now". */
static char    s_open_fail_path[OSAL_MAX_PATH_LEN];
static int32_t s_open_fail_status;   /* OSAL_SUCCESS = disabled       */

static int s_rename_calls;
static int s_cp_calls;
static int s_remove_calls;

/* --------------------------------------------------------------------- */
/* Reset                                                                  */
/* --------------------------------------------------------------------- */

void osal_file_mock_reset(void)
{
    memset(s_files, 0, sizeof(s_files));
    memset(s_open_in_use, 0, sizeof(s_open_in_use));
    memset(s_open_slot, 0, sizeof(s_open_slot));
    memset(s_open_writable, 0, sizeof(s_open_writable));
    memset(s_open_offset, 0, sizeof(s_open_offset));
    s_open_count = 0;

    osal_file_mock_reset_failures();
}

void osal_file_mock_reset_failures(void)
{
    s_write_fail_after = -1;
    s_write_total = 0;
    s_rename_status = OSAL_SUCCESS;
    s_remove_status = OSAL_SUCCESS;
    s_stat_status = OSAL_SUCCESS;
    s_fail_next_cp = false;
    s_rename_fail_at = 0;
    s_rename_fail_at_status = OSAL_SUCCESS;
    s_open_fail_path[0] = '\0';
    s_open_fail_status = OSAL_SUCCESS;
    s_rename_calls = 0;
    s_cp_calls = 0;
    s_remove_calls = 0;
}

/* --------------------------------------------------------------------- */
/* Test-control API                                                       */
/* --------------------------------------------------------------------- */

static mock_file_t *find_file(const char *path)
{
    for (int i = 0; i < OSAL_FILE_MOCK_MAX_FILES; ++i)
    {
        if (s_files[i].in_use && (strcmp(s_files[i].path, path) == 0))
        {
            return &s_files[i];
        }
    }
    return NULL;
}

bool osal_file_mock_add_file(const char *path, const char *contents)
{
    if ((path == NULL) || (contents == NULL))
    {
        return false;
    }

    const size_t len = strlen(contents);
    if (len > (size_t)OSAL_FILE_MOCK_MAX_FILE_SIZE)
    {
        return false;
    }

    mock_file_t *f = find_file(path);
    if (f == NULL)
    {
        for (int i = 0; i < OSAL_FILE_MOCK_MAX_FILES; ++i)
        {
            if (!s_files[i].in_use)
            {
                f = &s_files[i];
                break;
            }
        }
        if (f == NULL)
        {
            return false;
        }
        f->in_use = true;
        strncpy(f->path, path, sizeof(f->path) - 1U);
        f->path[sizeof(f->path) - 1U] = '\0';
        f->size = 0U;
    }

    memcpy(f->data, contents, len);
    f->size = len;
    return true;
}

bool osal_file_mock_get_file(const char *path, char *out, size_t out_cap)
{
    const mock_file_t *f = find_file(path);
    if ((f == NULL) || (out == NULL) || (out_cap == 0U))
    {
        return false;
    }
    if (f->size >= out_cap)
    {
        return false;
    }
    memcpy(out, f->data, f->size);
    out[f->size] = '\0';
    return true;
}

bool osal_file_mock_has_file(const char *path)
{
    return find_file(path) != NULL;
}

int osal_file_mock_file_count(void)
{
    int count = 0;
    for (int i = 0; i < OSAL_FILE_MOCK_MAX_FILES; ++i)
    {
        count += s_files[i].in_use ? 1 : 0;
    }
    return count;
}

void osal_file_mock_fail_write_after(int32_t total_bytes_written)
{
    s_write_fail_after = total_bytes_written;
    s_write_total = 0;
}

void osal_file_mock_set_rename_status(int32_t status)
{
    s_rename_status = status; /* consumed by the next osal_rename() call */
}

void osal_file_mock_fail_rename_at(int call_number, int32_t status)
{
    if (call_number <= 0)
    {
        s_rename_fail_at = 0;
        s_rename_fail_at_status = OSAL_SUCCESS;
        return;
    }
    /* The injection fires when the 1-based rename call counter reaches
     * @p call_number; 0-based target = call_number - 1. */
    s_rename_fail_at        = call_number - 1;
    s_rename_fail_at_status = status;
}

void osal_file_mock_set_open_status(const char *path, int32_t status)
{
    if (path == NULL)
    {
        s_open_fail_path[0] = '\0';
        s_open_fail_status = OSAL_SUCCESS;
        return;
    }

    const size_t len = strlen(path);
    if (len >= sizeof(s_open_fail_path))
    {
        return; /* Path longer than the double's storage: cannot inject. */
    }

    memcpy(s_open_fail_path, path, len + 1U);
    s_open_fail_status = status;
}

void osal_file_mock_set_stat_status(int32_t status)
{
    s_stat_status = status; /* consumed by the next osal_stat() call */
}

bool osal_file_mock_add_file_raw(const char *path, const void *data,
                                 size_t len)
{
    if ((path == NULL) || (data == NULL) || (len == 0U))
    {
        return false;
    }
    if (len > (size_t)OSAL_FILE_MOCK_MAX_FILE_SIZE)
    {
        return false;
    }

    mock_file_t *f = find_file(path);
    if (f == NULL)
    {
        for (int i = 0; i < OSAL_FILE_MOCK_MAX_FILES; ++i)
        {
            if (!s_files[i].in_use)
            {
                f = &s_files[i];
                break;
            }
        }
        if (f == NULL)
        {
            return false;
        }
        f->in_use = true;
        strncpy(f->path, path, sizeof(f->path) - 1U);
        f->path[sizeof(f->path) - 1U] = '\0';
        f->size = 0U;
    }

    memcpy(f->data, data, len);
    f->size = len;
    return true;
}

void osal_file_mock_fail_next_cp(bool fail)
{
    s_fail_next_cp = fail;
}

void osal_file_mock_set_remove_status(int32_t status)
{
    s_remove_status = status;
}

int osal_file_mock_rename_calls(void)
{
    return s_rename_calls;
}

int osal_file_mock_cp_calls(void)
{
    return s_cp_calls;
}

int osal_file_mock_remove_calls(void)
{
    return s_remove_calls;
}

/* --------------------------------------------------------------------- */
/* OSAL contract doubles                                                  */
/* --------------------------------------------------------------------- */

osal_file_id_t osal_open_create(const char *path, osal_file_flag_t flags,
                                os_file_access_t access_mode)
{
    if (path == NULL)
    {
        return (osal_file_id_t)OSAL_INVALID_POINTER;
    }
    if (s_open_count >= OSAL_FILE_MOCK_MAX_FILES)
    {
        return (osal_file_id_t)OSAL_ERR_NO_FREE_IDS;
    }

    /* Persistent open-failure injection (transient unreadable file). */
    if ((s_open_fail_status != OSAL_SUCCESS) &&
        (strcmp(path, s_open_fail_path) == 0))
    {
        return (osal_file_id_t)s_open_fail_status;
    }

    mock_file_t *f = find_file(path);
    const bool create = (flags & OSAL_FILE_FLAG_CREATE) != 0U;

    if (f == NULL)
    {
        if (!create)
        {
            return (osal_file_id_t)OSAL_ERR_NAME_NOT_FOUND;
        }
        for (int i = 0; i < OSAL_FILE_MOCK_MAX_FILES; ++i)
        {
            if (!s_files[i].in_use)
            {
                f = &s_files[i];
                break;
            }
        }
        if (f == NULL)
        {
            return (osal_file_id_t)OSAL_ERR_NO_FREE_IDS;
        }
        f->in_use = true;
        strncpy(f->path, path, sizeof(f->path) - 1U);
        f->path[sizeof(f->path) - 1U] = '\0';
        f->size = 0U;
    }
    else if ((flags & OSAL_FILE_FLAG_TRUNCATE) != 0U)
    {
        f->size = 0U;
    }

    int slot = -1;
    for (int i = 0; i < OSAL_FILE_MOCK_MAX_FILES; ++i)
    {
        if (!s_open_in_use[i])
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        return (osal_file_id_t)OSAL_ERR_NO_FREE_IDS;
    }

    s_open_in_use[slot] = true;
    s_open_slot[slot] = (int)(f - s_files);
    s_open_writable[slot] =
        (access_mode == OSAL_WRITE_ONLY) || (access_mode == OSAL_READ_WRITE);
    /* Read positions always start at 0; a non-truncating write continues
     * at EOF (append), like a POSIX fd opened without O_TRUNC. */
    s_open_offset[slot] =
        (((flags & OSAL_FILE_FLAG_TRUNCATE) == 0U) && s_open_writable[slot])
            ? f->size
            : 0U;
    ++s_open_count;

    return (osal_file_id_t)(slot + 1);
}

int32_t osal_close(osal_file_id_t filedes)
{
    const int slot = (int)filedes - 1;
    if ((slot < 0) || (slot >= OSAL_FILE_MOCK_MAX_FILES) ||
        !s_open_in_use[slot])
    {
        return OSAL_ERR_INVALID_ID;
    }

    s_open_in_use[slot] = false;
    --s_open_count;
    return OSAL_SUCCESS;
}

int32_t osal_read(osal_file_id_t filedes, void *buffer, size_t nbytes)
{
    if (buffer == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (nbytes == 0U)
    {
        return OSAL_ERR_INVALID_SIZE;
    }

    const int slot = (int)filedes - 1;
    if ((slot < 0) || (slot >= OSAL_FILE_MOCK_MAX_FILES) ||
        !s_open_in_use[slot])
    {
        return OSAL_ERR_INVALID_ID;
    }

    const mock_file_t *f = &s_files[s_open_slot[slot]];
    if (s_open_offset[slot] >= f->size)
    {
        return 0; /* EOF */
    }

    size_t available = f->size - s_open_offset[slot];
    if (available > nbytes)
    {
        available = nbytes;
    }

    memcpy(buffer, f->data + s_open_offset[slot], available);
    s_open_offset[slot] += available;
    return (int32_t)available;
}

int32_t osal_write(osal_file_id_t filedes, const void *buffer, size_t nbytes)
{
    if (buffer == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (nbytes == 0U)
    {
        return OSAL_ERR_INVALID_SIZE;
    }

    const int slot = (int)filedes - 1;
    if ((slot < 0) || (slot >= OSAL_FILE_MOCK_MAX_FILES) ||
        !s_open_in_use[slot])
    {
        return OSAL_ERR_INVALID_ID;
    }
    if (!s_open_writable[slot])
    {
        return OSAL_ERR_FILE;
    }

    /* Fault injection: interrupted write after N cumulative bytes.  The
     * bytes written so far stay in the file (truncated-file model). */
    if ((s_write_fail_after >= 0) &&
        ((s_write_total + (int32_t)nbytes) > s_write_fail_after))
    {
        const int32_t permitted = s_write_fail_after - s_write_total;
        s_write_total = s_write_fail_after;
        if (permitted > 0)
        {
            mock_file_t *f = &s_files[s_open_slot[slot]];
            size_t room = (size_t)permitted;
            if (room > ((size_t)OSAL_FILE_MOCK_MAX_FILE_SIZE -
                        s_open_offset[slot]))
            {
                room = (size_t)OSAL_FILE_MOCK_MAX_FILE_SIZE -
                       s_open_offset[slot];
            }
            memcpy(f->data + s_open_offset[slot], buffer, room);
            if ((s_open_offset[slot] + room) > f->size)
            {
                f->size = s_open_offset[slot] + room;
            }
            s_open_offset[slot] += room;
        }
        return OSAL_ERR_FILE;
    }

    mock_file_t *f = &s_files[s_open_slot[slot]];

    if ((s_open_offset[slot] + nbytes) >
        (size_t)OSAL_FILE_MOCK_MAX_FILE_SIZE)
    {
        return OSAL_ERR_OUTPUT_TOO_LARGE;
    }

    memcpy(f->data + s_open_offset[slot], buffer, nbytes);
    s_open_offset[slot] += nbytes;
    if (s_open_offset[slot] > f->size)
    {
        f->size = s_open_offset[slot];
    }
    s_write_total += (int32_t)nbytes;

    return (int32_t)nbytes;
}

int32_t osal_stat(const char *path, osal_fstat_t *filestats)
{
    if ((path == NULL) || (filestats == NULL))
    {
        return OSAL_INVALID_POINTER;
    }

    /* One-shot stat-failure injection (transient storage error). */
    const int32_t injected = s_stat_status;
    s_stat_status = OSAL_SUCCESS;
    if (injected != OSAL_SUCCESS)
    {
        return injected;
    }

    const mock_file_t *f = find_file(path);
    if (f == NULL)
    {
        return OSAL_ERR_NAME_NOT_FOUND;
    }

    filestats->file_mode_bits =
        OSAL_FILESTAT_MODE_READ | OSAL_FILESTAT_MODE_WRITE;
    filestats->file_size = f->size;
    filestats->file_time.tv_sec = 0;
    filestats->file_time.tv_nsec = 0;
    return OSAL_SUCCESS;
}

int32_t osal_remove(const char *path)
{
    ++s_remove_calls;

    if (path == NULL)
    {
        return OSAL_INVALID_POINTER;
    }

    const int32_t injected = s_remove_status;
    s_remove_status = OSAL_SUCCESS;
    if (injected != OSAL_SUCCESS)
    {
        return injected;
    }

    mock_file_t *f = find_file(path);
    if (f == NULL)
    {
        return OSAL_ERR_NAME_NOT_FOUND;
    }

    f->in_use = false;
    f->path[0] = '\0';
    f->size = 0U;
    return OSAL_SUCCESS;
}

int32_t osal_rename(const char *old_filename, const char *new_filename)
{
    ++s_rename_calls;

    if ((old_filename == NULL) || (new_filename == NULL))
    {
        return OSAL_INVALID_POINTER;
    }

    const int32_t injected = s_rename_status;
    s_rename_status = OSAL_SUCCESS; /* one-shot injection */
    if (injected != OSAL_SUCCESS)
    {
        return injected;
    }

    /* Call-number injection: fires exactly once, on the configured call. */
    if ((s_rename_fail_at != 0) && (s_rename_calls == s_rename_fail_at + 1))
    {
        s_rename_fail_at        = 0;
        const int32_t at_status = s_rename_fail_at_status;
        s_rename_fail_at_status = OSAL_SUCCESS;
        if (at_status != OSAL_SUCCESS)
        {
            return at_status;
        }
    }

    mock_file_t *f = find_file(old_filename);
    if (f == NULL)
    {
        return OSAL_ERR_NAME_NOT_FOUND;
    }

    mock_file_t *target = find_file(new_filename);
    if (target == NULL)
    {
        for (int i = 0; i < OSAL_FILE_MOCK_MAX_FILES; ++i)
        {
            if (!s_files[i].in_use)
            {
                target = &s_files[i];
                break;
            }
        }
        if (target == NULL)
        {
            return OSAL_ERR_NO_FREE_IDS;
        }
        target->in_use = true;
        strncpy(target->path, new_filename, sizeof(target->path) - 1U);
        target->path[sizeof(target->path) - 1U] = '\0';
    }

    memcpy(target->data, f->data, f->size);
    target->size = f->size;

    f->in_use = false;
    f->path[0] = '\0';
    f->size = 0U;
    return OSAL_SUCCESS;
}

int32_t osal_cp(const char *src, const char *dest)
{
    ++s_cp_calls;

    if ((src == NULL) || (dest == NULL))
    {
        return OSAL_INVALID_POINTER;
    }

    osal_file_id_t src_fd =
        osal_open_create(src, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
    if (src_fd < 0)
    {
        return src_fd;
    }

    osal_file_id_t dst_fd = osal_open_create(
        dest,
        (osal_file_flag_t)(OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE),
        OSAL_WRITE_ONLY);
    if (dst_fd < 0)
    {
        (void)osal_close(src_fd);
        return dst_fd;
    }

    int32_t status = OSAL_SUCCESS;
    char buf[512];

    while (true)
    {
        const int32_t nread = osal_read(src_fd, buf, sizeof(buf));
        if (nread < 0)
        {
            status = nread;
            break;
        }
        if (nread == 0)
        {
            break;
        }

        size_t offset = 0U;
        while (offset < (size_t)nread)
        {
            const int32_t nwritten =
                osal_write(dst_fd, buf + offset, (size_t)nread - offset);
            if (nwritten < 0)
            {
                status = nwritten;
                break;
            }
            offset += (size_t)nwritten;
        }

        if (status != OSAL_SUCCESS)
        {
            break;
        }
    }

    (void)osal_close(src_fd);
    (void)osal_close(dst_fd);

    if ((status == OSAL_SUCCESS) && s_fail_next_cp)
    {
        /* One-shot mid-copy failure: destination stays partial. */
        s_fail_next_cp = false;
        return OSAL_ERR_FILE;
    }
    s_fail_next_cp = false;

    return status;
}

