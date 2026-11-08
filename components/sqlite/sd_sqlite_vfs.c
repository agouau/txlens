// Minimal SQLite VFS for ESP-IDF FATFS. A single SD worker owns every
// connection and closes it before unmounting; no other task opens this DB.
// Disk rollback journals and fsync are required, including for recovery.
#include "sqlite3.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef _WIN32 // Host regression tests exercise the same VFS against real files.
#include <io.h>
#define open _open
#define close _close
#define read _read
#define write _write
#define lseek _lseek
#define fsync _commit
#define fstat _fstat
#define stat _stat
#define ftruncate _chsize_s
#else
#include <unistd.h>
#endif

typedef struct {
    sqlite3_file base;
    int fd;
} sd_file_t;

static int sd_close(sqlite3_file *file)
{
    return close(((sd_file_t *)file)->fd) == 0 ? SQLITE_OK : SQLITE_IOERR_CLOSE;
}

static int sd_read(sqlite3_file *file, void *data, int size, sqlite3_int64 offset)
{
    int fd = ((sd_file_t *)file)->fd;
    if (offset < 0 || offset > INT_MAX)
        return SQLITE_IOERR_SEEK;
    // FATFS f_lseek grows writable files when seeking past EOF. SQLite probes
    // offset 24 even in an empty DB: a read must never turn it into a 24-byte
    // invalid database. Return SQLite's zero-filled short read before seeking.
    struct stat info;
    if (fstat(fd, &info) != 0) return SQLITE_IOERR_FSTAT;
    if (offset >= info.st_size) {
        memset(data, 0, size);
        return SQLITE_IOERR_SHORT_READ;
    }
    if (lseek(fd, (long)offset, SEEK_SET) != offset) return SQLITE_IOERR_SEEK;
    int done = 0;
    while (done < size) {
        int n = (int)read(fd, (char *)data + done, (unsigned)(size - done));
        if (n < 0) return SQLITE_IOERR_READ;
        if (n == 0) {
            memset((char *)data + done, 0, size - done);
            return SQLITE_IOERR_SHORT_READ;
        }
        done += n;
    }
    return SQLITE_OK;
}

static int sd_write(sqlite3_file *file, const void *data, int size, sqlite3_int64 offset)
{
    int fd = ((sd_file_t *)file)->fd;
    if (offset < 0 || offset > INT_MAX || lseek(fd, (long)offset, SEEK_SET) < 0)
        return SQLITE_IOERR_SEEK;
    int done = 0;
    while (done < size) {
        int n = (int)write(fd, (const char *)data + done, (unsigned)(size - done));
        if (n <= 0) return errno == ENOSPC ? SQLITE_FULL : SQLITE_IOERR_WRITE;
        done += n;
    }
    return SQLITE_OK;
}

static int sd_truncate(sqlite3_file *file, sqlite3_int64 size)
{
    if (size < 0 || size > INT_MAX) return SQLITE_IOERR_TRUNCATE;
    return ftruncate(((sd_file_t *)file)->fd, (long)size) == 0 ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
}

static int sd_sync(sqlite3_file *file, int flags)
{
    (void)flags;
    return fsync(((sd_file_t *)file)->fd) == 0 ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

static int sd_size(sqlite3_file *file, sqlite3_int64 *size)
{
    struct stat info;
    if (fstat(((sd_file_t *)file)->fd, &info) != 0) return SQLITE_IOERR_FSTAT;
    *size = info.st_size;
    return SQLITE_OK;
}

static int sd_lock(sqlite3_file *file, int lock)
{
    (void)file; (void)lock;
    return SQLITE_OK; // Single connection, serialized by the SD worker.
}

static int sd_reserved(sqlite3_file *file, int *reserved)
{
    (void)file;
    *reserved = 0; // No competing connection; allows hot-journal recovery.
    return SQLITE_OK;
}

static int sd_control(sqlite3_file *file, int operation, void *arg)
{
    (void)file; (void)operation; (void)arg;
    return SQLITE_NOTFOUND;
}

static int sd_sector(sqlite3_file *file) { (void)file; return 4096; }
static int sd_characteristics(sqlite3_file *file) { (void)file; return 0; }
static const sqlite3_io_methods s_methods = {
    .iVersion = 1, .xClose = sd_close, .xRead = sd_read, .xWrite = sd_write,
    .xTruncate = sd_truncate, .xSync = sd_sync, .xFileSize = sd_size,
    .xLock = sd_lock, .xUnlock = sd_lock, .xCheckReservedLock = sd_reserved,
    .xFileControl = sd_control, .xSectorSize = sd_sector,
    .xDeviceCharacteristics = sd_characteristics,
};

static int sd_open(sqlite3_vfs *vfs, const char *path, sqlite3_file *file, int flags, int *out)
{
    (void)vfs;
    file->pMethods = NULL;
    if (!path || (flags & SQLITE_OPEN_DELETEONCLOSE)) return SQLITE_CANTOPEN;
    int mode = flags & SQLITE_OPEN_READONLY ? O_RDONLY : O_RDWR;
    if (flags & SQLITE_OPEN_CREATE) mode |= O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE) mode |= O_EXCL;
#ifdef _WIN32
    mode |= O_BINARY;
#endif
    int fd = open(path, mode, 0666);
    if (fd < 0) return SQLITE_CANTOPEN;
    ((sd_file_t *)file)->fd = fd;
    file->pMethods = &s_methods;
    if (out) *out = flags;
    return SQLITE_OK;
}

static int sd_delete(sqlite3_vfs *vfs, const char *path, int sync_dir)
{
    (void)vfs; (void)sync_dir; // FATFS unlink synchronizes directory metadata.
    if (remove(path) == 0 || errno == ENOENT) return SQLITE_OK;
    return SQLITE_IOERR_DELETE;
}

static int sd_access(sqlite3_vfs *vfs, const char *path, int flags, int *exists)
{
    (void)vfs; (void)flags;
    struct stat info;
    *exists = stat(path, &info) == 0;
    return *exists || errno == ENOENT ? SQLITE_OK : SQLITE_IOERR_ACCESS;
}

static int sd_path(sqlite3_vfs *vfs, const char *path, int size, char *out)
{
    (void)vfs;
    int n = snprintf(out, size, "%s", path);
    return n >= 0 && n < size ? SQLITE_OK : SQLITE_CANTOPEN;
}

static int sd_random(sqlite3_vfs *vfs, int size, char *out)
{
    (void)vfs;
    esp_fill_random(out, size);
    return size;
}

static int sd_sleep(sqlite3_vfs *vfs, int us)
{
    (void)vfs;
    vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000));
    return us;
}

static int sd_time(sqlite3_vfs *vfs, double *out)
{
    (void)vfs;
    *out = time(NULL) / 86400.0 + 2440587.5;
    return SQLITE_OK;
}

static sqlite3_vfs s_vfs = {
    .iVersion = 1, .szOsFile = sizeof(sd_file_t), .mxPathname = 256,
    .zName = "sd", .xOpen = sd_open, .xDelete = sd_delete,
    .xAccess = sd_access, .xFullPathname = sd_path, .xRandomness = sd_random,
    .xSleep = sd_sleep, .xCurrentTime = sd_time,
};

int sqlite3_os_init(void) { return sqlite3_vfs_register(&s_vfs, 1); }
int sqlite3_os_end(void) { return SQLITE_OK; }
