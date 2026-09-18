/**
 * @file wifi_storage_osal_support.c
 * @brief Test-only OSAL doubles for the wifi_ap.json overwrite host test
 *        (TASK-136) — see wifi_storage_osal_support.h.
 */

#include "wifi_storage_osal_support.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "osal_error.h"
#include "osal_file.h"

/* --------------------------------------------------------------------- */
/* Logical storage mount                                                 */
/* --------------------------------------------------------------------- */

/* Internal host path buffer: larger than OSAL_MAX_PATH_LEN so the storage
 * root plus a maximum-length logical path always fits. */
#define WIFI_STORAGE_HOST_PATH_MAX 512u

static char    s_root[WIFI_STORAGE_HOST_PATH_MAX];
static bool    s_mounted;

/**
 * @brief Map a logical OSAL path onto the host-side storage root.
 *
 * Mirror of the ESP OSAL back-end (osal_lfs_build_vfs_path):
 *   - "wifi_ap.json"   -> <root>/wifi_ap.json
 *   - "/config/foo"    -> <root>/config/foo
 *   - "/littlefs/foo"  -> <root>/littlefs/foo (treated as logical)
 *
 * @param[in]  in       Logical path.
 * @param[out] out      Host path buffer.
 * @param[in]  out_size Capacity of @p out.
 * @return OSAL_SUCCESS or an OSAL error code.
 */
static int32_t build_host_path(const char *in, char *out, size_t out_size)
{
    const char *stripped;
    int         n;

    if (in == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (in[0] == '\0')
    {
        return OSAL_FS_ERR_PATH_INVALID;
    }
    if (!s_mounted)
    {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }

    /* Consume any leading '/' (a "/littlefs/..." logical path resolves
     * under the storage root exactly like any other logical path). */
    stripped = in;
    while (*stripped == '/')
    {
        ++stripped;
    }
    if (*stripped == '\0')
    {
        stripped = ".";
    }

    n = snprintf(out, out_size, "%s/%s", s_root, stripped);
    if (n < 0 || (size_t)n >= out_size)
    {
        return OSAL_FS_ERR_PATH_TOO_LONG;
    }
    return OSAL_SUCCESS;
}

bool wifi_storage_test_mount(void)
{
    const char *pattern = "/tmp/klc_wifi_storage_XXXXXX";
    size_t      len;
    char      * dir;

    wifi_storage_test_unmount();

    len = strlen(pattern) + 1U;
    dir = (char *)malloc(len);
    if (dir == NULL)
    {
        return false;
    }
    memcpy(dir, pattern, len);

    if (mkdtemp(dir) == NULL)
    {
        free(dir);
        return false;
    }

    strncpy(s_root, dir, sizeof(s_root) - 1U);
    s_root[sizeof(s_root) - 1U] = '\0';
    free(dir);
    s_mounted = true;
    return true;
}

void wifi_storage_test_unmount(void)
{
    /* Remove every file below the storage root, then the root itself. */
    if (s_root[0] != '\0')
    {
        char cmd[WIFI_STORAGE_HOST_PATH_MAX + 64U];
        int  n = snprintf(cmd, sizeof(cmd), "chmod -R u+rwx '%s' >/dev/null "
                                           "2>&1; rm -rf -- '%s'",
                          s_root, s_root);
        if (n > 0 && (size_t)n < sizeof(cmd))
        {
            (void)system(cmd);
        }
        s_root[0] = '\0';
    }
    s_mounted = false;
}

const char *wifi_storage_test_mapped_path(const char *logical_path)
{
    static char mapped[WIFI_STORAGE_HOST_PATH_MAX];

    mapped[0] = '\0';
    if (build_host_path(logical_path, mapped, sizeof(mapped)) != OSAL_SUCCESS)
    {
        return "";
    }
    return mapped;
}

long wifi_storage_test_read_file(const char *logical_path, char *buf,
                                 size_t cap)
{
    char    host[WIFI_STORAGE_HOST_PATH_MAX];
    int32_t rc;
    FILE  * fh;
    size_t  nread = 0U;
    int     c;

    if (buf == NULL || cap == 0U)
    {
        return -1L;
    }
    rc = build_host_path(logical_path, host, sizeof(host));
    if (rc != OSAL_SUCCESS)
    {
        return -1L;
    }

    fh = fopen(host, "rb");
    if (fh == NULL)
    {
        return -1L;
    }
    while (nread + 1U < cap)
    {
        c = fgetc(fh);
        if (c == EOF)
        {
            break;
        }
        buf[nread++] = (char)c;
    }
    buf[nread] = '\0';
    (void)fclose(fh);
    return (long)nread;
}

/* --------------------------------------------------------------------- */
/* Captured log                                                           */
/* --------------------------------------------------------------------- */

#define LOG_CAP_BYTES 8192u

static char   s_log[LOG_CAP_BYTES];
static size_t s_log_len;

void wifi_storage_test_log_reset(void)
{
    memset(s_log, 0, sizeof(s_log));
    s_log_len = 0U;
}

const char *wifi_storage_test_log_get(void)
{
    return s_log;
}

bool wifi_storage_test_log_contains(const char *needle)
{
    if (needle == NULL || needle[0] == '\0')
    {
        return false;
    }
    return strstr(s_log, needle) != NULL;
}

void osal_log_printf(const char *level, const char *format, ...)
{
    va_list args;
    char    line[512];
    int     n;

    n = snprintf(line, sizeof(line), "[%s]: ", level);
    if ((n <= 0) || ((size_t)n >= sizeof(line)))
    {
        return;
    }

    va_start(args, format);
    {
        int body = vsnprintf(line + n, sizeof(line) - (size_t)n, format, args);
        if (body < 0)
        {
            va_end(args);
            return;
        }
    }
    va_end(args);

    {
        size_t written = strlen(line);
        if (written >= (LOG_CAP_BYTES - 1U))
        {
            /* Drop overlong lines rather than truncating them. */
            return;
        }
        if ((s_log_len + written + 1U) > LOG_CAP_BYTES)
        {
            /* Ring overflow: drop the oldest half. */
            size_t keep = s_log_len - (s_log_len / 2U);
            memmove(s_log, s_log + (s_log_len - keep), keep);
            s_log_len = keep;
        }
        memcpy(s_log + s_log_len, line, written);
        s_log_len += written;
        s_log[s_log_len] = '\0';
    }
}

/* --------------------------------------------------------------------- */
/* OSAL file API (host, real filesystem)                                 */
/* --------------------------------------------------------------------- */

static int access_to_posix(os_file_access_t access_mode)
{
    switch (access_mode)
    {
        case OSAL_WRITE_ONLY:
            return O_WRONLY;
        case OSAL_READ_WRITE:
            return O_RDWR;
        case OSAL_READ_ONLY:
        default:
            return O_RDONLY;
    }
}

osal_file_id_t osal_open_create(const char *path, osal_file_flag_t flags,
                                os_file_access_t access_mode)
{
    char    host[WIFI_STORAGE_HOST_PATH_MAX];
    int32_t rc;
    int     posix_flags;
    int     fd;

    rc = build_host_path(path, host, sizeof(host));
    if (rc != OSAL_SUCCESS)
    {
        return (osal_file_id_t)rc;
    }

    posix_flags = access_to_posix(access_mode);
    if (flags & OSAL_FILE_FLAG_CREATE)
    {
        posix_flags |= O_CREAT;
    }
    if (flags & OSAL_FILE_FLAG_TRUNCATE)
    {
        posix_flags |= O_TRUNC;
    }

    fd = open(host, posix_flags, 0664);
    if (fd < 0)
    {
        return (osal_file_id_t)OSAL_ERROR;
    }
    return (osal_file_id_t)fd;
}

int32_t osal_close(osal_file_id_t filedes)
{
    if (filedes < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }
    if (close((int)filedes) != 0)
    {
        return OSAL_ERR_INVALID_ID;
    }
    return OSAL_SUCCESS;
}

int32_t osal_read(osal_file_id_t filedes, void *buffer, size_t nbytes)
{
    ssize_t rc;

    if (buffer == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (nbytes == 0U)
    {
        return OSAL_ERR_INVALID_SIZE;
    }
    if (filedes < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    rc = read((int)filedes, buffer, nbytes);
    if (rc < 0)
    {
        return OSAL_ERROR;
    }
    return (int32_t)rc;
}

int32_t osal_write(osal_file_id_t filedes, const void *buffer, size_t nbytes)
{
    ssize_t rc;

    if (buffer == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    if (nbytes == 0U)
    {
        return OSAL_ERR_INVALID_SIZE;
    }
    if (filedes < 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    rc = write((int)filedes, buffer, nbytes);
    if (rc < 0)
    {
        return OSAL_ERROR;
    }
    return (int32_t)rc;
}

int32_t osal_stat(const char *path, osal_fstat_t *filestats)
{
    char           host[WIFI_STORAGE_HOST_PATH_MAX];
    int32_t        rc;
    struct stat    st;

    if (filestats == NULL)
    {
        return OSAL_INVALID_POINTER;
    }
    memset(filestats, 0, sizeof(*filestats));

    rc = build_host_path(path, host, sizeof(host));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    if (stat(host, &st) != 0)
    {
        return OSAL_FS_ERR_PATH_INVALID;
    }

    filestats->file_size = (size_t)st.st_size;
    if (S_ISDIR(st.st_mode))
    {
        filestats->file_mode_bits = OSAL_FILESTAT_MODE_DIR;
    }
    else
    {
        filestats->file_mode_bits =
            OSAL_FILESTAT_MODE_READ | OSAL_FILESTAT_MODE_WRITE;
    }
    filestats->file_time.tv_sec = (int64_t)st.st_mtime;
    filestats->file_time.tv_nsec = 0;
    return OSAL_SUCCESS;
}

int32_t osal_remove(const char *path)
{
    char    host[WIFI_STORAGE_HOST_PATH_MAX];
    int32_t rc;

    rc = build_host_path(path, host, sizeof(host));
    if (rc != OSAL_SUCCESS)
    {
        return rc;
    }

    if (unlink(host) != 0)
    {
        return (errno == ENOENT) ? OSAL_FS_ERR_PATH_INVALID : OSAL_ERROR;
    }
    return OSAL_SUCCESS;
}